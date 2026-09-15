#include "cella/db/exec/executor.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include "cella/db/auth/auth_store.h"
#include "cella/db/common/db_logger.h"
#include "cella/db/common/value_bridge.h"
#include "cella/db/exec/expr_eval.h"
#include "cella/storage/buffer/buffer_pool_manager.h"
#include "cella/storage/common/status.h"
#include "cella/storage/index/b_plus_tree.h"
#include "cella/storage/index/index_key.h"
#include "cella/storage/table/table_heap.h"

namespace cella::db
{
  // ── rowid 伪列：公共判定（会话层与执行器共用）─────────────────
  bool IsRowidName(const std::string &column)
  {
    return cella::cella_toUpper(column) == "ROWID";
  }

  namespace
  {
    // 表达式树里是否引用了 rowid
    bool ExprRefersRowid(const cella::CELLA_Expr *e)
    {
      if (e == nullptr)
        return false;
      if (e->kind == cella::CELLA_Expr::Kind::COLUMN_REF)
        return IsRowidName(e->column);
      return ExprRefersRowid(e->left.get()) || ExprRefersRowid(e->right.get()) ||
             ExprRefersRowid(e->child.get());
    }

    // 存储层错误码 → 整合层错误码（保留原始信息）
    DbStatus FromStorage(const storage::Status &s, const std::string &what)
    {
      if (s.ok())
      {
        return DbStatus::Ok();
      }
      DbCode code = DbCode::kStorageError;
      switch (s.code())
      {
      case storage::StatusCode::kTableNotFound:
        code = DbCode::kTableNotFound;
        break;
      case storage::StatusCode::kTableAlreadyExists:
        code = DbCode::kTableExists;
        break;
      case storage::StatusCode::kRecordTooLarge:
        code = DbCode::kRecordTooLarge;
        break;
      case storage::StatusCode::kTypeMismatch:
        code = DbCode::kTypeMismatch;
        break;
      case storage::StatusCode::kInvalidArgument:
        code = DbCode::kStorageError;
        break;
      default:
        code = DbCode::kStorageError;
        break;
      }
      return DbStatus::Error(code, what + " [存储: " + s.ToString() + "]");
    }

    // ── rowid 伪列：物理行标识 ⇄ 不透明整数 ──────────────────────
    // 编码 = (页号 << 16) | 槽号（页号 32 位、槽号 16 位，合起来在 int64 内不溢出）。
    // 语义边界：不透明（禁止算术）；UPDATE 是删旧+插新 → rowid 会变；
    // 删除后槽位可能被后续插入复用 → 陈旧 rowid 可能指向新行。
    // 因此客户端应在「同一持锁事务内 fetch → 改」，改完重新取一次 rowid。
    constexpr int kRowidSlotBits = 16;
    constexpr uint64_t kRowidSlotMask = 0xFFFFull;
    constexpr const char *kRowidName = "rowid";

    int64_t RowidOf(const storage::Rid &rid)
    {
      return (static_cast<int64_t>(rid.page_id) << kRowidSlotBits) |
             static_cast<int64_t>(rid.slot_id);
    }

    bool RidFromRowid(int64_t rowid, storage::Rid *out)
    {
      if (rowid < 0)
        return false;
      out->page_id = static_cast<storage::page_id_t>(static_cast<uint64_t>(rowid) >> kRowidSlotBits);
      out->slot_id = static_cast<storage::slot_id_t>(static_cast<uint64_t>(rowid) & kRowidSlotMask);
      return out->IsValid();
    }

    // 谓词是否恰为 `rowid = <非负整数字面量>`（两侧顺序不限）。
    // 命中时可按物理地址直达目标行，省掉全表扫描。
    bool TryRowidEqLiteral(const cella::CELLA_Expr *pred, int64_t *value)
    {
      if (pred == nullptr || pred->kind != cella::CELLA_Expr::Kind::BINARY ||
          pred->bop != cella::CELLA_Expr::BinOp::EQ || !pred->left || !pred->right)
      {
        return false;
      }
      const cella::CELLA_Expr *col = nullptr;
      const cella::CELLA_Expr *lit = nullptr;
      if (pred->left->kind == cella::CELLA_Expr::Kind::COLUMN_REF &&
          pred->right->kind == cella::CELLA_Expr::Kind::LITERAL)
      {
        col = pred->left.get();
        lit = pred->right.get();
      }
      else if (pred->right->kind == cella::CELLA_Expr::Kind::COLUMN_REF &&
               pred->left->kind == cella::CELLA_Expr::Kind::LITERAL)
      {
        col = pred->right.get();
        lit = pred->left.get();
      }
      else
      {
        return false;
      }
      if (!IsRowidName(col->column) || lit->lit != cella::CELLA_LiteralKind::NUMBER)
        return false;
      if (lit->text.find('.') != std::string::npos)
        return false; // 只接受整数
      errno = 0;
      const long long parsed = std::strtoll(lit->text.c_str(), nullptr, 10);
      if (errno != 0)
        return false;
      *value = static_cast<int64_t>(parsed);
      return true;
    }

    // 扫描用的字段列表（限定符 = 表名或别名）。
    // 仅当语句确实引用了 rowid 时才在末尾追加伪列：它（故意）不在编译器目录里，
    // 所以 `get *` 不会带出它；不引用时不追加，`get *` 的输出与改造前完全一致。
    std::vector<FieldRef> MakeFields(const CatalogTable &table, const std::string &qualifier,
                                     bool with_rowid)
    {
      std::vector<FieldRef> fields;
      fields.reserve(table.columns.size() + (with_rowid ? 1 : 0));
      for (const auto &c : table.columns)
      {
        FieldRef f;
        f.qualifier = qualifier;
        f.name = c.name;
        fields.push_back(std::move(f));
      }
      if (with_rowid)
      {
        FieldRef rid;
        rid.qualifier = qualifier;
        rid.name = kRowidName;
        fields.push_back(std::move(rid));
      }
      return fields;
    }

    // 供谓词求值的行值：物理列值（+ 引用 rowid 时末尾补 rowid），与 MakeFields 布局一致
    std::vector<storage::Value> ValuesWithRowid(const std::vector<storage::Value> &values,
                                                const storage::Rid &rid, bool with_rowid)
    {
      std::vector<storage::Value> out = values;
      if (with_rowid)
        out.push_back(storage::Value::BigInt(RowidOf(rid)));
      return out;
    }

    // 把一行拼成 "k1|k2|..." 便于去重（用渲染文本，避免引入哈希）
    std::string KeyOfRow(const std::vector<storage::Value> &row, const std::vector<int> &idx)
    {
      std::string key;
      for (int i : idx)
      {
        if (i < 0 || static_cast<size_t>(i) >= row.size())
        {
          key += "\x01";
          continue;
        }
        const storage::Value &v = row[static_cast<size_t>(i)];
        key += RenderValue(v);
        key += '\x01';
        key += (v.IsNull() ? 'N' : 'V');
        key += '\x02';
      }
      return key;
    }

    // 在 fields[0, limit) 范围内解析列引用：先「限定符 + 列名」精确匹配，
    // 失败则忽略限定符只按列名匹配 —— 计划里 Sort/Aggregate 位于 Project 之上，
    // 而 Project 已把限定符丢掉，此时按列名匹配是唯一可行且符合直觉的做法。
    int ResolveInRange(const std::vector<FieldRef> &fields, const cella::CELLA_ColName &key,
                       size_t limit)
    {
      const std::string want = cella::cella_toUpper(key.column);
      const std::string qual = cella::cella_toUpper(key.table);
      const size_t end = (limit < fields.size()) ? limit : fields.size();
      for (size_t i = 0; i < end; ++i)
      {
        if (cella::cella_toUpper(fields[i].name) != want)
        {
          continue;
        }
        if (!qual.empty() && cella::cella_toUpper(fields[i].qualifier) != qual)
        {
          continue;
        }
        return static_cast<int>(i);
      }
      if (!qual.empty())
      {
        for (size_t i = 0; i < end; ++i)
        {
          if (cella::cella_toUpper(fields[i].name) == want)
          {
            return static_cast<int>(i);
          }
        }
      }
      return -1;
    }

    int ResolveSortKey(const RowSet &rs, const cella::CELLA_ColName &key)
    {
      return ResolveInRange(rs.fields, key, rs.fields.size());
    }

    std::string KeyName(const cella::CELLA_ColName &key)
    {
      return key.table.empty() ? key.column : (key.table + "." + key.column);
    }

    // 存储访问串行化（存储层非线程安全）
    class StorageGuard
    {
    public:
      explicit StorageGuard(std::recursive_mutex *m) : lk_(*m) {}
      StorageGuard(const StorageGuard &) = delete;
      StorageGuard &operator=(const StorageGuard &) = delete;

    private:
      std::lock_guard<std::recursive_mutex> lk_;
    };

    // 两个值在索引语义下是否算「同一列值」。
    // NULL 与任何值都不相等（SQL 的 UNIQUE 语义：多个 NULL 允许共存），
    // 非 NULL 时用类型感知比较，避免 "1" 与 1 误判为相同。
    bool IndexValuesEqual(const storage::Value &a, const storage::Value &b)
    {
      if (a.IsNull() || b.IsNull())
      {
        return a.IsNull() && b.IsNull();
      }
      return ValueEquals(a, b);
    }

    // 标识符大小写不敏感比较（表名/列名在目录与计划里拼写可能不一致）
    bool SameIdent(const std::string &a, const std::string &b)
    {
      return cella::cella_toUpper(a) == cella::cella_toUpper(b);
    }

    // 把字面量表达式转成存储值。只接受能安全用于索引边界的类型：
    // 数值 / 字符串 / 布尔 / 日期文本。NULL 由调用方提前挡掉（区间边界不含 NULL）。
    bool LiteralToValue(const cella::CELLA_Expr &e, storage::Value *out)
    {
      using LK = cella::CELLA_LiteralKind;
      switch (e.lit)
      {
      case LK::NUMBER:
      {
        // 整数文本 → INT64，否则 → DOUBLE（与执行层既有的数值规约一致）。
        // 索引区间边界必须类型精确：把 20 编成 DOUBLE 会让「INT 列 = 20」
        // 的区间与叶子键的 INT 编码对不上，从而扫出空区间。
        if (e.text.find('.') == std::string::npos && e.text.find('e') == std::string::npos &&
            e.text.find('E') == std::string::npos)
        {
          errno = 0;
          const long long iv = std::strtoll(e.text.c_str(), nullptr, 10);
          if (errno == 0)
          {
            *out = storage::Value::BigInt(iv);
            return true;
          }
        }
        *out = storage::Value::Double(e.num);
        return true;
      }
      case LK::STRING:
        *out = storage::Value::Varchar(e.text);
        return true;
      case LK::BOOL_LIT:
        *out = storage::Value::Bool(e.boolVal);
        return true;
      case LK::DATE:
        *out = storage::Value::Varchar(e.text);
        return true;
      default:
        return false;
      }
    }

    // 代价的展示格式（固定 1 位小数，便于 golden 与人工阅读稳定）
    std::string FormatCost(double c)
    {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.1f", c);
      return std::string(buf);
    }

  } // namespace

  // 语句是否在任何位置引用了 rowid（投影 / 条件 / 分组 / 排序 / SET 表达式）
  bool StmtRefersRowid(const cella::CELLA_Stmt *st)
  {
    if (st == nullptr)
      return false;
    for (const auto &si : st->selectItems)
    {
      if (ExprRefersRowid(si.expr.get()))
        return true;
    }
    // 注意：GET 的条件存在 st->limit（方言里 limit = WHERE），
    // DELETE/UPDATE 的条件存在 st->where —— 两处都要看
    if (ExprRefersRowid(st->where.get()) || ExprRefersRowid(st->limit.get()) ||
        ExprRefersRowid(st->having.get()))
      return true;
    for (const auto &cn : st->grouped)
    {
      if (IsRowidName(cn.column))
        return true;
    }
    for (const auto &oi : st->ordered)
    {
      if (IsRowidName(oi.col.column))
        return true;
    }
    for (const auto &kv : st->sets)
    {
      if (ExprRefersRowid(kv.second.get()))
        return true;
    }
    return false;
  }

  Executor::Executor(storage::IStorage *storage, CatalogManager *catalog, TxnManager *txn_manager,
                     LockManager *locks, std::recursive_mutex *storage_mutex)
      : storage_(storage),
        catalog_(catalog),
        txn_manager_(txn_manager),
        locks_(locks),
        storage_mutex_(storage_mutex) {}

  // ═════════════════════════════════════════════════════════════
  // 语句级入口
  // ═════════════════════════════════════════════════════════════

  DbStatus Executor::Execute(const cella::CELLA_PlanNode &plan, const ExecContext &ctx,
                             QueryResult *out)
  {
    if (out == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "Execute: 输出为空");
    }
    out->Clear();
    ++operator_calls_;
    DbLogDebug(logcat::kExec, "执行算子 " + plan.op);

    if (plan.op == "CreateTable")
    {
      return ExecCreateTable(plan, ctx, out);
    }
    if (plan.op == "DropTable")
    {
      return ExecDropTable(plan, ctx, out);
    }
    if (plan.op == "CreateIndex")
    {
      return ExecCreateIndex(plan, ctx, out);
    }
    if (plan.op == "DropIndex")
    {
      return ExecDropIndex(plan, ctx, out);
    }
    if (plan.op == "AlterTable")
    {
      return ExecAlterTable(plan, ctx, out);
    }
    if (plan.op == "TruncateTable")
    {
      return ExecTruncateTable(plan, ctx, out);
    }
    if (plan.op == "Insert")
    {
      return ExecInsert(plan, ctx, out);
    }
    if (plan.op == "Delete")
    {
      return ExecDelete(plan, ctx, out);
    }
    if (plan.op == "Update")
    {
      return ExecUpdate(plan, ctx, out);
    }
    return ExecGet(plan, ctx, out);
  }

  // 表级权限判定。短路顺序：访问控制关 → 管理员 → 无需检查（DDL）→ 未绑定身份库 →
  // 系统表（只读放行，写已被 DB-512 拦）。命中即为「所有表访问的必经点」。
  DbStatus Executor::CheckTablePrivilege(const std::string &table, const ExecContext &ctx,
                                         TablePriv need)
  {
    if (!ctx.auth_enabled || ctx.is_admin || need == TablePriv::kNone || auth_ == nullptr)
    {
      return DbStatus::Ok();
    }
    if (CatalogManager::IsSystemTable(table))
    {
      return DbStatus::Ok();
    }
    Priv required = Priv::kGet;
    if (!TablePrivToPriv(need, &required))
    {
      return DbStatus::Ok();
    }
    if (auth_->HasPrivilege(ctx.user, ctx.lock_scope, table, required))
    {
      return DbStatus::Ok();
    }
    return DbStatus::Error(DbCode::kPermissionDenied,
                           "权限不足：用户 \"" + ctx.user + "\" 在 " + ctx.lock_scope + "." + table +
                               " 上没有 " + PrivName(required) + " 权限");
  }

  DbStatus Executor::LockTable(const std::string &table, LockMode mode, const ExecContext &ctx,
                               TablePriv need)
  {
    if (ctx.txn_id == kInvalidTxnId)
    {
      return DbStatus::Error(DbCode::kNoActiveTxn, "缺少事务上下文，无法加锁");
    }
    // 访问控制：表级读写在这个「所有表访问的必经点」上判定，新增算子也不会漏
    const DbStatus ps = CheckTablePrivilege(table, ctx, need);
    if (!ps.ok())
    {
      return ps;
    }
    // 锁资源带库前缀：同一路径里不会跨库（USE 在事务中被禁），
    // 前缀是防「引擎切换库后，别的会话残留的旧库锁」与新城同名表误撞。
    const std::string resource = ctx.lock_scope.empty() ? table : ctx.lock_scope + "." + table;
    const DbStatus s = locks_->Acquire(ctx.txn_id, resource, mode);
    if (s.ok() && ctx.recording())
    {
      ctx.txn->TouchTable(table); // undo/journal 记裸表名（回滚只在当前库内进行）
    }
    return s;
  }

  // ── CREATE TABLE ────────────────────────────────────────────

  DbStatus Executor::ExecCreateTable(const cella::CELLA_PlanNode &plan, const ExecContext &ctx,
                                     QueryResult *out)
  {
    const cella::CELLA_Stmt *st = plan.stmt;
    if (st == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "CreateTable 计划缺少语句信息");
    }
    if (catalog_->FindTable(st->tableName) != nullptr)
    {
      return DbStatus::Error(DbCode::kTableExists, "表已存在: " + st->tableName);
    }
    const DbStatus ls = LockTable(st->tableName, LockMode::kExclusive, ctx, TablePriv::kNone);
    if (!ls.ok())
    {
      return ls;
    }

    CatalogTable entry;
    entry.name = st->tableName;
    for (const auto &cd : st->columns)
    {
      CatalogColumn c;
      c.name = cd.name;
      c.type = cd.type;
      c.len = (cd.type == cella::CELLA_DataType::CHAR || cd.type == cella::CELLA_DataType::VARCHAR)
                  ? (cd.hasLen ? cd.len : 255)
                  : 0;
      c.not_null = cd.notNull || cd.primaryKey; // 主键隐含 NOT NULL
      c.primary_key = cd.primaryKey;
      entry.columns.push_back(c);
    }

    const storage::Schema schema = ToStorageSchema(entry);
    {
      StorageGuard guard(storage_mutex_);
      const storage::Status s = storage_->create_table(entry.name, schema);
      if (!s.ok())
      {
        return FromStorage(s, "建表 " + entry.name);
      }
      // 存储层不通过 IStorage 暴露首数据页号，用 open_table 的句柄补齐（仅诊断用途）
      std::shared_ptr<storage::TableHeap> heap;
      if (storage_->open_table(entry.name, &heap).ok() && heap)
      {
        entry.first_page_id = heap->first_page_id();
      }
    }

    const DbStatus rs = catalog_->RegisterTable(entry);
    if (!rs.ok())
    {
      StorageGuard guard(storage_mutex_);
      (void)storage_->drop_table(entry.name); // 元数据登记失败 → 撤销物理表
      return rs;
    }
    const CatalogTable *registered = catalog_->FindTable(entry.name);
    if (registered == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "建表后目录查找失败: " + entry.name);
    }
    {
      StorageGuard guard(storage_mutex_);
      const DbStatus ss = catalog_->WriteTableRow(*registered);
      if (!ss.ok())
      {
        // 目录行写失败 → 撤销物理表与内存条目，保证三者一致
        (void)catalog_->RemoveTable(entry.name);
        (void)storage_->drop_table(entry.name);
        return ss;
      }
    }

    // ── P1.4：主键自动索引 ──────────────────────────────────────
    // 主键列天然需要「唯一 + 高频等值查找」，是最值得建索引的列。建表时自动
    // 建一棵唯一 B+ 树，名字固定为 <table>_pk（写入 cella_index 系统表），
    // 因此元数据随目录一起持久化，重启后 Attach 到同一棵树上即可继续用。
    {
      const DbStatus ps = CreatePrimaryIndex(*registered);
      if (!ps.ok())
      {
        // 主键索引失败 → 回滚整张表的建立，避免留下「有表无主键索引」的半成品
        StorageGuard guard(storage_mutex_);
        (void)catalog_->DeleteIndexesOfTable(entry.name);
        (void)catalog_->RemoveTable(entry.name);
        (void)catalog_->DeleteTableRow(entry.name);
        (void)storage_->drop_table(entry.name);
        return ps;
      }
    }

    out->tag = "CREATE TABLE " + entry.name;
    DbLogInfo(logcat::kCatalog, out->tag + "（" + std::to_string(entry.columns.size()) + " 列）");
    return DbStatus::Ok();
  }

  // ── P1.4：为表的主键列建立唯一索引 ──────────────────────────
  // 表没有主键列时是空操作。索引名 = <table>_pk，业务语义上属于系统生成，
  // 但为了不改变 cella_index 的行格式（保持既有 6 列布局与 golden 输出），
  // 它作为一条**普通索引元数据**登记 —— 由此也自动获得：
  //   * 重启后随 LoadIndexesFromStorage 恢复
  //   * DROP TABLE 时被 DeleteIndexesOfTable 级联清理
  //   * DML 时被 ExecInsert/ExecUpdate/ExecDelete 的索引维护统一覆盖
  // 复合主键（表级 PRIMARY KEY (a, b)）同样建索引：键序 = 主键列**声明序**
  //（系统约定，见 CatalogTable::PrimaryKeyColumns）。B+ 树键 = 各主键列
  // 依次编码 + 行定位；唯一性由 IndexValueFree 的元组前缀比较保证。
  DbStatus Executor::CreatePrimaryIndex(const CatalogTable &table)
  {
    const std::vector<int> pk_cols = table.PrimaryKeyColumns();
    if (pk_cols.empty())
    {
      return DbStatus::Ok(); // 无主键 → 不建
    }
    if (storage_ == nullptr || catalog_ == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "执行器未绑定存储/目录，无法建主键索引");
    }

    // 幂等：同一张表重复调用（重放/迁移）不应建出第二棵
    const std::string index_name = PrimaryIndexName(table.name);
    if (catalog_->FindIndex(index_name) != nullptr)
    {
      return DbStatus::Ok();
    }

    // 键规格：按声明序逐列收集（类型 + 长度上限），并校验最坏键长放得进一页
    storage::BPlusTree::KeySpec spec;
    std::vector<std::string> pk_names;
    for (int ci : pk_cols)
    {
      const CatalogColumn &col = table.columns[static_cast<size_t>(ci)];
      spec.columns.push_back(storage::BPlusTree::Column{
          ToStorageType(col.type), static_cast<uint16_t>(col.len)});
      pk_names.push_back(col.name);
    }

    StorageGuard guard(storage_mutex_);
    storage::BufferPoolManager *bpm = storage_->buffer_pool();
    if (bpm == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "存储引擎未暴露缓冲池，无法建主键索引");
    }
    if (!storage::BPlusTree::KeyFitsPage(bpm->page_size(), spec))
    {
      return DbStatus::Error(DbCode::kValueTooLong,
                             "主键 " + JoinIndexColumns(pk_names) +
                                 " 的键编码过长，单页放不下最坏情况，拒绝建索引");
    }

    storage::BPlusTree tree(bpm, spec);
    storage::page_id_t root = storage::kInvalidPageId;
    const storage::Status cs = tree.Create(&root);
    if (!cs.ok())
    {
      return FromStorage(cs, "建主键索引 " + index_name);
    }

    // 建表时表是空的，无需回填；但为了对「已存在的表补建索引」也安全，
    // 这里仍走一遍全表回填（空表时循环体不执行）。
    std::shared_ptr<storage::TableHeap> heap;
    const storage::Status os = storage_->open_table(table.name, &heap);
    if (!os.ok())
    {
      return FromStorage(os, "回填主键索引时打开表 " + table.name);
    }
    size_t backfilled = 0;
    for (auto it = heap->begin(); it != heap->end(); ++it)
    {
      const storage::Record &rec = *it;
      std::vector<storage::Value> key_vals;
      bool bad = false;
      for (int ci : pk_cols)
      {
        if (static_cast<size_t>(ci) >= rec.value_count())
        {
          bad = true;
          break;
        }
        key_vals.push_back(rec.value(static_cast<size_t>(ci)));
      }
      if (bad)
      {
        continue;
      }
      const std::string leaf = storage::EncodeLeafKeyColumns(
          key_vals, it.rid().page_id, static_cast<uint8_t>(it.rid().slot_id));
      bool dup = false;
      const storage::Status is = tree.Insert(leaf, &dup);
      if (!is.ok())
      {
        return FromStorage(is, "回填主键索引 " + index_name);
      }
      ++backfilled;
    }

    CatalogIndex entry;
    entry.name = index_name;
    entry.table = table.name;
    entry.columns = pk_names;
    entry.column = entry.JoinedColumns();
    entry.unique = true; // 主键语义就是唯一
    entry.root_page_id = root;
    entry.created_at = static_cast<int64_t>(std::time(nullptr));
    const DbStatus ws = catalog_->WriteIndexRow(entry);
    if (!ws.ok())
    {
      return ws;
    }
    DbLogInfo(logcat::kCatalog, "已为主键 " + table.name + "(" + entry.column +
                                   ") 自动建索引 " + index_name +
                                   "（回填 " + std::to_string(backfilled) + " 行）");
    return DbStatus::Ok();
  }

  std::string Executor::PrimaryIndexName(const std::string &table)
  {
    return table + "_pk";
  }

  // 表的主键索引是否已存在（诊断/测试用）
  bool Executor::HasPrimaryIndex(const std::string &table) const
  {
    return catalog_ != nullptr && catalog_->FindIndex(PrimaryIndexName(table)) != nullptr;
  }

  // ── DROP TABLE ──────────────────────────────────────────────

  DbStatus Executor::ExecDropTable(const cella::CELLA_PlanNode &plan, const ExecContext &ctx,
                                   QueryResult *out)
  {
    const cella::CELLA_Stmt *st = plan.stmt;
    if (st == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "DropTable 计划缺少语句信息");
    }
    const CatalogTable *meta = catalog_->FindTable(st->tableName);
    if (meta == nullptr)
    {
      return DbStatus::Error(DbCode::kTableNotFound, "表不存在: " + st->tableName);
    }
    const std::string name = meta->name;
    if (CatalogManager::IsProtectedSystemTable(name))
    {
      return DbStatus::Error(DbCode::kSystemTableProtected, "系统表禁止删除: " + name);
    }
    const DbStatus ls = LockTable(name, LockMode::kExclusive, ctx, TablePriv::kNone);
    if (!ls.ok())
    {
      return ls;
    }
    {
      StorageGuard guard(storage_mutex_);
      const storage::Status s = storage_->drop_table(name);
      if (!s.ok())
      {
        return FromStorage(s, "删表 " + name);
      }
    }
    const DbStatus rs = catalog_->RemoveTable(name);
    if (!rs.ok())
    {
      return rs;
    }
    {
      StorageGuard guard(storage_mutex_);
      const DbStatus ss = catalog_->DeleteTableRow(name);
      if (!ss.ok())
      {
        return ss;
      }
      // 级联清理该表的所有二级索引（P1.2）。物理 B+ 树页随文件回收策略统一处理，
      // 元数据行必须删干净，否则重建库时会指向不存在的表。
      const std::vector<const CatalogIndex *> owned = catalog_->IndexesOfTable(name);
      if (!owned.empty())
      {
        const DbStatus is = catalog_->DeleteIndexesOfTable(name);
        if (!is.ok())
        {
          return is;
        }
        DbLogInfo(logcat::kCatalog,
                  "DROP TABLE " + name + " 级联删除 " + std::to_string(owned.size()) + " 个索引");
      }
    }
    out->tag = "DROP TABLE " + name;
    DbLogInfo(logcat::kCatalog, out->tag);
    return DbStatus::Ok();
  }

  // ── CREATE INDEX ────────────────────────────────────────────

  DbStatus Executor::ExecCreateIndex(const cella::CELLA_PlanNode &plan, const ExecContext &ctx,
                                     QueryResult *out)
  {
    const cella::CELLA_Stmt *st = plan.stmt;
    if (st == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "CreateIndex 计划缺少语句信息");
    }
    const CatalogTable *meta = catalog_->FindTable(st->tableName);
    if (meta == nullptr)
    {
      return DbStatus::Error(DbCode::kTableNotFound, "表不存在: " + st->tableName);
    }
    if (CatalogManager::IsProtectedSystemTable(meta->name))
    {
      return DbStatus::Error(DbCode::kSystemTableProtected, "系统表禁止建索引: " + meta->name);
    }
    if (catalog_->FindIndex(st->indexName) != nullptr)
    {
      return DbStatus::Error(DbCode::kIndexExists, "索引已存在: " + st->indexName);
    }
    // 复合索引：列清单（AST 里按声明序；兜底解析逗号拼接的 indexColumn）
    std::vector<std::string> index_cols = st->indexColumns;
    if (index_cols.empty())
    {
      index_cols = SplitIndexColumns(st->indexColumn);
    }
    if (index_cols.empty())
    {
      return DbStatus::Error(DbCode::kValueTooLong, "索引必须至少指定一列");
    }
    // 逐列校验存在性 + 收集键规格（声明序），同时校验最坏键长放得进一页
    storage::BPlusTree::KeySpec spec;
    std::vector<int> col_idxs;
    for (const std::string &cn : index_cols)
    {
      const CatalogColumn *col = meta->FindColumn(cn);
      if (col == nullptr)
      {
        return DbStatus::Error(DbCode::kColumnNotFound,
                               "列不存在: " + st->tableName + "." + cn);
      }
      spec.columns.push_back(storage::BPlusTree::Column{
          ToStorageType(col->type), static_cast<uint16_t>(col->len)});
      col_idxs.push_back(meta->ColumnIndex(col->name));
    }
    const DbStatus ls = LockTable(meta->name, LockMode::kShared, ctx, TablePriv::kNone);
    if (!ls.ok())
    {
      return ls;
    }

    std::unique_ptr<storage::BPlusTree> tree;
    {
      StorageGuard guard(storage_mutex_);
      storage::BufferPoolManager *bpm = storage_->buffer_pool();
      if (bpm == nullptr)
      {
        return DbStatus::Error(DbCode::kInternal, "存储引擎未暴露缓冲池，无法建索引");
      }
      // 键长上限必须在建索引时校验：复合键 = Σ(各列编码) + 位图 + 行定位，
      // 多个长 VARCHAR 组合可能撑爆一页（一页至少要能放 2~3 个键才能分裂）。
      if (!storage::BPlusTree::KeyFitsPage(bpm->page_size(), spec))
      {
        return DbStatus::Error(DbCode::kValueTooLong,
                               "索引 " + st->indexName + " 的键编码过长（列 " +
                                   JoinIndexColumns(index_cols) +
                                   "），单页放不下最坏情况，拒绝建索引");
      }
      tree = std::make_unique<storage::BPlusTree>(bpm, spec);
      storage::page_id_t root = storage::kInvalidPageId;
      const storage::Status cs = tree->Create(&root);
      if (!cs.ok())
      {
        return FromStorage(cs, "建索引 " + st->indexName);
      }

      // 回填：全表扫描现有行，逐行插入复合索引键
      std::shared_ptr<storage::TableHeap> heap;
      const storage::Status os = storage_->open_table(meta->name, &heap);
      if (!os.ok())
      {
        return FromStorage(os, "回填索引时打开表 " + meta->name);
      }
      size_t backfilled = 0;
      for (auto it = heap->begin(); it != heap->end(); ++it)
      {
        const storage::Record &rec = *it;
        std::vector<storage::Value> key_vals;
        bool bad = false;
        for (int ci : col_idxs)
        {
          if (ci < 0 || static_cast<size_t>(ci) >= rec.value_count())
          {
            bad = true;
            break;
          }
          key_vals.push_back(rec.value(static_cast<size_t>(ci)));
        }
        if (bad)
        {
          continue;
        }
        const std::string leaf = storage::EncodeLeafKeyColumns(
            key_vals, it.rid().page_id, static_cast<uint8_t>(it.rid().slot_id));
        bool dup = false;
        const storage::Status is = tree->Insert(leaf, &dup);
        if (!is.ok())
        {
          return FromStorage(is, "回填索引 " + st->indexName);
        }
        ++backfilled;
      }

      // 唯一性检查：同列值元组不同行（键不同但列值前缀相同）视为冲突。
      // 键按「元组编码 + 行定位」有序 → 相邻键去尾后相等即同元组。
      if (st->unique)
      {
        std::vector<std::string> all;
        const storage::Status ss = tree->ScanAll(&all);
        if (!ss.ok())
        {
          return FromStorage(ss, "校验唯一索引 " + st->indexName);
        }
        for (size_t i = 1; i < all.size(); ++i)
        {
          if (storage::StripLeafRowId(all[i - 1]).compare(storage::StripLeafRowId(all[i])) == 0)
          {
            return DbStatus::Error(DbCode::kIndexExists,
                                   "唯一索引 " + st->indexName + " 建立失败：列 (" +
                                       JoinIndexColumns(index_cols) + ") 存在重复值");
          }
        }
      }

      CatalogIndex entry;
      entry.name = st->indexName;
      entry.table = meta->name;
      entry.columns = index_cols;
      entry.column = entry.JoinedColumns();
      entry.unique = st->unique;
      entry.root_page_id = root;
      entry.created_at = static_cast<int64_t>(std::time(nullptr));
      const DbStatus ws = catalog_->WriteIndexRow(entry);
      if (!ws.ok())
      {
        return ws;
      }
      out->tag = "CREATE INDEX " + entry.name;
      DbLogInfo(logcat::kCatalog, out->tag + " on " + entry.table + "(" + entry.column +
                                        ")，回填 " + std::to_string(backfilled) + " 行");
    }
    return DbStatus::Ok();
  }

  // ── DROP INDEX ──────────────────────────────────────────────

  DbStatus Executor::ExecDropIndex(const cella::CELLA_PlanNode &plan, const ExecContext &ctx,
                                   QueryResult *out)
  {
    const cella::CELLA_Stmt *st = plan.stmt;
    if (st == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "DropIndex 计划缺少语句信息");
    }
    const CatalogIndex *meta = catalog_->FindIndex(st->indexName);
    if (meta == nullptr)
    {
      return DbStatus::Error(DbCode::kIndexNotFound, "索引不存在: " + st->indexName);
    }
    const std::string index_name = meta->name;
    const std::string table_name = meta->table;
    const DbStatus ls = LockTable(table_name, LockMode::kShared, ctx, TablePriv::kNone);
    if (!ls.ok())
    {
      return ls;
    }
    {
      StorageGuard guard(storage_mutex_);
      const DbStatus ds = catalog_->DeleteIndexRows(index_name);
      if (!ds.ok())
      {
        return ds;
      }
      // 物理 B+ 树页不在此处逐页回收（与 DROP TABLE 一致，交给文件级回收策略）。
      // 关键是元数据失效：后续查询不会再选到这条路径。
    }
    out->tag = "DROP INDEX " + index_name;
    DbLogInfo(logcat::kCatalog, out->tag);
    return DbStatus::Ok();
  }

  // ═════════════════════════════════════════════════════════════
  // ALTER TABLE / TRUNCATE TABLE（P5：DDL 演进能力）
  // ═════════════════════════════════════════════════════════════
  //
  // 统一套路：除 ADD/DROP PRIMARY KEY 之外，所有动作都要**重建物理表**。
  //
  //   为什么重建是必需的？
  //     行的二进制布局是 `列数 + NULL 位图 + 逐列值`，反序列化会拿 schema 的列数
  //     与字节流里的列数对账（slotted_record_serializer.cpp）。列集合一变，
  //     老字节流立刻解不出来；存储层也没有「原地改列」的能力。所以只能把老表
  //     整体导出、按新结构建表、再逐行回填。表改名同理（存储层没有 rename）。
  //
  //   代价与副作用：
  //     * O(表行数) 的读写 —— 与「改列必须重写每一行」的物理事实一致；
  //     * 所有行的定位（页号 + 槽位）都会变 → 表上的索引全部失效，必须重建
  //       （见 RebuildTableIndexes），否则索引会指向已不存在的行。
  //
  //   这些语句按 DDL 处理（会话层隐式提交前置事务 + 执行后强制存盘），
  //   与 CREATE/DROP TABLE 一致：元数据与物理表已经改了，回滚它们没有意义。

  size_t Executor::CountRowsOf(const std::string &table) const
  {
    if (storage_ == nullptr)
    {
      return 0;
    }
    std::shared_ptr<storage::TableHeap> heap;
    if (!storage_->open_table(table, &heap).ok() || !heap)
    {
      return 0;
    }
    size_t n = 0;
    for (auto it = heap->begin(); it != heap->end(); ++it)
    {
      ++n;
    }
    return n;
  }

  DbStatus Executor::RebuildTableRows(const CatalogTable &old_meta, const CatalogTable &new_meta,
                                      const std::vector<int> &col_map, bool copy_rows,
                                      uint32_t *new_first_page)
  {
    if (storage_ == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "执行器未绑定存储，无法重建表");
    }
    // ① 把老表全部行进内存（必须先读完再删表；TRUNCATE 不搬数据则跳过）
    std::vector<std::vector<storage::Value>> rows;
    if (copy_rows)
    {
      std::shared_ptr<storage::TableHeap> heap;
      const storage::Status os = storage_->open_table(old_meta.name, &heap);
      if (!os.ok())
      {
        return FromStorage(os, "ALTER 时打开表 " + old_meta.name);
      }
      for (auto it = heap->begin(); it != heap->end(); ++it)
      {
        const storage::Record &rec = *it;
        std::vector<storage::Value> row;
        row.reserve(rec.value_count());
        for (size_t i = 0; i < rec.value_count(); ++i)
        {
          row.push_back(rec.value(i));
        }
        rows.push_back(std::move(row));
      }
    }
    // ② 按新结构重建物理表（原表名与新表名可能不同 —— 表改名场景）
    const storage::Schema schema = ToStorageSchema(new_meta);
    {
      const storage::Status ds = storage_->drop_table(old_meta.name);
      if (!ds.ok())
      {
        return FromStorage(ds, "ALTER 时删除旧表 " + old_meta.name);
      }
      const storage::Status cs = storage_->create_table(new_meta.name, schema);
      if (!cs.ok())
      {
        return FromStorage(cs, "ALTER 后重建表 " + new_meta.name);
      }
    }
    // ③ 回填：按 col_map 逐列取值，新列（-1）补 NULL
    for (const auto &old_row : rows)
    {
      storage::Record rec;
      for (size_t i = 0; i < col_map.size(); ++i)
      {
        const int src = col_map[i];
        if (src < 0 || static_cast<size_t>(src) >= old_row.size())
        {
          rec.AddValue(storage::Value::Null());
        }
        else
        {
          rec.AddValue(old_row[static_cast<size_t>(src)]);
        }
      }
      storage::Rid rid;
      const storage::Status is = storage_->insert_record(new_meta.name, rec, &rid);
      if (!is.ok())
      {
        return FromStorage(is, "ALTER 回填 " + new_meta.name);
      }
    }
    if (new_first_page != nullptr)
    {
      std::shared_ptr<storage::TableHeap> heap;
      if (storage_->open_table(new_meta.name, &heap).ok() && heap)
      {
        *new_first_page = static_cast<uint32_t>(heap->first_page_id());
      }
    }
    DbLogInfo(logcat::kCatalog, "ALTER：已重建表 " + old_meta.name + " → " + new_meta.name + "（" +
                                    std::to_string(rows.size()) + " 行，" +
                                    std::to_string(new_meta.columns.size()) + " 列）");
    return DbStatus::Ok();
  }

  DbStatus Executor::BuildIndexTree(const CatalogTable &table,
                                    const std::vector<std::string> &cols, bool unique,
                                    bool require_not_null, uint32_t *root_out)
  {
    if (storage_ == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "执行器未绑定存储，无法建索引");
    }
    storage::BufferPoolManager *bpm = storage_->buffer_pool();
    if (bpm == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "存储引擎未暴露缓冲池，无法建索引");
    }
    // 列定位 + 键规格（按声明序）
    storage::BPlusTree::KeySpec spec;
    std::vector<int> col_idxs;
    for (const std::string &cn : cols)
    {
      const CatalogColumn *col = table.FindColumn(cn);
      if (col == nullptr)
      {
        return DbStatus::Error(DbCode::kColumnNotFound, "列不存在: " + table.name + "." + cn);
      }
      spec.columns.push_back(storage::BPlusTree::Column{
          ToStorageType(col->type), static_cast<uint16_t>(col->len)});
      col_idxs.push_back(table.ColumnIndex(col->name));
    }
    if (cols.empty())
    {
      return DbStatus::Error(DbCode::kInternal, "索引列清单为空");
    }
    if (!storage::BPlusTree::KeyFitsPage(bpm->page_size(), spec))
    {
      return DbStatus::Error(DbCode::kValueTooLong,
                             "索引列 (" + JoinIndexColumns(cols) +
                                 ") 的键编码过长，单页放不下最坏情况，拒绝建索引");
    }
    storage::BPlusTree tree(bpm, spec);
    storage::page_id_t root = storage::kInvalidPageId;
    const storage::Status cs = tree.Create(&root);
    if (!cs.ok())
    {
      return FromStorage(cs, "建索引树 " + table.name);
    }

    std::shared_ptr<storage::TableHeap> heap;
    const storage::Status os = storage_->open_table(table.name, &heap);
    if (!os.ok())
    {
      return FromStorage(os, "回填索引时打开表 " + table.name);
    }
    for (auto it = heap->begin(); it != heap->end(); ++it)
    {
      const storage::Record &rec = *it;
      std::vector<storage::Value> key_vals;
      bool bad = false;
      for (int ci : col_idxs)
      {
        if (ci < 0 || static_cast<size_t>(ci) >= rec.value_count())
        {
          bad = true;
          break;
        }
        key_vals.push_back(rec.value(static_cast<size_t>(ci)));
      }
      if (bad)
      {
        continue;
      }
      if (require_not_null)
      {
        for (const storage::Value &v : key_vals)
        {
          if (v.IsNull())
          {
            return DbStatus::Error(DbCode::kNotNullViolation,
                                   "列 (" + JoinIndexColumns(cols) +
                                       ") 存在 NULL 值，无法建立主键（主键列隐含非空）");
          }
        }
      }
      const std::string leaf = storage::EncodeLeafKeyColumns(
          key_vals, it.rid().page_id, static_cast<uint8_t>(it.rid().slot_id));
      bool dup = false;
      const storage::Status is = tree.Insert(leaf, &dup);
      if (!is.ok())
      {
        return FromStorage(is, "回填索引 " + table.name);
      }
    }
    // 唯一性校验：键 = 元组编码 + 行定位，按字节序排列 → 相邻键「去掉行定位后」
    // 相等即同元组。这把「列值元组重复」与「同键重复」一次覆盖。
    if (unique)
    {
      std::vector<std::string> all;
      const storage::Status ss = tree.ScanAll(&all);
      if (!ss.ok())
      {
        return FromStorage(ss, "校验唯一索引 " + table.name);
      }
      for (size_t i = 1; i < all.size(); ++i)
      {
        if (storage::StripLeafRowId(all[i - 1]).compare(storage::StripLeafRowId(all[i])) == 0)
        {
          return DbStatus::Error(DbCode::kUniqueViolation,
                                 "列 (" + JoinIndexColumns(cols) +
                                     ") 存在重复值，无法建立唯一索引/主键");
        }
      }
    }
    if (root_out != nullptr)
    {
      *root_out = static_cast<uint32_t>(root);
    }
    return DbStatus::Ok();
  }

  DbStatus Executor::RenameIndexMeta(const std::string &old_index_name, const CatalogIndex &index)
  {
    const DbStatus ds = catalog_->DeleteIndexRows(old_index_name);
    if (!ds.ok())
    {
      return ds;
    }
    return catalog_->WriteIndexRow(index);
  }

  DbStatus Executor::RebuildTableIndexes(const CatalogTable &new_meta,
                                         const std::vector<CatalogIndex> &index_metas,
                                         const std::string &rename_from,
                                         const std::string &rename_to)
  {
    const bool renaming = !rename_from.empty() && !rename_to.empty();
    for (const CatalogIndex &snap : index_metas)
    {
      CatalogIndex ix = snap;
      if (renaming)
      {
        ix.table = rename_to;
        // <表>_pk 是系统按表名派生的（见 PrimaryIndexName），表改名时跟着改；
        // 用户起的二级索引名保持不动（索引名全局唯一，改它反而是意外）。
        if (ix.name == PrimaryIndexName(rename_from))
        {
          ix.name = PrimaryIndexName(rename_to);
        }
      }
      uint32_t root = 0;
      const DbStatus bs = BuildIndexTree(new_meta, ix.columns, ix.unique, false, &root);
      if (!bs.ok())
      {
        return bs;
      }
      ix.root_page_id = root;
      // 旧名要显式删 —— 表改名时 <旧表>_pk 那条元数据行必须清掉，
      // 否则重启后目录里会多出一个指向不存在的表的索引。
      const DbStatus ws = RenameIndexMeta(snap.name, ix);
      if (!ws.ok())
      {
        return ws;
      }
    }
    return DbStatus::Ok();
  }

  DbStatus Executor::ExecAlterTable(const cella::CELLA_PlanNode &plan, const ExecContext &ctx,
                                    QueryResult *out)
  {
    const cella::CELLA_Stmt *st = plan.stmt;
    if (st == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "AlterTable 计划缺少语句信息");
    }
    const CatalogTable *found = catalog_->FindTable(st->tableName);
    if (found == nullptr)
    {
      return DbStatus::Error(DbCode::kTableNotFound, "表不存在: " + st->tableName);
    }
    if (CatalogManager::IsProtectedSystemTable(found->name))
    {
      return DbStatus::Error(DbCode::kSystemTableProtected, "系统表禁止修改: " + found->name);
    }
    const DbStatus ls = LockTable(found->name, LockMode::kExclusive, ctx, TablePriv::kNone);
    if (!ls.ok())
    {
      return ls;
    }
    // 快照：ReplaceTableMeta 会重建 tables_ 里的条目，之后再解引用 found 就悬空了。
    const CatalogTable old_meta = *found;
    std::vector<CatalogIndex> saved;
    for (const CatalogIndex *ix : catalog_->IndexesOfTable(old_meta.name))
    {
      saved.push_back(*ix);
    }

    switch (st->alterAction)
    {
    case cella::CELLA_Stmt::AlterAction::ADD_COLUMN:
      return AlterAddColumn(old_meta, saved, *st, out);
    case cella::CELLA_Stmt::AlterAction::DROP_COLUMN:
      return AlterDropColumn(old_meta, saved, *st, out);
    case cella::CELLA_Stmt::AlterAction::RENAME_TABLE:
      return AlterRenameTable(old_meta, saved, *st, out);
    case cella::CELLA_Stmt::AlterAction::RENAME_COLUMN:
      return AlterRenameColumn(old_meta, saved, *st, out);
    case cella::CELLA_Stmt::AlterAction::ADD_PRIMARY_KEY:
      return AlterAddPrimaryKey(old_meta, saved, *st, out);
    case cella::CELLA_Stmt::AlterAction::DROP_PRIMARY_KEY:
      return AlterDropPrimaryKey(old_meta, saved, *st, out);
    }
    return DbStatus::Error(DbCode::kInternal, "未知的 ALTER TABLE 动作");
  }

  // ALTER TABLE ... ADD [COLUMN] c T [NOT NULL]
  // 老行补 NULL（P5.1 的关键点）。若新列声明 NOT NULL 而表里已有数据 → 无法补值，拒绝。
  DbStatus Executor::AlterAddColumn(const CatalogTable &old_meta,
                                    const std::vector<CatalogIndex> &indexes,
                                    const cella::CELLA_Stmt &st, QueryResult *out)
  {
    const cella::CELLA_ColumnDef &cd = st.newColumn;
    if (IsRowidName(cd.name))
    {
      return DbStatus::Error(DbCode::kSqlError, "rowid 是只读伪列，不能用作列名");
    }
    if (old_meta.FindColumn(cd.name) != nullptr)
    {
      return DbStatus::Error(DbCode::kSqlError,
                             "列已存在: " + old_meta.name + "." + cd.name);
    }
    if (cd.primaryKey)
    {
      return DbStatus::Error(DbCode::kSqlError,
                             "ADD COLUMN 不接受 PRIMARY KEY；请改用 ADD PRIMARY KEY (列)");
    }

    CatalogColumn nc;
    nc.name = cd.name;
    nc.type = cd.type;
    nc.len = (cd.type == cella::CELLA_DataType::CHAR || cd.type == cella::CELLA_DataType::VARCHAR)
                 ? (cd.hasLen ? cd.len : 255)
                 : 0;
    nc.not_null = cd.notNull;
    nc.primary_key = false;

    {
      StorageGuard guard(storage_mutex_);
      if (nc.not_null && CountRowsOf(old_meta.name) > 0)
      {
        return DbStatus::Error(DbCode::kNotNullViolation,
                               "表 " + old_meta.name + " 已有数据，新列 " + cd.name +
                                   " 声明 NOT NULL 却无法为老行补值；"
                                   "请先去数据或改用允许 NULL 的列");
      }
    }

    CatalogTable new_meta = old_meta;
    new_meta.columns = old_meta.columns;
    new_meta.columns.push_back(nc);
    std::vector<int> col_map;
    for (size_t i = 0; i < old_meta.columns.size(); ++i)
    {
      col_map.push_back(static_cast<int>(i));
    }
    col_map.push_back(-1); // 新列：老行补 NULL

    uint32_t first_page = 0;
    {
      StorageGuard guard(storage_mutex_);
      const DbStatus rs = RebuildTableRows(old_meta, new_meta, col_map, true, &first_page);
      if (!rs.ok())
      {
        return rs;
      }
      CatalogTable persisted = new_meta;
      persisted.first_page_id = first_page;
      const DbStatus ws = catalog_->ReplaceTableMeta(old_meta.name, persisted);
      if (!ws.ok())
      {
        return ws;
      }
      const DbStatus is = RebuildTableIndexes(persisted, indexes, "", "");
      if (!is.ok())
      {
        return is;
      }
    }
    out->tag = "ALTER TABLE " + old_meta.name + " ADD COLUMN " + cd.name;
    DbLogInfo(logcat::kCatalog, out->tag);
    return DbStatus::Ok();
  }

  // ALTER TABLE ... DROP [COLUMN] c
  // 需要重写行（P5.2）：删掉该列后每一行的列数少 1。涉及该列的索引整条级联删除
  // （与语义层 SEM 的级联规则一致）；其余索引因行定位改变而重建。
  DbStatus Executor::AlterDropColumn(const CatalogTable &old_meta,
                                     const std::vector<CatalogIndex> &indexes,
                                     const cella::CELLA_Stmt &st, QueryResult *out)
  {
    const CatalogColumn *col = old_meta.FindColumn(st.alterColumnName);
    if (col == nullptr)
    {
      return DbStatus::Error(DbCode::kColumnNotFound,
                             "列不存在: " + old_meta.name + "." + st.alterColumnName);
    }
    if (old_meta.columns.size() <= 1)
    {
      return DbStatus::Error(DbCode::kSqlError,
                             "不能删除表 " + old_meta.name + " 的最后一列（表至少要有一列）");
    }
    if (col->primary_key)
    {
      return DbStatus::Error(DbCode::kSqlError,
                             "列 " + col->name + " 是主键列，不能直接删除；"
                             "请先 ALTER TABLE " + old_meta.name + " DROP PRIMARY KEY");
    }
    const std::string drop_name = col->name;
    const std::string drop_key = cella::cella_toUpper(drop_name);
    const int drop_idx = old_meta.ColumnIndex(drop_name);

    // 新列定义与映射：跳过被删列
    CatalogTable new_meta = old_meta;
    new_meta.columns.clear();
    std::vector<int> col_map;
    for (size_t i = 0; i < old_meta.columns.size(); ++i)
    {
      if (static_cast<int>(i) == drop_idx)
      {
        continue;
      }
      new_meta.columns.push_back(old_meta.columns[i]);
      col_map.push_back(static_cast<int>(i));
    }

    // 索引分两拨：引用了被删列的 → 删除；其余的 → 重建
    std::vector<CatalogIndex> keep;
    std::vector<std::string> cascaded;
    for (const CatalogIndex &ix : indexes)
    {
      bool hit = false;
      for (const std::string &ic : ix.columns)
      {
        if (cella::cella_toUpper(ic) == drop_key)
        {
          hit = true;
          break;
        }
      }
      if (hit)
      {
        cascaded.push_back(ix.name);
      }
      else
      {
        keep.push_back(ix);
      }
    }

    uint32_t first_page = 0;
    {
      StorageGuard guard(storage_mutex_);
      const DbStatus rs = RebuildTableRows(old_meta, new_meta, col_map, true, &first_page);
      if (!rs.ok())
      {
        return rs;
      }
      CatalogTable persisted = new_meta;
      persisted.first_page_id = first_page;
      const DbStatus ws = catalog_->ReplaceTableMeta(old_meta.name, persisted);
      if (!ws.ok())
      {
        return ws;
      }
      for (const std::string &ixn : cascaded)
      {
        const DbStatus ds = catalog_->DeleteIndexRows(ixn);
        if (!ds.ok())
        {
          return ds;
        }
      }
      const DbStatus is = RebuildTableIndexes(persisted, keep, "", "");
      if (!is.ok())
      {
        return is;
      }
    }
    out->tag = "ALTER TABLE " + old_meta.name + " DROP COLUMN " + drop_name;
    DbLogInfo(logcat::kCatalog, out->tag + (cascaded.empty()
                                                ? std::string()
                                                : "（级联删除索引 " +
                                                      JoinIndexColumns(cascaded) + "）"));
    return DbStatus::Ok();
  }

  // ALTER TABLE ... RENAME TO <new>
  // 存储层没有 rename_table，因此同样走「重建」：换名建表 + 回填 + 换目录。
  DbStatus Executor::AlterRenameTable(const CatalogTable &old_meta,
                                      const std::vector<CatalogIndex> &indexes,
                                      const cella::CELLA_Stmt &st, QueryResult *out)
  {
    if (cella::cella_toUpper(st.newName) == cella::cella_toUpper(old_meta.name))
    {
      out->tag = "ALTER TABLE " + old_meta.name + " RENAME TO " + st.newName;
      return DbStatus::Ok(); // 改成同名：空操作
    }
    if (CatalogManager::IsSystemTable(st.newName))
    {
      return DbStatus::Error(DbCode::kSqlError, "不能把表改名为系统表名: " + st.newName);
    }
    if (catalog_->FindTable(st.newName) != nullptr)
    {
      return DbStatus::Error(DbCode::kTableExists, "表已存在: " + st.newName);
    }

    CatalogTable new_meta = old_meta;
    new_meta.name = st.newName;
    std::vector<int> col_map;
    for (size_t i = 0; i < old_meta.columns.size(); ++i)
    {
      col_map.push_back(static_cast<int>(i)); // 列集合不变
    }

    uint32_t first_page = 0;
    {
      StorageGuard guard(storage_mutex_);
      const DbStatus rs = RebuildTableRows(old_meta, new_meta, col_map, true, &first_page);
      if (!rs.ok())
      {
        return rs;
      }
      CatalogTable persisted = new_meta;
      persisted.first_page_id = first_page;
      const DbStatus ws = catalog_->ReplaceTableMeta(old_meta.name, persisted);
      if (!ws.ok())
      {
        return ws;
      }
      const DbStatus is =
          RebuildTableIndexes(persisted, indexes, old_meta.name, st.newName);
      if (!is.ok())
      {
        return is;
      }
    }
    out->tag = "ALTER TABLE " + old_meta.name + " RENAME TO " + st.newName;
    DbLogInfo(logcat::kCatalog, out->tag);
    return DbStatus::Ok();
  }

  // ALTER TABLE ... RENAME COLUMN <old> TO <new>
  DbStatus Executor::AlterRenameColumn(const CatalogTable &old_meta,
                                       const std::vector<CatalogIndex> &indexes,
                                       const cella::CELLA_Stmt &st, QueryResult *out)
  {
    const CatalogColumn *col = old_meta.FindColumn(st.alterColumnName);
    if (col == nullptr)
    {
      return DbStatus::Error(DbCode::kColumnNotFound,
                             "列不存在: " + old_meta.name + "." + st.alterColumnName);
    }
    if (IsRowidName(st.newName))
    {
      return DbStatus::Error(DbCode::kSqlError, "rowid 是只读伪列，不能用作列名");
    }
    if (cella::cella_toUpper(st.newName) != cella::cella_toUpper(col->name) &&
        old_meta.FindColumn(st.newName) != nullptr)
    {
      return DbStatus::Error(DbCode::kSqlError, "列已存在: " + old_meta.name + "." + st.newName);
    }
    const std::string old_col_name = col->name;
    const std::string old_key = cella::cella_toUpper(old_col_name);

    CatalogTable new_meta = old_meta;
    new_meta.columns = old_meta.columns;
    for (CatalogColumn &c : new_meta.columns)
    {
      if (cella::cella_toUpper(c.name) == old_key)
      {
        c.name = st.newName;
      }
    }
    std::vector<int> col_map;
    for (size_t i = 0; i < old_meta.columns.size(); ++i)
    {
      col_map.push_back(static_cast<int>(i));
    }

    // 索引元数据里的列名同步改名（键按「值」编码、不含列名），但行定位变了 → 重建树
    std::vector<CatalogIndex> renamed = indexes;
    for (CatalogIndex &ix : renamed)
    {
      for (std::string &ic : ix.columns)
      {
        if (cella::cella_toUpper(ic) == old_key)
        {
          ic = st.newName;
        }
      }
      // column 字段是 cella_index 行格式里的权威文本，必须跟着 columns 重新拼
      ix.column = ix.JoinedColumns();
    }

    uint32_t first_page = 0;
    {
      StorageGuard guard(storage_mutex_);
      const DbStatus rs = RebuildTableRows(old_meta, new_meta, col_map, true, &first_page);
      if (!rs.ok())
      {
        return rs;
      }
      CatalogTable persisted = new_meta;
      persisted.first_page_id = first_page;
      const DbStatus ws = catalog_->ReplaceTableMeta(old_meta.name, persisted);
      if (!ws.ok())
      {
        return ws;
      }
      const DbStatus is = RebuildTableIndexes(persisted, renamed, "", "");
      if (!is.ok())
      {
        return is;
      }
    }
    out->tag = "ALTER TABLE " + old_meta.name + " RENAME COLUMN " + old_col_name + " TO " +
               st.newName;
    DbLogInfo(logcat::kCatalog, out->tag);
    return DbStatus::Ok();
  }

  // ALTER TABLE ... ADD PRIMARY KEY (a [, b])
  // 只加约束与索引，不动行布局 —— 因此**不需要重建物理表**。
  // 但已有数据必须满足主键的两条前提（非空 + 唯一），由 BuildIndexTree 校验。
  DbStatus Executor::AlterAddPrimaryKey(const CatalogTable &old_meta,
                                        const std::vector<CatalogIndex> &indexes,
                                        const cella::CELLA_Stmt &st, QueryResult *out)
  {
    (void)indexes;
    if (!old_meta.PrimaryKeyColumns().empty())
    {
      return DbStatus::Error(DbCode::kSqlError,
                             "表 " + old_meta.name + " 已有主键；请先 DROP PRIMARY KEY");
    }
    if (st.pkColumns.empty())
    {
      return DbStatus::Error(DbCode::kSqlError, "ADD PRIMARY KEY 至少需要一列");
    }
    // 解析成表内真实列名（保持大小写），并顺手查重
    std::vector<std::string> pk_names;
    std::set<std::string> seen;
    for (const std::string &cn : st.pkColumns)
    {
      const CatalogColumn *col = old_meta.FindColumn(cn);
      if (col == nullptr)
      {
        return DbStatus::Error(DbCode::kColumnNotFound,
                               "主键列不存在: " + old_meta.name + "." + cn);
      }
      if (!seen.insert(cella::cella_toUpper(col->name)).second)
      {
        return DbStatus::Error(DbCode::kSqlError, "主键列重复: " + col->name);
      }
      pk_names.push_back(col->name);
    }
    const std::string index_name = PrimaryIndexName(old_meta.name);

    CatalogTable new_meta = old_meta;
    new_meta.columns = old_meta.columns;
    for (CatalogColumn &c : new_meta.columns)
    {
      for (const std::string &pn : pk_names)
      {
        if (cella::cella_toUpper(c.name) == cella::cella_toUpper(pn))
        {
          c.primary_key = true;
          c.not_null = true; // 主键隐含非空（与 CREATE TABLE 一致）
        }
      }
    }

    {
      StorageGuard guard(storage_mutex_);
      // 顺序很重要：先建树（唯一性与空值校验都在里面），树建成后再落元数据。
      // 若中途失败，最坏情况是「多了个索引但目录还没标主键」——数据层的唯一性
      // 依然被索引守着，比反过来（标了主键却没有索引 → 唯一性失守）安全得多。
      uint32_t root = 0;
      const DbStatus bs =
          BuildIndexTree(old_meta, pk_names, true, true, &root);
      if (!bs.ok())
      {
        return bs;
      }
      CatalogIndex ix;
      ix.name = index_name;
      ix.table = old_meta.name;
      ix.columns = pk_names;
      ix.column = ix.JoinedColumns();
      ix.unique = true;
      ix.root_page_id = root;
      ix.created_at = static_cast<int64_t>(std::time(nullptr));
      const DbStatus ws = catalog_->WriteIndexRow(ix);
      if (!ws.ok())
      {
        return ws;
      }
      const DbStatus ms = catalog_->ReplaceTableMeta(old_meta.name, new_meta);
      if (!ms.ok())
      {
        return ms;
      }
    }
    out->tag = "ALTER TABLE " + old_meta.name + " ADD PRIMARY KEY (" +
               JoinIndexColumns(pk_names) + ")";
    DbLogInfo(logcat::kCatalog, out->tag);
    return DbStatus::Ok();
  }

  // ALTER TABLE ... DROP PRIMARY KEY
  // 只解除约束：删掉 <表>_pk 索引元数据 + 清掉的 primary_key 标志。
  // NOT NULL 保留（MySQL 同样如此，避免「删主键顺手放宽空值」的意外）。
  DbStatus Executor::AlterDropPrimaryKey(const CatalogTable &old_meta,
                                         const std::vector<CatalogIndex> &indexes,
                                         const cella::CELLA_Stmt &st, QueryResult *out)
  {
    (void)st;
    if (old_meta.PrimaryKeyColumns().empty())
    {
      return DbStatus::Error(DbCode::kSqlError, "表 " + old_meta.name + " 没有主键");
    }
    CatalogTable new_meta = old_meta;
    new_meta.columns = old_meta.columns;
    for (CatalogColumn &c : new_meta.columns)
    {
      c.primary_key = false;
    }
    const std::string index_name = PrimaryIndexName(old_meta.name);
    {
      StorageGuard guard(storage_mutex_);
      if (catalog_->FindIndex(index_name) != nullptr)
      {
        const DbStatus ds = catalog_->DeleteIndexRows(index_name);
        if (!ds.ok())
        {
          return ds;
        }
      }
      else
      {
        // 目录里没有 <表>_pk（历史库/手工索引）→ 兜底按主键列名找一条唯一索引删掉
        for (const CatalogIndex &ix : indexes)
        {
          if (ix.unique && !ix.columns.empty())
          {
            bool same = ix.columns.size() == old_meta.PrimaryKeyColumns().size();
            for (size_t i = 0; same && i < ix.columns.size(); ++i)
            {
              const int ci = old_meta.ColumnIndex(ix.columns[i]);
              if (ci < 0 || ci != old_meta.PrimaryKeyColumns()[i])
              {
                same = false;
              }
            }
            if (same)
            {
              const DbStatus ds = catalog_->DeleteIndexRows(ix.name);
              if (!ds.ok())
              {
                return ds;
              }
            }
          }
        }
      }
      const DbStatus ms = catalog_->ReplaceTableMeta(old_meta.name, new_meta);
      if (!ms.ok())
      {
        return ms;
      }
    }
    out->tag = "ALTER TABLE " + old_meta.name + " DROP PRIMARY KEY";
    DbLogInfo(logcat::kCatalog, out->tag);
    return DbStatus::Ok();
  }

  // ═════════════════════════════════════════════════════════════
  // TRUNCATE TABLE（P5.5）
  // ═════════════════════════════════════════════════════════════
  //
  // 语义：清空全部行、**保留表结构与索引定义**。实现是「删物理表 + 按同 schema 重建」，
  // 因此释放整棵数据页树是 O(1) 级的，比 `DELETE in t`（逐行删除 + 逐行维护索引）
  // 快得多 —— 这正是 TRUNCATE 存在的理由。
  // 索引的键指向行定位（页号 + 槽位），行没了必须重置 —— 每棵索引建一棵空树即可。
  DbStatus Executor::ExecTruncateTable(const cella::CELLA_PlanNode &plan, const ExecContext &ctx,
                                       QueryResult *out)
  {
    const cella::CELLA_Stmt *st = plan.stmt;
    if (st == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "TruncateTable 计划缺少语句信息");
    }
    const CatalogTable *found = catalog_->FindTable(st->tableName);
    if (found == nullptr)
    {
      return DbStatus::Error(DbCode::kTableNotFound, "表不存在: " + st->tableName);
    }
    if (CatalogManager::IsProtectedSystemTable(found->name))
    {
      return DbStatus::Error(DbCode::kSystemTableProtected, "系统表禁止清空: " + found->name);
    }
    const DbStatus ls = LockTable(found->name, LockMode::kExclusive, ctx, TablePriv::kNone);
    if (!ls.ok())
    {
      return ls;
    }
    const CatalogTable old_meta = *found;
    std::vector<CatalogIndex> saved;
    for (const CatalogIndex *ix : catalog_->IndexesOfTable(old_meta.name))
    {
      saved.push_back(*ix);
    }

    uint32_t first_page = 0;
    {
      StorageGuard guard(storage_mutex_);
      // 空表重建：col_map 全为恒等（列集合不变），可复用的行数为 0
      CatalogTable empty_meta = old_meta;
      std::vector<int> col_map;
      for (size_t i = 0; i < old_meta.columns.size(); ++i)
      {
        col_map.push_back(static_cast<int>(i));
      }
      const DbStatus rs = RebuildTableRows(old_meta, empty_meta, col_map, false, &first_page);
      if (!rs.ok())
      {
        return rs;
      }
      CatalogTable persisted = empty_meta;
      persisted.first_page_id = first_page;
      const DbStatus ws = catalog_->ReplaceTableMeta(old_meta.name, persisted);
      if (!ws.ok())
      {
        return ws;
      }
      const DbStatus is = RebuildTableIndexes(persisted, saved, "", "");
      if (!is.ok())
      {
        return is;
      }
    }
    out->tag = "TRUNCATE TABLE " + old_meta.name;
    DbLogInfo(logcat::kCatalog, out->tag);
    return DbStatus::Ok();
  }

  // ── INSERT ──────────────────────────────────────────────────

  DbStatus Executor::ExecInsert(const cella::CELLA_PlanNode &plan, const ExecContext &ctx,
                                QueryResult *out)
  {
    const cella::CELLA_Stmt *st = plan.stmt;
    if (st == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "Insert 计划缺少语句信息");
    }
    const CatalogTable *meta = catalog_->FindTable(st->tableName);
    if (meta == nullptr)
    {
      return DbStatus::Error(DbCode::kTableNotFound, "表不存在: " + st->tableName);
    }
    const std::string name = meta->name;
    if (CatalogManager::IsProtectedSystemTable(name))
    {
      return DbStatus::Error(DbCode::kSystemTableProtected, "系统表禁止修改: " + name);
    }
    const DbStatus ls = LockTable(name, LockMode::kExclusive, ctx, TablePriv::kInsert);
    if (!ls.ok())
    {
      return ls;
    }

    // 目标列 → catalog 列下标
    std::vector<int> targets;
    if (st->insertColumns.empty())
    {
      for (size_t i = 0; i < meta->columns.size(); ++i)
      {
        targets.push_back(static_cast<int>(i));
      }
    }
    else
    {
      for (const auto &cn : st->insertColumns)
      {
        const int idx = meta->ColumnIndex(cn);
        if (idx < 0)
        {
          return DbStatus::Error(DbCode::kColumnNotFound, "INSERT 目标列不存在: " + cn);
        }
        targets.push_back(idx);
      }
    }

    StorageGuard guard(storage_mutex_);
    const size_t ncol = meta->columns.size();
    size_t inserted = 0;

    // ── P1.5：索引句柄一次性打开，循环内复用（每行都重建树句柄会重复 Attach）──
    std::vector<IndexHandle> indexes;
    {
      const DbStatus os = OpenTableIndexes(*meta, &indexes);
      if (!os.ok())
      {
        return os;
      }
    }

    // ── 主键唯一性：先一次性收集已有行的键，插入过程中累积比对 ──
    // 语句内累积（而不是逐行重扫）避免批量插入退化为 O(N²)；冲突即返回，
    // 语句级回滚会撤销本语句已插入的行。
    // 复合主键（表级 PRIMARY KEY (a, b)）没有主键索引，走这里的扫描式查重；
    // 单列主键另有 <table>_pk B+ 树做索引级唯一（见 P1.5），二者语义一致。
    const std::vector<int> pk_cols = meta->PrimaryKeyColumns();
    std::set<std::string> pk_keys;
    if (!pk_cols.empty())
    {
      std::vector<std::pair<storage::Rid, storage::Record>> existing;
      const DbStatus ss = ScanMatching(*meta, nullptr, ctx.with_rowid, &existing);
      if (!ss.ok())
      {
        return ss;
      }
      for (const auto &er : existing)
      {
        pk_keys.insert(KeyOfRow(er.second.values(), pk_cols));
      }
    }

    for (const auto &row : st->rows)
    {
      if (row.size() != targets.size())
      {
        return DbStatus::Error(DbCode::kValueCountMismatch,
                               "值个数 " + std::to_string(row.size()) + " 与目标列个数 " +
                                   std::to_string(targets.size()) + " 不一致");
      }
      storage::Record rec;
      for (size_t i = 0; i < ncol; ++i)
      {
        const int as_target =
            [&]() -> int
        {
          for (size_t j = 0; j < targets.size(); ++j)
          {
            if (targets[j] == static_cast<int>(i))
            {
              return static_cast<int>(j);
            }
          }
          return -1;
        }();
        if (as_target < 0)
        {
          if (meta->columns[i].not_null)
          {
            return DbStatus::Error(DbCode::kNotNullViolation,
                                   "列 " + meta->columns[i].name + " 为 NOT NULL，但 INSERT 未提供取值");
          }
          rec.AddValue(storage::Value::Null());
          continue;
        }
        storage::Value raw;
        const DbStatus es = ExprEval::Eval(*row[static_cast<size_t>(as_target)], EvalRow::Empty(), &raw);
        if (!es.ok())
        {
          return es;
        }
        storage::Value coerced;
        const DbStatus cs = CoerceValue(raw, meta->columns[i].type, meta->MaxLenAt(i),
                                        meta->columns[i].not_null, &coerced);
        if (!cs.ok())
        {
          return DbStatus::Error(cs.code(), "列 " + meta->columns[i].name + ": " + cs.message());
        }
        rec.AddValue(std::move(coerced));
      }

      if (!pk_cols.empty())
      {
        const std::string key = KeyOfRow(rec.values(), pk_cols);
        if (!pk_keys.insert(key).second)
        {
          if (pk_cols.size() == 1)
          {
            return DbStatus::Error(DbCode::kPrimaryKeyViolation,
                                   "主键冲突: 列 " +
                                       meta->columns[static_cast<size_t>(pk_cols[0])].name +
                                       " 的值已存在（表 " + name + "）");
          }
          std::string cols_text;
          for (size_t i = 0; i < pk_cols.size(); ++i)
          {
            if (i != 0)
            {
              cols_text += ", ";
            }
            cols_text += meta->columns[static_cast<size_t>(pk_cols[i])].name;
          }
          return DbStatus::Error(DbCode::kPrimaryKeyViolation,
                                 "主键冲突: 列组 (" + cols_text +
                                     ") 的组合值已存在（表 " + name + "）");
        }
      }

      storage::Rid rid;
      const storage::Status s = storage_->insert_record(name, rec, &rid);
      if (!s.ok())
      {
        return FromStorage(s, "插入 " + name);
      }
      // ── P1.5：索引与表数据保持一致 ──
      // 顺序讲究：先登记 undo、再做索引维护。索引维护可能因为唯一冲突失败，
      // 那时表行已经写进去了，只有 undo 里有记录，回滚路径才能把它撤掉 —— 否则
      // 会留下「表里有行、索引里没有」的脏数据（正是修复前观察到的现象）。
      if (ctx.recording())
      {
        UndoRecord u;
        u.kind = UndoRecord::Kind::kInsert;
        u.table = name;
        u.rid = rid;
        u.after = rec; // P2：撤销这条插入时要写一条 kDelete，得知道被插入的内容
        ctx.txn->AddUndo(std::move(u));
      }
      // ── P2.1：行变更入 WAL（前后像都在，重做/撤销都够用）──
      AppendWalRow(ctx.txn_id, wal::RecordType::kInsert, name, rid, {}, rec.values());
      {
        const DbStatus is = IndexRowInsert(&indexes, *meta, rec.values(), rid);
        if (!is.ok())
        {
          return is;
        }
      }
      ++inserted;
    }

    out->affected = inserted;
    out->tag = "INSERT 0 " + std::to_string(inserted);
    DbLogInfo(logcat::kExec, out->tag + " → " + name);
    return DbStatus::Ok();
  }

  // ── DELETE / UPDATE 共用的匹配扫描 ──────────────────────────

  DbStatus Executor::ScanMatching(const CatalogTable &table, const cella::CELLA_Expr *pred,
                                  bool with_rowid,
                                  std::vector<std::pair<storage::Rid, storage::Record>> *out)
  {
    StorageGuard guard(storage_mutex_);
    std::shared_ptr<storage::TableHeap> heap;
    const storage::Status os = storage_->open_table(table.name, &heap);
    if (!os.ok())
    {
      return FromStorage(os, "打开表 " + table.name);
    }

    const std::vector<FieldRef> fields = MakeFields(table, table.name, with_rowid);
    for (auto it = heap->begin(); it != heap->end(); ++it)
    {
      if (pred != nullptr)
      {
        EvalRow row;
        row.fields = &fields;
        // 谓词里可以出现 rowid，故求值行值与字段布局保持一致（末尾补 rowid）
        const std::vector<storage::Value> vals = ValuesWithRowid(it->values(), it.rid(), with_rowid);
        row.values = &vals;
        bool pass = false;
        const DbStatus es = ExprEval::EvalPredicate(*pred, row, &pass);
        if (!es.ok())
        {
          return es;
        }
        if (!pass)
        {
          continue;
        }
      }
      out->emplace_back(it.rid(), *it);
    }
    return DbStatus::Ok();
  }

  // 谓词恰为 `rowid = <整数>` 时按物理地址直达（省掉全表扫描）。
  // 命中返回 true 并填好 out；返回 false 表示「不适用或存储层出错」→ 调用方退回全表扫描。
  // 注意：rowid 由物理位置推出，位置若已空则必然无匹配行，这与扫描结果一致。
  bool Executor::DirectByRowid(const CatalogTable &table, const cella::CELLA_Expr *pred,
                               std::vector<std::pair<storage::Rid, storage::Record>> *out)
  {
    int64_t rowid = 0;
    if (!TryRowidEqLiteral(pred, &rowid))
    {
      return false;
    }
    storage::Rid rid;
    if (!RidFromRowid(rowid, &rid))
    {
      out->clear(); // 越界的 rowid 不可能命中任何行
      return true;
    }
    StorageGuard guard(storage_mutex_);
    std::shared_ptr<storage::TableHeap> heap;
    if (!storage_->open_table(table.name, &heap).ok())
    {
      return false; // 交给通用路径去报错
    }
    storage::Record rec;
    const storage::Status s = heap->GetRecord(rid, &rec);
    if (s.ok())
    {
      out->clear();
      out->emplace_back(rid, std::move(rec));
      return true;
    }
    if (s.code() == storage::StatusCode::kPageNotFound ||
        s.code() == storage::StatusCode::kInvalidArgument)
    {
      out->clear(); // 该物理位置没有行（已删或从未存在）→ 空命中
      return true;
    }
    return false; // 其它存储错误：退回扫描路径，由它给出准确诊断
  }

  // 目标行收集：rowid 直达优先（O(1)），否则退回全表扫描 + 谓词过滤
  DbStatus Executor::CollectTargets(const CatalogTable &table, const cella::CELLA_Expr *pred,
                                    bool with_rowid,
                                    std::vector<std::pair<storage::Rid, storage::Record>> *out)
  {
    out->clear();
    if (DirectByRowid(table, pred, out))
    {
      return DbStatus::Ok();
    }
    return ScanMatching(table, pred, with_rowid, out);
  }

  // ═════════════════════════════════════════════════════════════
  // 索引维护（P1.5）
  // ═════════════════════════════════════════════════════════════
  //
  // 设计要点：
  //   * 索引是**派生数据**：表行是权威，索引只是加速结构。因此维护顺序固定为
  //     「先改表、后改索引」——插入时若索引失败，表行已落盘，由语句级回滚撤销；
  //     删除时先打墓碑再删索引项，避免「索引里有、表里没有」的悬空项。
  //   * 唯一性检查必须在**插入索引项之前**做，否则重复值已经进树，再去查重会
  //     把自己也算作冲突。
  //   * 同一列值 → 同一条叶子键？不是：叶子键 = 列值编码 + 5B 行定位。因此
  //     「列值相同、行不同」在树里是**两条键**，唯一索引查重必须按列值前缀比，
  //     这正是 IndexValueFree 用 ScanRange 圈出等值区间再逐键解码的原因。
  //   * UPDATE 的存储实现是「删旧 + 插新」：若被索引列的值变了，索引项必须
  //     先删旧键、再插新键（DELETE_INSERT）；值没变则只换行定位，做原地替换。

  DbStatus Executor::OpenTableIndexes(const CatalogTable &table, std::vector<IndexHandle> *out)
  {
    out->clear();
    if (storage_ == nullptr || catalog_ == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "执行器未绑定存储/目录，无法维护索引");
    }
    const std::vector<const CatalogIndex *> metas = catalog_->IndexesOfTable(table.name);
    if (metas.empty())
    {
      return DbStatus::Ok();
    }
    storage::BufferPoolManager *bpm = storage_->buffer_pool();
    if (bpm == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "存储引擎未暴露缓冲池，无法维护索引");
    }
    out->reserve(metas.size());
    for (const CatalogIndex *m : metas)
    {
      if (m == nullptr || !m->valid())
      {
        continue;
      }
      // 复合适配：索引列清单逐列定位下标；任一列失效则整条索引跳过
      //（报错会让整张表彻底无法写，代价远大于一条陈旧索引）。
      std::vector<int> cols;
      bool stale = false;
      for (const std::string &cn : m->columns)
      {
        const int col = table.ColumnIndex(cn);
        if (col < 0)
        {
          DbLogWarn(logcat::kExec, "索引 " + m->name + " 的列 " + cn +
                                       " 不在表 " + table.name + " 上，已跳过维护");
          stale = true;
          break;
        }
        cols.push_back(col);
      }
      if (stale)
      {
        continue;
      }
      storage::BPlusTree::KeySpec spec;
      for (int col : cols)
      {
        const CatalogColumn &cc = table.columns[static_cast<size_t>(col)];
        spec.columns.push_back(storage::BPlusTree::Column{
            ToStorageType(cc.type), static_cast<uint16_t>(cc.len)});
      }

      IndexHandle h;
      h.meta = m;
      h.columns = std::move(cols);
      h.tree = std::make_unique<storage::BPlusTree>(bpm, spec);
      h.tree->Attach(static_cast<storage::page_id_t>(m->root_page_id));
      out->push_back(std::move(h));
    }
    // 保持稳定顺序，让多索引维护与诊断输出可复现
    std::sort(out->begin(), out->end(),
              [](const IndexHandle &a, const IndexHandle &b)
              {
                const int ca = a.columns.empty() ? -1 : a.columns[0];
                const int cb = b.columns.empty() ? -1 : b.columns[0];
                if (ca != cb)
                {
                  return ca < cb;
                }
                return a.meta->name < b.meta->name;
              });
    return DbStatus::Ok();
  }

  bool Executor::IndexKeyOf(const IndexHandle &ix, const std::vector<storage::Value> &row_values,
                            const storage::Rid &rid, std::string *leaf_key)
  {
    // 逐列取值（按索引声明序）；任一列越界都视为无效行 → 不维护
    std::vector<storage::Value> key_vals;
    key_vals.reserve(ix.columns.size());
    for (int col : ix.columns)
    {
      if (col < 0 || static_cast<size_t>(col) >= row_values.size())
      {
        return false;
      }
      key_vals.push_back(row_values[static_cast<size_t>(col)]);
    }
    // NULL 也进索引（与编码约定一致：NULL 排在最前）。
    // 唯一性由 IndexValueFree 单独判定（含 NULL 的元组不参与唯一判定）。
    *leaf_key = storage::EncodeLeafKeyColumns(key_vals, rid.page_id,
                                              static_cast<uint8_t>(rid.slot_id));
    return true;
  }

  DbStatus Executor::IndexValueFree(const IndexHandle &ix,
                                    const std::vector<storage::Value> &row_values,
                                    const storage::Rid &rid, bool *busy)
  {
    *busy = false;
    if (!ix.meta->unique)
    {
      return DbStatus::Ok();  // 非唯一索引一律放行
    }
    // 组装本索引的键值元组；任一列为 NULL → 不参与唯一判定（标准 SQL 语义：
    // NULL 表示「未知」，两个未知不相等。PK 列隐含 NOT NULL，不受影响）。
    std::vector<storage::Value> tuple;
    tuple.reserve(ix.columns.size());
    for (int col : ix.columns)
    {
      if (col < 0 || static_cast<size_t>(col) >= row_values.size())
      {
        return DbStatus::Ok();  // 无法定位 → 保守放行
      }
      const storage::Value &v = row_values[static_cast<size_t>(col)];
      if (v.IsNull())
      {
        return DbStatus::Ok();  // 含 NULL 的元组不参与唯一判定
      }
      tuple.push_back(v);
    }
    // 圈出「元组 == tuple」的全部行：元组编码（位图 + 各列）是单射的，
    // 同元组 ⇔ 字节前缀相同 → 用前缀扫描原语，完全不需要解码。
    const std::string prefix = storage::EncodeColumnKeys(tuple);
    std::vector<std::string> hits;
    const storage::Status ss = ix.tree->ScanPrefix(
        prefix,
        [&](const std::string &leaf) -> bool
        {
          hits.push_back(leaf);
          return true;
        });
    if (!ss.ok())
    {
      return FromStorage(ss, "唯一索引查重 " + ix.meta->name);
    }
    for (const std::string &leaf : hits)
    {
      // 同一个 Rid 是自己 → 不算冲突（UPDATE 原地不动时应允许）
      storage::page_id_t p = 0;
      uint8_t s = 0;
      if (storage::DecodeLeafKeyRid(leaf, &p, &s) && p == rid.page_id && s == rid.slot_id)
      {
        continue;
      }
      *busy = true;   // 前缀相同且 Rid 不同 → 同元组的另一行，冲突
      return DbStatus::Ok();
    }
    return DbStatus::Ok();
  }

  DbStatus Executor::IndexRowInsert(std::vector<IndexHandle> *indexes, const CatalogTable &table,
                                    const std::vector<storage::Value> &row_values,
                                    const storage::Rid &rid)
  {
    for (IndexHandle &ix : *indexes)
    {
      std::string leaf;
      if (!IndexKeyOf(ix, row_values, rid, &leaf))
      {
        continue;
      }
      bool busy = false;
      const DbStatus vs = IndexValueFree(ix, row_values, rid, &busy);
      if (!vs.ok())
      {
        return vs;
      }
      if (busy)
      {
        ++index_stats_.violations;
        const bool is_pk = SameIdent(ix.meta->name, PrimaryIndexName(table.name));
        return DbStatus::Error(
            is_pk ? DbCode::kPrimaryKeyViolation : DbCode::kUniqueViolation,
            std::string(is_pk ? "主键冲突: 列 (" : "唯一索引冲突: 列 (") +
                ix.meta->column + ") 的值已存在（表 " + table.name +
                (is_pk ? "）" : "，索引 " + ix.meta->name + "）"));
      }
      bool dup = false;
      const storage::Status is = ix.tree->Insert(leaf, &dup);
      if (!is.ok())
      {
        return FromStorage(is, "插入索引项 " + ix.meta->name);
      }
      ++index_stats_.inserts;
    }
    return DbStatus::Ok();
  }

  DbStatus Executor::IndexRowDelete(std::vector<IndexHandle> *indexes, const CatalogTable &table,
                                    const std::vector<storage::Value> &row_values,
                                    const storage::Rid &rid)
  {
    (void)table;
    for (IndexHandle &ix : *indexes)
    {
      std::string leaf;
      if (!IndexKeyOf(ix, row_values, rid, &leaf))
      {
        continue;
      }
      bool removed = false;
      const storage::Status s = ix.tree->Remove(leaf, &removed);
      if (!s.ok())
      {
        return FromStorage(s, "删除索引项 " + ix.meta->name);
      }
      if (removed)
      {
        ++index_stats_.deletes;
      }
    }
    return DbStatus::Ok();
  }

  DbStatus Executor::IndexRowUpdate(std::vector<IndexHandle> *indexes, const CatalogTable &table,
                                    const std::vector<storage::Value> &before,
                                    const std::vector<storage::Value> &after,
                                    const storage::Rid &old_rid, const storage::Rid &new_rid)
  {
    (void)table; // 表信息仅在预检阶段（IndexRowCheckUpdate）用于生成诊断文本
    // 唯一性预检已在 IndexRowCheckUpdate 中完成；这里只做树上的实际操作。
    for (IndexHandle &ix : *indexes)
    {
      std::string old_leaf;
      std::string new_leaf;
      if (!IndexKeyOf(ix, before, old_rid, &old_leaf) ||
          !IndexKeyOf(ix, after, new_rid, &new_leaf))
      {
        continue;
      }
      // 复合适配：任一索引列的值变了 → 元组变了 → 需要 DELETE_INSERT
      bool value_changed = false;
      for (int col : ix.columns)
      {
        const size_t ci = static_cast<size_t>(col);
        if (ci < before.size() && ci < after.size() &&
            !IndexValuesEqual(before[ci], after[ci]))
        {
          value_changed = true;
          break;
        }
      }
      const bool rid_changed =
          old_rid.page_id != new_rid.page_id || old_rid.slot_id != new_rid.slot_id;
      if (!value_changed && !rid_changed)
      {
        continue; // 键完全没变 → 无需触碰索引
      }
      bool removed = false;
      const storage::Status ds = ix.tree->Remove(old_leaf, &removed);
      if (!ds.ok())
      {
        return FromStorage(ds, "更新索引(删旧) " + ix.meta->name);
      }
      if (removed)
      {
        ++index_stats_.deletes;
      }
      bool dup = false;
      const storage::Status is = ix.tree->Insert(new_leaf, &dup);
      if (!is.ok())
      {
        return FromStorage(is, "更新索引(插新) " + ix.meta->name);
      }
      ++index_stats_.inserts;
      ++index_stats_.updates;
    }
    return DbStatus::Ok();
  }

  DbStatus Executor::IndexRowCheckUpdate(const std::vector<IndexHandle> &indexes,
                                         const CatalogTable &table,
                                         const std::vector<storage::Value> &before,
                                         const std::vector<storage::Value> &after,
                                         const storage::Rid &old_rid)
  {
    for (const IndexHandle &ix : indexes)
    {
      // 复合适配：任一索引列的值都没变 → 不可能产生新冲突，跳过
      bool any_changed = false;
      for (int col : ix.columns)
      {
        const size_t ci = static_cast<size_t>(col);
        if (ci >= before.size() || ci >= after.size())
        {
          continue;
        }
        if (!IndexValuesEqual(before[ci], after[ci]))
        {
          any_changed = true;
          break;
        }
      }
      if (!any_changed)
      {
        continue; // 该索引的元组没变 → 不可能产生新冲突
      }
      bool busy = false;
      const DbStatus vs = IndexValueFree(ix, after, old_rid, &busy);
      if (!vs.ok())
      {
        return vs;
      }
      if (busy)
      {
        ++index_stats_.violations;
        const bool is_pk = SameIdent(ix.meta->name, PrimaryIndexName(table.name));
        return DbStatus::Error(
            is_pk ? DbCode::kPrimaryKeyViolation : DbCode::kUniqueViolation,
            std::string(is_pk ? "主键冲突: 更新后的 (" : "唯一索引冲突: 更新后的 (") +
                ix.meta->column + ") 值与其它行重复（表 " + table.name +
                (is_pk ? "）" : "，索引 " + ix.meta->name + "）"));
      }
    }
    return DbStatus::Ok();
  }

  DbStatus Executor::IndexVerifyRow(const std::vector<IndexHandle> &indexes, const CatalogTable &table,
                                    const std::vector<storage::Value> &row_values,
                                    const storage::Rid &rid, bool *present)
  {
    (void)table;  // 索引句柄已自带列定义，表仅用于调用方可读性
    *present = true;
    for (const IndexHandle &ix : indexes)
    {
      std::string leaf;
      if (!IndexKeyOf(ix, row_values, rid, &leaf))
      {
        continue;
      }
      bool found = false;
      const storage::Status s = ix.tree->Contains(leaf, &found);
      if (!s.ok())
      {
        return FromStorage(s, "校验索引项 " + ix.meta->name);
      }
      if (!found)
      {
        *present = false;
        return DbStatus::Ok();
      }
    }
    return DbStatus::Ok();
  }

  // ── undo 补偿期间的索引维护（P1.5）──────────────────────────
  //
  // 回滚路径与执行路径的关键差异：回滚时**没有**调用方持有的 IndexHandle 列表，
  // 而且 undo 日志里只有表名。因此这里每次按表名重新打开索引句柄 —— 代价是
  // 回滚比正常执行慢（每步一次目录查找 + N 个 B+ 树 Attach），但回滚本身是
  // 异常路径，正确性优先。钩子失败只记警告，不改变回滚的成败判定：
  // 回滚失败会让事务进入 kRollbackFailed，比「索引暂时不一致」严重得多。

  void Executor::UndoIndexDropRow(const std::string &table_name, const storage::Rid &rid)
  {
    const CatalogTable *meta = catalog_ == nullptr ? nullptr : catalog_->FindTable(table_name);
    if (meta == nullptr)
    {
      return; // 表都没了（DROP TABLE 后回滚）→ 索引元数据也已级联清理
    }
    std::vector<IndexHandle> indexes;
    if (!OpenTableIndexes(*meta, &indexes).ok() || indexes.empty())
    {
      return;
    }
    // 行已从表里删掉，但索引键需要行值才能重建 → 用「按列值前缀 + Rid 尾」的做法
    // 无法还原（列值未知）。改为：把该行在每个索引里的**旧键**按 Rid 定位删除。
    // 由于叶子键 = 列值编码 + Rid，仅凭 Rid 无法直接定位；但被撤销的插入在
    // 索引里恰好有一条以该 Rid 结尾的键。遍历一次等值区间代价高，这里改用
    // 全树扫描匹配 Rid 尾部 —— 只在回滚路径发生，可接受。
    for (IndexHandle &ix : indexes)
    {
      std::vector<std::string> victims;
      const storage::Status ss = ix.tree->ScanAll(&victims);
      if (!ss.ok())
      {
        DbLogWarn(logcat::kExec, "回滚清理索引 " + ix.meta->name + " 扫描失败: " + ss.ToString());
        continue;
      }
      for (const std::string &leaf : victims)
      {
        storage::page_id_t p = 0;
        uint8_t s = 0;
        if (!storage::DecodeLeafKeyRid(leaf, &p, &s))
        {
          continue;
        }
        if (p != rid.page_id || s != rid.slot_id)
        {
          continue;
        }
        bool removed = false;
        const storage::Status ds = ix.tree->Remove(leaf, &removed);
        if (!ds.ok())
        {
          DbLogWarn(logcat::kExec, "回滚清理索引 " + ix.meta->name + " 失败: " + ds.ToString());
          continue;
        }
        if (removed)
        {
          ++index_stats_.deletes;
        }
      }
    }
  }

  void Executor::UndoIndexRebuildRow(const std::string &table_name, const storage::Rid &rid,
                                     const storage::Record &record)
  {
    const CatalogTable *meta = catalog_ == nullptr ? nullptr : catalog_->FindTable(table_name);
    if (meta == nullptr)
    {
      return;
    }
    std::vector<IndexHandle> indexes;
    if (!OpenTableIndexes(*meta, &indexes).ok() || indexes.empty())
    {
      return;
    }
    // 先清掉可能残留的同 Rid 旧键（UPDATE 的旧版本键用的是旧 Rid，通常不冲突；
    // 但 DELETE 回滚时槽位可能被复用），再按恢复的内容重插。
    for (IndexHandle &ix : indexes)
    {
      std::string leaf;
      if (!IndexKeyOf(ix, record.values(), rid, &leaf))
      {
        continue;
      }
      bool found = false;
      if (ix.tree->Contains(leaf, &found).ok() && !found)
      {
        bool dup = false;
        const storage::Status is = ix.tree->Insert(leaf, &dup);
        if (!is.ok())
        {
          DbLogWarn(logcat::kExec, "回滚重建索引 " + ix.meta->name + " 失败: " + is.ToString());
          continue;
        }
        ++index_stats_.inserts;
      }
    }
  }

  void Executor::IndexUndoAdapter::OnUndoInsertDeleted(const std::string &table,
                                                       const storage::Rid &rid)
  {
    owner_->UndoIndexDropRow(table, rid);
  }

  void Executor::IndexUndoAdapter::OnUndoRowRestored(const std::string &table,
                                                     const storage::Rid &rid,
                                                     const storage::Record &record)
  {
    owner_->UndoIndexRebuildRow(table, rid, record);
  }

  // ═════════════════════════════════════════════════════════════
  // 访问路径选择（P1.3）
  //
  // 设计要点：
  //   * 计划树里表访问节点**始终**是 SeqScan —— 这是编译器的 golden 契约。
  //     索引下推是执行器的**运行时决策**：只有执行层拿得到索引元数据（cella_index）、
  //     表的列类型与真实行数，编译器看不到这些。
  //   * 下推谓词的来源：Filter 节点正下方的表访问节点。`Filter(pred) -> Scan` 是编译器
  //     固定的形状，因此「把谓词交给下方扫描」不会漏掉过滤条件 —— Filter 仍然照常执行，
  //     索引只负责把候选集缩小。**正确性由 Filter 兜底**（这也是 C3 的语义保证），
  //     所以我们只需保证索引区间「不漏行」。
  //   * 只识别能安全下推的形状（`列 op 常量`、`常量 op 列`、及其 AND 组合）。
  //     OR / 函数 / 列与列比较一律不下推 → 全表扫描，绝不冒漏行的风险。
  // ═════════════════════════════════════════════════════════════

  std::string AccessPathChoice::Describe() const
  {
    switch (path)
    {
    case AccessPath::kRowidLookup:
      return "RowidLookup (rowid = " + reason + ")";
    case AccessPath::kIndexOnlyScan:
    case AccessPath::kIndexScan:
    {
      std::string how;
      if (!eq_tuple.empty())
      {
        // 复合全键等值：逐列渲染 "a = 1 AND b = 'x'"
        std::vector<std::string> cols = SplitIndexColumns(index_column);
        for (size_t i = 0; i < eq_tuple.size() && i < cols.size(); ++i)
        {
          if (!how.empty())
          {
            how += " AND ";
          }
          how += cols[i] + " = " + RenderValue(eq_tuple[i]);
        }
      }
      else if (equality)
      {
        how = "= " + RenderValue(lower);
      }
      else if (has_lower || has_upper)
      {
        how = (has_lower ? (lower_inclusive ? ">= " : "> ") + RenderValue(lower) : std::string()) +
              (has_lower && has_upper ? " AND " : "") +
              (has_upper ? (upper_inclusive ? "<= " : "< ") + RenderValue(upper) : std::string());
      }
      else
      {
        how = "全区间";
      }
      const std::string kind =
          (path == AccessPath::kIndexOnlyScan) ? "IndexOnlyScan" : "IndexScan";
      return kind + " using " + index_name + " (" + index_column + " " + how + ")";
    }
    case AccessPath::kSeqScan:
    default:
      return "SeqScan";
    }
  }

  void Executor::CollectScanPredicates(const cella::CELLA_PlanNode &node,
                                       const cella::CELLA_Expr *pred, bool with_rowid,
                                       std::vector<ScanPredicate> *out)
  {
    // Filter 是唯一携带谓词的节点；一路往下继承，直到遇到真正的表访问节点。
    const cella::CELLA_Expr *carried = pred;
    if (node.op == "Filter" && node.pred != nullptr)
    {
      carried = node.pred.get();
    }
    if (node.op == "SeqScan" || node.op == "IndexScan")
    {
      ScanPredicate sp;
      sp.scan = &node;
      sp.pred = carried;
      sp.need_rowid = with_rowid;
      out->push_back(sp);
      return;  // 表访问节点没有子节点
    }
    // Join 之下不再继承外层谓词：Join 的 ON 条件属于 Join 自己，把外层条件
    // 错算到某一张表上会导致区间失真；靠子树的 Filter 各自下推即可。
    const cella::CELLA_Expr *child_pred = (node.op == "Join") ? nullptr : carried;
    for (const auto &c : node.children)
    {
      CollectScanPredicates(*c, child_pred, with_rowid, out);
    }
  }

  bool Executor::PredRefsColumn(const cella::CELLA_Expr *e, const std::string &column)
  {
    if (e == nullptr)
    {
      return false;
    }
    if (e->kind == cella::CELLA_Expr::Kind::COLUMN_REF)
    {
      return SameIdent(e->column, column);
    }
    return PredRefsColumn(e->left.get(), column) || PredRefsColumn(e->right.get(), column) ||
           PredRefsColumn(e->child.get(), column);
  }

  // 把一个「比较条件」尝试折算成索引区间的一侧。
  // 返回 true 表示识别成功且已更新 choice 的某一侧边界。
  namespace
  {
    // 比较运算符取反（`5 < x` 等价于 `x > 5`）：把「常量在左」翻成「列在左」
    cella::CELLA_Expr::BinOp FlipCompare(cella::CELLA_Expr::BinOp op)
    {
      using B = cella::CELLA_Expr::BinOp;
      switch (op)
      {
      case B::LT: return B::GT;
      case B::LE: return B::GE;
      case B::GT: return B::LT;
      case B::GE: return B::LE;
      default: return op;  // EQ / NE 对称
      }
    }
  }  // namespace

  bool Executor::TryIndexRange(const CatalogTable &table, const CatalogIndex &index, int column,
                               const cella::CELLA_Expr *pred, AccessPathChoice *choice) const
  {
    if (pred == nullptr || column < 0)
    {
      return false;
    }
    using Kind = cella::CELLA_Expr::Kind;
    using B = cella::CELLA_Expr::BinOp;

    // AND：两侧分别尝试，区间自然取交集（因为是就地收紧边界）
    if (pred->kind == Kind::BINARY && pred->bop == B::AND)
    {
      const bool l = TryIndexRange(table, index, column, pred->left.get(), choice);
      const bool r = TryIndexRange(table, index, column, pred->right.get(), choice);
      return l || r;
    }
    // OR / 其它逻辑一律不下推
    if (pred->kind != Kind::BINARY || pred->bop == B::OR)
    {
      return false;
    }

    const std::string target = table.columns[static_cast<size_t>(column)].name;
    const B op = pred->bop;
    if (op != B::EQ && op != B::LT && op != B::LE && op != B::GT && op != B::GE)
    {
      return false;  // 算术运算不是过滤条件
    }

    // 识别 `列 op 常量` 与 `常量 op 列`
    const cella::CELLA_Expr *col_side = nullptr;
    const cella::CELLA_Expr *lit_side = nullptr;
    B eff = op;
    if (pred->left && pred->left->kind == Kind::COLUMN_REF)
    {
      col_side = pred->left.get();
      lit_side = pred->right.get();
    }
    else if (pred->right && pred->right->kind == Kind::COLUMN_REF)
    {
      col_side = pred->right.get();
      lit_side = pred->left.get();
      eff = FlipCompare(op);
    }
    if (col_side == nullptr || lit_side == nullptr)
    {
      return false;
    }
    // 索引列必须正是被比较的那一列（可带表限定符）
    if (!SameIdent(col_side->column, target))
    {
      return false;
    }
    // 右侧必须是字面量（常量折叠后计划里就是字面量）；NULL 不可下推
    if (lit_side->kind != Kind::LITERAL || lit_side->lit == cella::CELLA_LiteralKind::NULL_LIT)
    {
      return false;
    }
    storage::Value lit;
    if (!LiteralToValue(*lit_side, &lit))
    {
      return false;
    }
    // 关键：区间边界必须换算成**索引列的声明类型**再编码。
    // 叶子键里的列值是按列类型编码的（INT 列 → kInt32），而字面量 20 在
    // LiteralToValue 里会变成 kInt64。两者编码出的字节不同，区间就会落空
    // （表现为「索引扫描一行都扫不到」）。用 CoerceValue 归一到列类型。
    {
      const CatalogColumn &cc = table.columns[static_cast<size_t>(column)];
      storage::Value coerced;
      const DbStatus cs2 = CoerceValue(lit, cc.type, static_cast<uint16_t>(cc.len),
                                      /*not_null=*/false, &coerced);
      if (cs2.ok())
      {
        lit = coerced;
      }
      // 归一失败（类型确实不兼容，如字符串与数值比较）→ 不下推，交给 Filter
      else
      {
        return false;
      }
    }

    // 收紧边界。等值同时压两侧，并把 equality 标上（描述与代价都用得到）。
    if (eff == B::EQ)
    {
      choice->equality = true;
      choice->has_lower = true;
      choice->lower = lit;
      choice->lower_inclusive = true;
      choice->has_upper = true;
      choice->upper = lit;
      choice->upper_inclusive = true;
      return true;
    }
    if (eff == B::GT || eff == B::GE)
    {
      // 已有更紧的下界就不替换（区间取交集）
      bool known = false;
      const int cmp = CompareValues(lit, choice->lower, &known);
      if (!choice->has_lower || !known || cmp > 0 || (cmp == 0 && eff == B::GT))
      {
        choice->has_lower = true;
        choice->lower = lit;
        choice->lower_inclusive = (eff == B::GE);
      }
      return true;
    }
    if (eff == B::LT || eff == B::LE)
    {
      bool known2 = false;
      const int cmp2 = CompareValues(lit, choice->upper, &known2);
      if (!choice->has_upper || !known2 || cmp2 < 0 || (cmp2 == 0 && eff == B::LT))
      {
        choice->has_upper = true;
        choice->upper = lit;
        choice->upper_inclusive = (eff == B::LE);
      }
      return true;
    }
    return false;
  }

  // ── 复合索引谓词下推（第一版：全键等值）─────────────────────
  // 在 AND 谓词树里收集「列 = 字面量」约束（`常量 = 列` 经 FlipCompare 后
  // 同样成立），要求索引的**每一列**都被约束到。缺任何一列（只约束了
  // 前缀）→ 不下推，老老实实全表扫描 —— 部分前缀匹配是第二步扩展，
  // 届时配合 ScanPrefix 的「列数 > 前缀列数」形态即可。
  bool Executor::TryCompositeEquality(const CatalogTable &table, const CatalogIndex &index,
                                      const std::vector<int> &columns,
                                      const cella::CELLA_Expr *pred, AccessPathChoice *choice) const
  {
    (void)index;  // 元数据仅用于将来诊断输出；列信息由 columns 携带
    if (pred == nullptr || columns.empty())
    {
      return false;
    }
    using Kind = cella::CELLA_Expr::Kind;
    using B = cella::CELLA_Expr::BinOp;

    // 递归收集 AND 树里的等值约束：键 = 列下标，值 = 已按列类型归一的字面量
    std::map<int, storage::Value, std::less<int>> eqs;
    // 递归遍历；返回 false 表示遇到无法处理的形状 → 整体放弃下推
    std::function<bool(const cella::CELLA_Expr *)> collect =
        [&](const cella::CELLA_Expr *e) -> bool
    {
      if (e == nullptr)
      {
        return true;  // 空子树：无约束但也不破坏形状
      }
      if (e->kind == Kind::BINARY && e->bop == B::AND)
      {
        return collect(e->left.get()) && collect(e->right.get());
      }
      if (e->kind != Kind::BINARY || e->bop != B::EQ)
      {
        // 非等值比较/其它形状：不下推也不拦截 —— 交给 Filter 兜底，
        // 但它不能为复合索引提供等值约束
        return true;
      }
      const cella::CELLA_Expr *col_side = nullptr;
      const cella::CELLA_Expr *lit_side = nullptr;
      if (e->left && e->left->kind == Kind::COLUMN_REF && e->right &&
          e->right->kind == Kind::LITERAL)
      {
        col_side = e->left.get();
        lit_side = e->right.get();
      }
      else if (e->right && e->right->kind == Kind::COLUMN_REF && e->left &&
               e->left->kind == Kind::LITERAL)
      {
        col_side = e->right.get();
        lit_side = e->left.get();
      }
      if (col_side == nullptr || lit_side == nullptr ||
          lit_side->lit == cella::CELLA_LiteralKind::NULL_LIT)
      {
        return true;  // 不是 `列 = 字面量`：跳过（NULL 等值不可下推）
      }
      const int ci = table.ColumnIndex(col_side->column);
      if (ci < 0)
      {
        return true;  // 其它表的列：跳过
      }
      storage::Value lit;
      if (!LiteralToValue(*lit_side, &lit))
      {
        return true;
      }
      // 与 TryIndexRange 同理：字面量必须归一到**列声明类型**再编码
      const CatalogColumn &cc = table.columns[static_cast<size_t>(ci)];
      storage::Value coerced;
      const DbStatus cs2 = CoerceValue(lit, cc.type, static_cast<uint16_t>(cc.len),
                                       /*not_null=*/false, &coerced);
      if (!cs2.ok())
      {
        return true;  // 类型不兼容 → 该约束作废（可能只剩 Filter 能过滤）
      }
      eqs[ci] = coerced;
      return true;
    };
    if (!collect(pred))
    {
      return false;
    }

    // 每一列都有等值约束 → 全键等值成立；按索引声明序组装元组
    std::vector<storage::Value> tuple;
    tuple.reserve(columns.size());
    for (int col : columns)
    {
      const auto it = eqs.find(col);
      if (it == eqs.end())
      {
        return false;  // 缺一列 → 不是全键等值（最左前缀规则，暂不服务）
      }
      tuple.push_back(it->second);
    }
    choice->eq_tuple = std::move(tuple);
    choice->equality = true;  // 代价模型按「等值定位」计费
    return true;
  }

  // 用索引的 min/max 估算范围选择率 = 区间宽度 / 值域宽度。
  //
  // 为什么可以这么做：B+ 树的叶子按列值编码有序，因此**第一个叶子键**就是
  // 该列最小值、**最后一个叶子键**就是最大值 —— 一份几乎免费的一维统计，
  // 不需要额外的统计表（P1.6 的统计信息就建立在这个观察上）。
  //
  // 只在数值列上做（字符串/日期的「宽度」没有直观含义，直接返回 0 走经验值）。
  double Executor::ComputeRangeSpan(storage::BPlusTree *tree,
                                    const AccessPathChoice &choice) const
  {
    if (tree == nullptr)
    {
      return 0.0;
    }
    // 范围选择率只对单列数值索引有意义（复合索引第一版只做全键等值，
    // 不会带着范围边界走到这里）
    if (tree->key_spec().columns.empty())
    {
      return 0.0;
    }
    const storage::ValueType vt = tree->key_spec().columns[0].type;
    const bool numeric = (vt == storage::ValueType::kInt32 || vt == storage::ValueType::kInt64 ||
                          vt == storage::ValueType::kFloat || vt == storage::ValueType::kDouble);
    if (!numeric)
    {
      return 0.0;
    }
    // 取首末键：ScanRange(nullptr, nullptr, true, cb) 顺序遍历，只留第一个；
    // 末位用一个「到最大」的扫描，只留最后一个。
    storage::Value lo_v;
    storage::Value hi_v;
    bool have_lo = false;
    bool have_hi = false;
    {
      const storage::Status ss =
          tree->ScanRange(nullptr, nullptr, true,
                         [&](const std::string &leaf) -> bool
                         {
                           if (!have_lo)
                           {
                             have_lo = storage::DecodeLeafKeyColumn(leaf, vt, &lo_v);
                           }
                           have_hi = storage::DecodeLeafKeyColumn(leaf, vt, &hi_v);
                           return true;  // 需要拿到最后一个，故不能提前终止
                         });
      if (!ss.ok() || !have_lo || !have_hi)
      {
        return 0.0;
      }
      if (lo_v.IsNull() || hi_v.IsNull())
      {
        return 0.0;  // NULL 参与值域会让占比失真 → 退回经验值
      }
    }
    auto as_double = [](const storage::Value &v, double *out) -> bool
    {
      switch (v.type)
      {
      case storage::ValueType::kInt32:
        *out = static_cast<double>(v.int32_val);
        return true;
      case storage::ValueType::kInt64:
        *out = static_cast<double>(v.int64_val);
        return true;
      case storage::ValueType::kFloat:
        *out = static_cast<double>(v.float_val);
        return true;
      case storage::ValueType::kDouble:
        *out = v.double_val;
        return true;
      default:
        return false;
      }
    };
    double lo = 0.0;
    double hi = 0.0;
    if (!as_double(lo_v, &lo) || !as_double(hi_v, &hi))
    {
      return 0.0;
    }
    if (hi <= lo)
    {
      return 0.0;  // 值域退化（全表同值）→ 经验值
    }
    // 谓词区间的两端：缺省时用值域端点补全
    double plo = lo;
    double phi = hi;
    if (choice.has_lower && !as_double(choice.lower, &plo))
    {
      return 0.0;
    }
    if (choice.has_upper && !as_double(choice.upper, &phi))
    {
      return 0.0;
    }
    if (phi < plo)
    {
      return 0.0;
    }
    const double span = (phi - plo) / (hi - lo);
    return span;
  }

  DbStatus Executor::UsableIndexes(const CatalogTable &table, std::vector<IndexHandle> *out)
  {
    // 与 OpenTableIndexes 同源，但语义更严：这里只挑「列下标有效」的索引，
    // 因为访问路径选择必须精确知道索引建在哪一列上。
    return OpenTableIndexes(table, out);
  }

  DbStatus Executor::ChooseAccessPath(const CatalogTable &table, const cella::CELLA_Expr *pred,
                                      bool with_rowid, AccessPathChoice *out)
  {
    *out = AccessPathChoice{};

    // ① rowid 等值 → 直接物理定位，永远最省
    int64_t rowid = 0;
    if (TryRowidEqLiteral(pred, &rowid))
    {
      out->path = AccessPath::kRowidLookup;
      out->reason = std::to_string(rowid);
      out->est_rows = 1.0;
      out->est_cost = 1.0;
      return DbStatus::Ok();
    }

    // ② 找出所有可用于下推的索引候选
    std::vector<IndexHandle> indexes;
    const DbStatus os = UsableIndexes(table, &indexes);
    if (!os.ok())
    {
      return os;
    }

    size_t row_count = 0;
    if (!indexes.empty())
    {
      // 行数是代价模型的输入。这里仍要走一遍表（存储层没有行数统计），
      // 但在「有索引且谓词可下推」的前提下这笔开销是划算的；P1.6 会缓存它。
      row_count = CountTableRows(table);
      if (row_count == 0)
      {
        indexes.clear();  // 空表：索引也省了，直接全表扫描（反正没有行）
      }
    }

    AccessPathChoice best;
    bool has_best = false;
    for (const IndexHandle &ix : indexes)
    {
      AccessPathChoice cand;
      cand.index_name = ix.meta->name;
      // 展示用列名：逗号拼接（单列 = 原名）
      cand.index_column = ix.meta->column;

      // 最左前缀规则：复合索引第一版只支持「全键等值」——谓词为每一列都
      // 提供 `列 = 常量` 时才下推（TryCompositeEquality）。
      // 单列索引走既有的范围下推（等值/区间），保持历史行为逐字节不变。
      bool pushed = false;
      if (ix.columns.size() > 1)
      {
        pushed = TryCompositeEquality(table, *ix.meta, ix.columns, pred, &cand);
      }
      else
      {
        pushed = TryIndexRange(table, *ix.meta,
                               ix.columns.empty() ? -1 : ix.columns[0], pred, &cand);
      }
      if (!pushed)
      {
        continue;  // 该索引下推不了这个谓词
      }
      // 覆盖扫描（index-only）：只读索引就能满足查询，不必回表。
      // 本项目索引键**只存被索引列的值**，所以 index-only 的前提是：
      //   ① 查询不需要 rowid；
      //   ② 谓词引用的列只有索引列本身；
      //   ③ 投影/排序/分组引用的列也只有索引列。
      // 条件 ③ 无法在表访问节点处可靠判断（上方算子的列需求在这里看不到），
      // 因此这里**只允许覆盖条件成立时使用 index-only**，其余一律回表 ——
      // 「多回一次表」只是慢一点，而「漏列」是错的。
      const bool covering = !with_rowid && !PredRefsColumnOutside(pred, table, ix.columns) &&
                            !StmtNeedsOtherColumn(table.name, ix.columns);
      cand.path = covering ? AccessPath::kIndexOnlyScan : AccessPath::kIndexScan;

      // 范围选择率：拿索引首末叶子键当该列的 min/max（索引有序 → 免费统计）。
      // 只在「非等值且有至少一侧边界」时才有意义。
      if (!cand.equality && (cand.has_lower || cand.has_upper))
      {
        cand.range_span = ComputeRangeSpan(ix.tree.get(), cand);
      }

      // 索引的实测几何（高度、叶子页数）—— P1.6 的关键输入。
      // 早期版本把树高写成常数 2.0，代价是「小表上索引永远输」：
      // 200 行表一次等值查找估成 (2+1)*2.0 = 6 的 I/O，比 5 页全表还贵。
      // 取真实树高后，小索引高度 = 1（根即叶子），下降只需 1 页。
      // BPlusTree::Height/LeafPageCount 都是 O(树高) / O(叶子数) 的轻量遍历。
      IndexStats st;
      if (ix.tree != nullptr)
      {
        st.height = static_cast<double>(ix.tree->Height());
        st.leaf_pages = static_cast<double>(ix.tree->LeafPageCount());
      }

      double er = 0.0;
      double ec = 0.0;
      EstimateAccessPath(table, row_count, st, cand, &er, &ec);
      cand.est_rows = er;
      cand.est_cost = ec;

      if (!has_best || cand.est_cost < best.est_cost)
      {
        best = cand;
        has_best = true;
      }
    }

    // ③ 与全表扫描比代价（走索引几何无关，传默认 Stats 即可）
    AccessPathChoice seq;
    seq.path = AccessPath::kSeqScan;
    double seq_rows = 0.0;
    EstimateAccessPath(table, row_count, IndexStats{}, seq, &seq_rows, &seq.est_cost);
    seq.est_rows = seq_rows;

    if (has_best && best.est_cost < seq.est_cost)
    {
      best.reason = "索引代价 " + FormatCost(best.est_cost) + " < 全表 " + FormatCost(seq.est_cost);
      *out = best;
      return DbStatus::Ok();
    }

    seq.reason = has_best
                     ? ("索引代价 " + FormatCost(best.est_cost) + " >= 全表 " + FormatCost(seq.est_cost))
                     : (indexes.empty() ? "表上无可用索引" : "谓词无法下推到任何索引");
    *out = seq;
    return DbStatus::Ok();
  }

  // 谓词是否引用了「该索引列之外」的列（决定能否 index-only scan）
  bool Executor::PredRefsColumnOutside(const cella::CELLA_Expr *pred, const CatalogTable &table,
                                       const std::vector<int> &index_columns)
  {
    if (pred == nullptr)
    {
      return false;
    }
    if (pred->kind == cella::CELLA_Expr::Kind::COLUMN_REF)
    {
      const int ci = table.ColumnIndex(pred->column);
      // 引用「索引列集合之外」的表列 → 必须回表（复合索引：集合内任一列都覆盖）
      const bool inside =
          std::find(index_columns.begin(), index_columns.end(), ci) != index_columns.end();
      return ci >= 0 && !inside;
    }
    return PredRefsColumnOutside(pred->left.get(), table, index_columns) ||
           PredRefsColumnOutside(pred->right.get(), table, index_columns) ||
           PredRefsColumnOutside(pred->child.get(), table, index_columns);
  }

  size_t Executor::CountTableRows(const CatalogTable &table)
  {
    StorageGuard guard(storage_mutex_);
    std::shared_ptr<storage::TableHeap> heap;
    if (!storage_->open_table(table.name, &heap).ok() || heap == nullptr)
    {
      return 0;
    }
    size_t n = 0;
    for (auto it = heap->begin(); it != heap->end(); ++it)
    {
      ++n;
    }
    return n;
  }

  // ── 代价估算 ────────────────────────────────────────────────
  //
  // P1.3 先用一个「能做出正确方向性决策」的简化模型，P1.6 会替换成基于
  // 表行数 / 索引高度 / 选择率 / I/O-CPU 因子的完整代价模型。这里刻意把
  // 结构写成「每页 I/O 的成本 + 每行的 CPU 成本」的形状，便于 P1.6 直接
  // 调参而不改调用点。
  //
  //   SeqScan      : cost = ceil(R / rows_per_page) * IO + R * CPU
  //   IndexScan    : cost = (索引高度 + 命中叶子数) * IO_INDEX
  //                        + 命中行数 * (CPU + 回表 IO)
  //   IndexOnlyScan: 与 IndexScan 同，但省掉回表 I/O
  //
  // 默认参数（可测、可解释，不追求和真实硬件成比例）：
  //   kRandomIo    = 2.0   随机页 I/O（索引下降与回表都算随机）
  //   kSeqIo       = 1.0   顺序页 I/O（全表扫描按顺序，显著更便宜）
  //   kCpuPerRow   = 0.10  处理一行的 CPU
  //   kRowsPerPage = 40    一页能放的行数（用于估算 SeqScan/叶子的页数）
  //   kIndexHeight = 2.0   索引高度（叶子 + 根；P1.6 会从 B+ 树实际高度取）
  //
  // 常数取值的依据（P1.3 收尾时按「让模型给出物理上正确的答案」反推）：
  //
  //   ① 随机 I/O / 顺序 I/O = 2 : 1。真实磁盘上这个比值通常是 5~50，
  //      但教学库的表都很小（几十页），比值取太大会让**任何**索引都因
  //      「一次随机读 = 十次顺序读」而输给全表扫描 —— 这不符合事实：
  //      小表上主键等值查 1 行，物理上一定比扫全表快。
  //      2 : 1 保留了「随机比顺序贵」的定性，又不会淹没 CPU 项。
  //   ② 每行 CPU = 0.10。这一项必须**真实存在**且量级可观：全表扫描的
  //      代价主体是「逐行解析/过滤」，而非「读页」。200 行 × 0.10 = 20，
  //      远大于 5 页的顺序 I/O —— 这正是索引能赢的原因。
  //      反过来，若把它调成 0.05，200 行的 CPU 只有 10，就会被
  //      「索引下降 3 页 × 4 = 12 的 I/O」翻盘（P1.3 重启用例失败的根因）。
  //
  // 关键约束（两个都不能违反，否则索引形同虚设）：
  //   ① 索引定位只花「树高」页 I/O，不能按命中行数重复计树高；
  //   ② 全表扫描的**每行 CPU 成本必须真实存在**（kCpuPerRow > 0），
  //      否则「行数少 ⇒ 全表几乎免费」会让任何索引都赢不了。
  // 有 ② 之后，只要命中行数 < 全表行数，索引的 CPU 节省就能盖过
  // 「随机 I/O 比顺序 I/O 贵」的劣势 —— 这正是现实里索引生效的原因。
  namespace
  {
    constexpr double kRandomIo = 2.0;
    constexpr double kSeqIo = 1.0;
    constexpr double kCpuPerRow = 0.10;
    constexpr double kRowsPerPage = 40.0;
    // 树高未知时的兜底值（正常情况都由 BPlusTree::Height 给出实测值）
    constexpr double kIndexHeightFallback = 1.0;
  }  // namespace

  void Executor::EstimateAccessPath(const CatalogTable &table, size_t row_count,
                                    const IndexStats &ix, const AccessPathChoice &choice,
                                    double *est_rows, double *est_cost) const
  {
    (void)table;
    const double r = static_cast<double>(row_count);
    switch (choice.path)
    {
    case AccessPath::kRowidLookup:
      *est_rows = 1.0;
      *est_cost = kRandomIo + kCpuPerRow;
      return;

    case AccessPath::kIndexOnlyScan:
    case AccessPath::kIndexScan:
    {
      // 选择率估算：
      //   * 等值：1/R（唯一索引下至多 1 行；非唯一按均匀分布估）
      //   * 范围：用索引的 min/max 做「区间占比」—— 索引本身按列值有序，
      //     首末叶子键就是该列的最小/最大值，等价于一份免费的一维直方图
      //     （P1.6 统计信息的基础）。拿不到 min/max 时退回 1/3 的经验值。
      //     没有这一步，`v > 990` 与 `v > 2` 会被估成同一个选择率，
      //     索引在「窄范围」上就永远赢不了全表扫描。
      double sel = 1.0 / 3.0;
      if (choice.equality)
      {
        sel = r > 0.0 ? 1.0 / r : 1.0;
      }
      else if (choice.range_span > 0.0)
      {
        // choice.range_span = 谓词区间宽度 / 列值域宽度（在 ChooseAccessPath
        // 里由索引 min/max 算出；无统计时为 0 → 用经验值）
        sel = choice.range_span;
        if (sel > 1.0)
        {
          sel = 1.0;
        }
      }
      double hits = r * sel;
      if (hits < 1.0)
      {
        hits = 1.0;
      }
      if (hits > r)
      {
        hits = r;
      }

      // ② 下降代价 = 树高页随机 I/O。用**实测树高**（P1.6），未知才退回默认值。
      // 这一点对结果的正确性很关键：小表上树高就是 1（根即叶子），
      // 若仍按常数 2.0 计，等值查找会被估成 3 页随机 I/O，
      // 反而输给「1 页顺序 I/O + CPU」的全表扫描。
      const double height = ix.known() ? ix.height : kIndexHeightFallback;

      // ③ 叶子代价：等值只落 1 页；范围按命中行铺满的页数估，
      // 上限是索引自身的叶子页数（不可能读超过整个叶子链）。
      double leaves = choice.equality ? 1.0 : std::ceil(hits / kRowsPerPage);
      if (leaves < 1.0)
      {
        leaves = 1.0;
      }
      if (ix.leaf_pages > 0.0 && leaves > ix.leaf_pages)
      {
        leaves = ix.leaf_pages;
      }

      double cost = (height + leaves) * kRandomIo + hits * kCpuPerRow;
      if (choice.path == AccessPath::kIndexScan)
      {
        // ④ 回表：按「命中行落在多少个不同的表页」计费，而不是每行一次 I/O。
        //
        // 命中行数不能直接当页数用：索引扫描取的是**一个区间**，区间内的行在
        // 物理上往往相邻（尤其主键/自增列），10 行很可能只落在 1~2 个页里。
        // 因此按「命中行自身铺满多少页」估，即 ceil(hits / 每页行数)，
        // 再与表的总页数取小（不可能读超过整表）。
        //
        // 这个修正是必要的：早先按 min(hits, 表页数) 计费时，
        // `v >= 100 AND v <= 120`（命中 10 行）被估成 10 次随机 I/O = 40，
        // 总代价 56.6 > 全表 38.0，索引永远赢不了 —— 这正是 P1.3 里
        // 「AND 区间」用例失败的根因。
        const double table_pages = std::max(1.0, std::ceil(r / kRowsPerPage));
        const double hit_pages = std::max(1.0, std::ceil(hits / kRowsPerPage));
        const double heap_pages = std::min(hit_pages, table_pages);
        cost += heap_pages * kRandomIo;
      }
      *est_rows = hits;
      *est_cost = cost;
      return;
    }

    case AccessPath::kSeqScan:
    default:
      *est_rows = r;
      // 顺序读所有页（一页至少算一次，空表也要读首页）
      *est_cost = std::max(1.0, std::ceil(r / kRowsPerPage)) * kSeqIo + r * kCpuPerRow;
      return;
    }
  }

  // ── EXPLAIN ────────────────────────────────────────────────

  DbStatus Executor::Explain(const cella::CELLA_PlanNode &plan, std::vector<AccessPathRecord> *out)
  {
    out->clear();
    // 收集 (表访问节点, 其上方谓词)，然后逐个做访问路径选择。
    // 只解析不执行：不申请表锁、不取数据行，因此 EXPLAIN 是**只读且廉价**的。
    std::vector<ScanPredicate> scans;
    CollectScanPredicates(plan, nullptr, /*with_rowid=*/false, &scans);
    for (const ScanPredicate &sp : scans)
    {
      std::string name;
      std::string alias;
      if (sp.scan->tableRef != nullptr)
      {
        name = sp.scan->tableRef->name;
        alias = sp.scan->tableRef->alias;
      }
      else if (!ParseTableDisplay(sp.scan->detail, &name, &alias))
      {
        continue;
      }
      const CatalogTable *meta = catalog_->FindTable(name);
      if (meta == nullptr)
      {
        return DbStatus::Error(DbCode::kTableNotFound, "表不存在: " + name);
      }
      AccessPathRecord rec;
      rec.table = meta->name;
      const DbStatus cs = ChooseAccessPath(*meta, sp.pred, sp.need_rowid, &rec.choice);
      if (!cs.ok())
      {
        return cs;
      }
      out->push_back(std::move(rec));
    }
    return DbStatus::Ok();
  }

  std::string Executor::ExplainText(const cella::CELLA_PlanNode &plan)
  {
    // 与 ExecGet 完全一致地登记列需求 —— 这是「EXPLAIN 展示的路径 == 实跑路径」
    // 的前提。否则 index-only scan 会被保守降级，两边给出不同答案。
    RegisterNeededColumns(plan);
    return ExplainTextNoRegister(plan);
  }

  std::string Executor::ExplainTextNoRegister(const cella::CELLA_PlanNode &plan) const
  {
    std::string out;
    ExplainNode(plan, nullptr, 0, &out);
    return out;
  }

  void Executor::ExplainNode(const cella::CELLA_PlanNode &node, const cella::CELLA_Expr *pred,
                             int depth, std::string *out) const
  {
    const std::string indent(static_cast<size_t>(depth) * 2, ' ');
    // Filter 的谓词要下传给下方的表访问节点，这样才能展示真实的选择结果
    const cella::CELLA_Expr *carried = pred;
    if (node.op == "Filter" && node.pred != nullptr)
    {
      carried = node.pred.get();
    }

    if (node.op == "SeqScan" || node.op == "IndexScan")
    {
      std::string name;
      std::string alias;
      if (node.tableRef != nullptr)
      {
        name = node.tableRef->name;
        alias = node.tableRef->alias;
      }
      else
      {
        ParseTableDisplay(node.detail, &name, &alias);
      }
      // EXPLAIN 必须保持 const：用 const_cast 走同一条选择逻辑（不修改任何状态）
      Executor *self = const_cast<Executor *>(this);
      const CatalogTable *meta = catalog_->FindTable(name);
      if (meta == nullptr)
      {
        *out += indent + node.op + " " + node.detail + "  [表不存在]\n";
        return;
      }
      AccessPathChoice choice;
      const DbStatus cs = self->ChooseAccessPath(*meta, carried, /*with_rowid=*/false, &choice);
      if (!cs.ok())
      {
        *out += indent + node.op + " " + node.detail + "  [路径选择失败]\n";
        return;
      }
      *out += indent + choice.Describe() + " [" + (alias.empty() ? name : name + " " + alias) + "]";
      *out += "  rows≈" + std::to_string(static_cast<long long>(choice.est_rows));
      *out += " cost≈" + FormatCost(choice.est_cost);
      if (node.op == "IndexScan" && !choice.uses_index())
      {
        *out += "  (显式 IndexScan 节点，但未选中索引)";
      }
      *out += "\n";
      return;
    }

    // 非表访问节点：照原样打印算子树，谓词继续向下传递
    *out += indent + node.op;
    if (!node.detail.empty())
    {
      *out += " " + node.detail;
    }
    *out += "\n";
    const cella::CELLA_Expr *child_pred =
        (node.op == "Join") ? nullptr : carried;
    for (const auto &c : node.children)
    {
      ExplainNode(*c, child_pred, depth + 1, out);
    }
  }



  // ── DELETE ──────────────────────────────────────────────────

  DbStatus Executor::ExecDelete(const cella::CELLA_PlanNode &plan, const ExecContext &ctx,
                                QueryResult *out)
  {
    const cella::CELLA_Stmt *st = plan.stmt;
    if (st == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "Delete 计划缺少语句信息");
    }
    const CatalogTable *meta = catalog_->FindTable(st->tableName);
    if (meta == nullptr)
    {
      return DbStatus::Error(DbCode::kTableNotFound, "表不存在: " + st->tableName);
    }
    const std::string name = meta->name;
    if (CatalogManager::IsProtectedSystemTable(name))
    {
      return DbStatus::Error(DbCode::kSystemTableProtected, "系统表禁止修改: " + name);
    }
    const DbStatus ls = LockTable(name, LockMode::kExclusive, ctx, TablePriv::kDelete);
    if (!ls.ok())
    {
      return ls;
    }

    // 两阶段：先收集命中行，再逐条打墓碑（避免边扫描边改页）
    std::vector<std::pair<storage::Rid, storage::Record>> hits;
    const DbStatus ms = CollectTargets(*meta, st->where.get(), ctx.with_rowid, &hits);
    if (!ms.ok())
    {
      return ms;
    }

    StorageGuard guard(storage_mutex_);
    // ── P1.5：打开索引句柄（每行删除都要同步删索引项）──
    std::vector<IndexHandle> indexes;
    {
      const DbStatus os = OpenTableIndexes(*meta, &indexes);
      if (!os.ok())
      {
        return os;
      }
    }
    size_t removed = 0;
    for (const auto &h : hits)
    {
      const storage::Status s = storage_->delete_record(name, h.first);
      if (!s.ok())
      {
        return FromStorage(s, "删除 " + name);
      }
      // 表行已打墓碑 → 索引项必须同步删除，否则会出现「索引指向已删行」的悬空项
      {
        const DbStatus is = IndexRowDelete(&indexes, *meta, h.second.values(), h.first);
        if (!is.ok())
        {
          return is;
        }
      }
      if (ctx.recording())
      {
        UndoRecord u;
        u.kind = UndoRecord::Kind::kDelete;
        u.table = name;
        u.rid = h.first;
        u.before = h.second; // 回滚时按内容重插
        ctx.txn->AddUndo(std::move(u));
      }
      AppendWalRow(ctx.txn_id, wal::RecordType::kDelete, name, h.first, h.second.values(), {});
      ++removed;
    }
    out->affected = removed;
    out->tag = "DELETE " + std::to_string(removed);
    DbLogInfo(logcat::kExec, out->tag + " ← " + name);
    return DbStatus::Ok();
  }

  // ── UPDATE ──────────────────────────────────────────────────
  // 存储层没有原地更新，故实现为「删旧 + 插新」，旧内容进 undo 日志。

  DbStatus Executor::ExecUpdate(const cella::CELLA_PlanNode &plan, const ExecContext &ctx,
                                QueryResult *out)
  {
    const cella::CELLA_Stmt *st = plan.stmt;
    if (st == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "Update 计划缺少语句信息");
    }
    const CatalogTable *meta = catalog_->FindTable(st->tableName);
    if (meta == nullptr)
    {
      return DbStatus::Error(DbCode::kTableNotFound, "表不存在: " + st->tableName);
    }
    const std::string name = meta->name;
    if (CatalogManager::IsProtectedSystemTable(name))
    {
      return DbStatus::Error(DbCode::kSystemTableProtected, "系统表禁止修改: " + name);
    }
    const DbStatus ls = LockTable(name, LockMode::kExclusive, ctx, TablePriv::kUpdate);
    if (!ls.ok())
    {
      return ls;
    }

    struct Assignment
    {
      int index = -1;
      const cella::CELLA_Expr *expr = nullptr;
    };
    std::vector<Assignment> assigns;
    for (const auto &kv : st->sets)
    {
      const int idx = meta->ColumnIndex(kv.first);
      if (idx < 0)
      {
        return DbStatus::Error(DbCode::kColumnNotFound, "SET 目标列不存在: " + kv.first);
      }
      Assignment a;
      a.index = idx;
      a.expr = kv.second.get();
      assigns.push_back(a);
    }

    std::vector<std::pair<storage::Rid, storage::Record>> hits;
    const DbStatus ms = CollectTargets(*meta, st->where.get(), ctx.with_rowid, &hits);
    if (!ms.ok())
    {
      return ms;
    }

    const std::vector<FieldRef> fields = MakeFields(*meta, name, ctx.with_rowid);
    StorageGuard guard(storage_mutex_);
    size_t updated = 0;

    // ── P1.5：打开索引句柄（UPDATE = 删旧 + 插新 → 索引做 DELETE_INSERT）──
    std::vector<IndexHandle> indexes;
    {
      const DbStatus os = OpenTableIndexes(*meta, &indexes);
      if (!os.ok())
      {
        return os;
      }
    }

    // ── 主键唯一性（仅当更新涉及主键列时检查）──
    // 语义是「把命中行的主键值改成新值」：新值不能与**未命中行**冲突，
    // 命中行之间也不能改成同一个新值；「排除自身」用命中 Rid 集合实现。
    // 复合主键（表级 PRIMARY KEY (a, b)，无主键索引）与单列主键走同一条扫描查重，
    // 差别仅在「涉及主键列」的判定是集合包含而非相等。
    const std::vector<int> pk_cols = meta->PrimaryKeyColumns();
    bool pk_assigned = false;
    for (const auto &a : assigns)
    {
      if (!pk_cols.empty() &&
          std::find(pk_cols.begin(), pk_cols.end(), a.index) != pk_cols.end())
      {
        pk_assigned = true;
      }
    }
    std::set<std::string> pk_taken;
    if (pk_assigned)
    {
      std::vector<std::pair<storage::Rid, storage::Record>> all_rows;
      const DbStatus as = ScanMatching(*meta, nullptr, ctx.with_rowid, &all_rows);
      if (!as.ok())
      {
        return as;
      }
      std::set<std::pair<storage::page_id_t, storage::slot_id_t>> hit_rids;
      for (const auto &h : hits)
      {
        hit_rids.insert({h.first.page_id, h.first.slot_id});
      }
      for (const auto &row : all_rows)
      {
        // 命中行会被改写，其旧主键值不参与比对（这才是「排除自身」）
        if (hit_rids.count({row.first.page_id, row.first.slot_id}) != 0)
        {
          continue;
        }
        pk_taken.insert(KeyOfRow(row.second.values(), pk_cols));
      }
    }

    for (const auto &h : hits)
    {
      storage::Record fresh;
      for (size_t i = 0; i < meta->columns.size(); ++i)
      {
        const Assignment *a = nullptr;
        for (const auto &cand : assigns)
        {
          if (cand.index == static_cast<int>(i))
          {
            a = &cand;
            break;
          }
        }
        if (a == nullptr)
        {
          fresh.AddValue(h.second.value(i)); // 未赋值列保持原值
          continue;
        }
        EvalRow row;
        row.fields = &fields;
        row.values = &h.second.values();
        storage::Value raw;
        const DbStatus es = ExprEval::Eval(*a->expr, row, &raw);
        if (!es.ok())
        {
          return es;
        }
        storage::Value coerced;
        const DbStatus cs =
            CoerceValue(raw, meta->columns[i].type, meta->MaxLenAt(i), meta->columns[i].not_null, &coerced);
        if (!cs.ok())
        {
          return DbStatus::Error(cs.code(), "列 " + meta->columns[i].name + ": " + cs.message());
        }
        fresh.AddValue(std::move(coerced));
      }

      if (pk_assigned)
      {
        const std::string key = KeyOfRow(fresh.values(), pk_cols);
        if (!pk_taken.insert(key).second)
        {
          return DbStatus::Error(DbCode::kPrimaryKeyViolation,
                                 "主键冲突: 更新后的主键值与其它行重复（表 " + name + "）");
        }
      }

      // ── P1.5：唯一索引预检，必须发生在「删旧行」之前 ──
      // 索引维护分两步：删除旧版本键、插入新版本键。若把唯一性检查推后到
      // 第二步，删除已经发生 → 回滚要同时还原「旧行」并清理「新行」，
      // 而新行的 undo 记录此时还没写（要等 insert_record 拿到 Rid）。
      // 因此这里先只做检查（不动树），确认无冲突后再生效，保证冲突时
      // 表与索引都处于「什么都没改」的状态。
      {
        const DbStatus cs = IndexRowCheckUpdate(indexes, *meta, h.second.values(), fresh.values(),
                                                h.first);
        if (!cs.ok())
        {
          return cs;
        }
      }

      const storage::Status ds = storage_->delete_record(name, h.first);
      if (!ds.ok())
      {
        return FromStorage(ds, "更新(删旧) " + name);
      }

      UndoRecord *update_undo = nullptr;
      if (ctx.recording())
      {
        UndoRecord u;
        u.kind = UndoRecord::Kind::kUpdate;
        u.table = name;
        u.rid = storage::Rid{}; // 新版本尚未插入，失败时只需重插旧内容
        u.before = h.second;
        u.after = fresh;        // P2：撤销时要写一条反向 kUpdate
        update_undo = &ctx.txn->AddUndo(std::move(u));
      }

      storage::Rid new_rid;
      const storage::Status is = storage_->insert_record(name, fresh, &new_rid);
      if (!is.ok())
      {
        return FromStorage(is, "更新(插新) " + name);
      }
      // 立刻回填新版本位置：此后任何失败，回滚都能先删新版本再重插旧内容。
      // （不能等索引维护成功再填 —— 索引维护失败时新行已经落库了。）
      if (update_undo != nullptr)
      {
        update_undo->rid = new_rid;
      }
      AppendWalRow(ctx.txn_id, wal::RecordType::kUpdate, name, new_rid, h.second.values(),
                   fresh.values());
      // ── P1.5：索引同步。行定位变了（删旧+插新），所以即便列值没变，
      //           叶子键也必须换成新 Rid；唯一性已在上面预检过。
      {
        const DbStatus xs =
            IndexRowUpdate(&indexes, *meta, h.second.values(), fresh.values(), h.first, new_rid);
        if (!xs.ok())
        {
          return xs;
        }
      }
      ++updated;
    }

    out->affected = updated;
    out->tag = "UPDATE " + std::to_string(updated);
    DbLogInfo(logcat::kExec, out->tag + " ~ " + name);
    return DbStatus::Ok();
  }

  // ── GET ─────────────────────────────────────────────────────

  DbStatus Executor::ExecGet(const cella::CELLA_PlanNode &plan, const ExecContext &ctx,
                             QueryResult *out)
  {
    // 登记「本语句每张表需要哪些列」——判定 index-only scan 用（见头文件说明）。
    // 收集范围：投影表达式、谓词、排序键、分组键、聚合参数，以及聚合/排序节点
    // 自身引用的列。取并集并**宁多勿少**：多登记一列只会让计划退回回表扫描，
    // 少登记一列会导致索引覆盖扫描漏列 —— 后者是错误，前者只是慢。
    RegisterNeededColumns(plan);
    RowSet rs;
    const DbStatus s = Run(plan, ctx, &rs);
    if (!s.ok())
    {
      return s;
    }
    out->columns.reserve(rs.fields.size());
    for (const auto &f : rs.fields)
    {
      ResultColumn c;
      c.name = f.Display();
      out->columns.push_back(std::move(c));
    }
    out->rows = std::move(rs.rows);
    out->tag = "GET " + std::to_string(out->rows.size());
    return DbStatus::Ok();
  }

  // ═════════════════════════════════════════════════════════════
  // 查询算子
  // ═════════════════════════════════════════════════════════════

  DbStatus Executor::Run(const cella::CELLA_PlanNode &node, const ExecContext &ctx, RowSet *out)
  {
    if (out == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "Run: 输出为空");
    }
    out->Clear();
    ++operator_calls_;
    DbLogDebug(logcat::kExec, "算子 " + node.op);

    const std::string &op = node.op;
    if (op == "SeqScan")
      return OpSeqScan(node, ctx, out);
    if (op == "IndexScan")
      return OpIndexScan(node, ctx, out);
    if (op == "Filter")
      return OpFilter(node, ctx, out);
    if (op == "Project")
      return OpProject(node, ctx, out);
    if (op == "Distinct")
      return OpDistinct(node, ctx, out);
    if (op == "Aggregate")
      return OpAggregate(node, ctx, out);
    if (op == "Sort")
      return OpSort(node, ctx, out);
    if (op == "Limit")
      return OpLimit(node, ctx, out);
    if (op == "Page")
      return OpPage(node, ctx, out);
    if (op == "Join")
      return OpJoin(node, ctx, out);
    if (op == "Union")
      return OpUnion(node, ctx, out);

    return DbStatus::Error(DbCode::kNotImplemented, "执行器不支持算子: " + op);
  }

  bool Executor::ParseTableDisplay(const std::string &detail, std::string *name, std::string *alias)
  {
    std::string inner = detail;
    if (!inner.empty() && inner.front() == '[')
    {
      inner.erase(inner.begin());
    }
    if (!inner.empty() && inner.back() == ']')
    {
      inner.pop_back();
    }
    std::istringstream is(inner);
    if (!(is >> *name))
    {
      return false;
    }
    is >> *alias; // 可缺省
    return true;
  }

  // ── 表访问算子的公共实现 ──────────────────────────────────────
  //
  // 计划里表访问节点恒为 SeqScan，但**实际走哪条路径由这里决定**（见文件上方
  // 「访问路径选择」的说明）。这是 P1.3 的核心：把「计划文本」与「执行决策」
  // 解耦 —— 计划文本保持 golden 契约不变，执行期却能吃到索引加速。
  //
  // 谓词从哪来？OpSeqScan 是 Run() 递归下到叶子时被调用的，它看不到上层的
  // Filter。因此调用方（OpFilter）在进入子算子前会把谓词登记到 scan_pred_，
  // 由这里取用。契约：登记/取用严格配对（Filter 进 → Scan 取），失败路径也要
  // 清理，否则会污染后续语句（见 ScanPredScope 的 RAII）。
  DbStatus Executor::OpTableAccess(const cella::CELLA_PlanNode &node, const ExecContext &ctx,
                                   RowSet *out, bool explicit_index_scan)
  {
    std::string name;
    std::string alias;
    if (node.tableRef != nullptr)
    {
      name = node.tableRef->name;
      alias = node.tableRef->alias;
    }
    else if (!ParseTableDisplay(node.detail, &name, &alias))
    {
      return DbStatus::Error(DbCode::kInternal, "表访问缺少表信息: " + node.detail);
    }

    const CatalogTable *meta = catalog_->FindTable(name);
    if (meta == nullptr)
    {
      return DbStatus::Error(DbCode::kTableNotFound, "表不存在: " + name);
    }
    const std::string real = meta->name;
    const DbStatus ls = LockTable(real, LockMode::kShared, ctx, TablePriv::kRead); // 读锁
    if (!ls.ok())
    {
      return ls;
    }

    // 取出预登记的谓词（消费式：取一次就清掉）
    const cella::CELLA_Expr *pred = TakeScanPredicate(real);

    AccessPathChoice choice;
    const DbStatus cs = ChooseAccessPath(*meta, pred, ctx.with_rowid, &choice);
    if (!cs.ok())
    {
      return cs;
    }
    if (explicit_index_scan && !choice.uses_index())
    {
      // 显式构造的 IndexScan 计划节点（单测/EXPLAIN 用）：强制走索引扫描框架，
      // 没有可用区间时就退化成「整索引扫 + 回表」，语义仍与全表扫描等价。
      choice.path = AccessPath::kIndexScan;
      if (!choice.has_lower && !choice.has_upper)
      {
        choice.index_name = FirstIndexName(*meta);
      }
    }

    const std::string qualifier = alias.empty() ? real : alias;

    AccessPathRecord rec;
    rec.table = real;
    rec.choice = choice;

    StorageGuard guard(storage_mutex_);
    std::shared_ptr<storage::TableHeap> heap;
    const storage::Status os = storage_->open_table(real, &heap);
    if (!os.ok())
    {
      return FromStorage(os, "打开表 " + real);
    }
    out->fields = MakeFields(*meta, qualifier, ctx.with_rowid);

    if (choice.uses_index())
    {
      // 索引扫描：先在索引上定位候选 Rid，再回表取整行。
      std::vector<IndexHandle> indexes;
      const DbStatus is = OpenTableIndexes(*meta, &indexes);
      if (!is.ok())
      {
        return is;
      }
      const IndexHandle *ix = nullptr;
      for (const IndexHandle &h : indexes)
      {
        if (SameIdent(h.meta->name, choice.index_name))
        {
          ix = &h;
          break;
        }
      }
      if (ix == nullptr)
      {
        // 索引在计划与执行之间被删掉了 → 退回全表扫描（不报错：
        // 「索引消失」不是查询的错，静默退化成正确的慢路径即可）
        rec.choice = AccessPathChoice{};
        rec.choice.path = AccessPath::kSeqScan;
        rec.choice.reason = "索引已不存在，退回全表扫描";
      }
      else
      {
        std::vector<storage::Rid> rids;
        const DbStatus rs = ScanIndexRids(*meta, *ix, choice, &rids);
        if (!rs.ok())
        {
          return rs;
        }
        // 回表：按 Rid 取整行（保持索引顺序 —— 等值/范围查询的有序输出）
        for (const storage::Rid &rid : rids)
        {
          storage::Record record;
          const storage::Status gs = heap->GetRecord(rid, &record);
          if (!gs.ok())
          {
            continue;  // 悬空索引项（理论上不该有）：跳过而不是让查询失败
          }
          out->rows.push_back(ValuesWithRowid(record.values(), rid, ctx.with_rowid));
        }
        rec.rows_out = out->rows.size();
        rec.pages_read = rids.size();
        access_paths_.push_back(rec);
        return DbStatus::Ok();
      }
    }

    for (auto it = heap->begin(); it != heap->end(); ++it)
    {
      // 只有语句引用了 rowid 时才在末尾补上（与 MakeFields 的伪列对齐）
      out->rows.push_back(ValuesWithRowid(it->values(), it.rid(), ctx.with_rowid));
    }
    rec.rows_out = out->rows.size();
    access_paths_.push_back(rec);
    return DbStatus::Ok();
  }

  // 在索引上按区间定位候选 Rid（等值 / 范围 / 全索引）。
  // 边界用「列值键」：lo = 下界列值，hi = 上界列值。
  //   * 等值：lo=hi=值，include_hi=true（含该列值全部行）
  //   * 范围：上界不包含时（< 或 <= 的严格侧），用 include_hi 控制
  //   * 无界：nullptr 表示从最小 / 到最大
  DbStatus Executor::ScanIndexRids(const CatalogTable &table, const IndexHandle &ix,
                                   const AccessPathChoice &choice,
                                   std::vector<storage::Rid> *out)
  {
    (void)table;
    out->clear();
    if (ix.meta == nullptr || ix.tree == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "索引句柄无效");
    }
    // ── 复合索引全键等值：前缀扫描原语 ──────────────────────
    // eq_tuple 非空 = 谓词给每一列都提供了等值约束。把整条元组编码成
    // 「NULL 位图 + 各列编码」的前缀，用 ScanPrefix 做字节前缀匹配 ——
    // 元组编码是单射的，字节前缀相同 ⇔ 元组相同；也绕开了「VARCHAR 内容
    // 可含 0xFF，排他上界凑不出来」的问题。
    if (!choice.eq_tuple.empty())
    {
      const std::string prefix = storage::EncodeColumnKeys(choice.eq_tuple);
      const storage::Status ss = ix.tree->ScanPrefix(
          prefix,
          [&](const std::string &leaf) -> bool
          {
            storage::page_id_t p = 0;
            uint8_t s = 0;
            if (storage::DecodeLeafKeyRid(leaf, &p, &s))
            {
              storage::Rid rid;
              rid.page_id = p;
              rid.slot_id = s;
              out->push_back(rid);
            }
            return true;
          });
      if (!ss.ok())
      {
        return FromStorage(ss, "索引前缀扫描 " + ix.meta->name);
      }
      return DbStatus::Ok();
    }
    std::string lo;
    std::string hi;
    const std::string *lo_p = nullptr;
    const std::string *hi_p = nullptr;
    if (choice.has_lower)
    {
      lo = storage::StripLeafRowId(storage::EncodeLeafKey(choice.lower, 0, 0));
      lo_p = &lo;
    }
    if (choice.has_upper)
    {
      hi = storage::StripLeafRowId(storage::EncodeLeafKey(choice.upper, 0, 0));
      hi_p = &hi;
    }
    // include_hi：等值时含边界；范围按边界开闭。
    // 注意 ScanRange 的 include_hi=false 会排除「列值 == hi」的全部行，
    // 正好对应 SQL 的严格 `<`；true 对应 `<=` 与等值。
    const bool include_hi = choice.equality ? true : choice.upper_inclusive;
    std::vector<storage::Rid> rids;
    const storage::Status ss = ix.tree->ScanRange(
        lo_p, hi_p, include_hi,
        [&](const std::string &leaf) -> bool
        {
          storage::page_id_t p = 0;
          uint8_t s = 0;
          if (storage::DecodeLeafKeyRid(leaf, &p, &s))
          {
            storage::Rid rid;
            rid.page_id = p;
            rid.slot_id = s;
            rids.push_back(rid);
          }
          return true;
        });
    if (!ss.ok())
    {
      return FromStorage(ss, "索引扫描 " + ix.meta->name);
    }
    *out = std::move(rids);
    return DbStatus::Ok();
  }

  std::string Executor::FirstIndexName(const CatalogTable &table) const
  {
    const std::vector<const CatalogIndex *> metas = catalog_->IndexesOfTable(table.name);
    for (const CatalogIndex *m : metas)
    {
      if (m != nullptr && m->valid())
      {
        return m->name;
      }
    }
    return std::string();
  }

  // 谓词登记表：Filter(进) → Scan(取)。用表名（大写）做键。
  const cella::CELLA_Expr *Executor::TakeScanPredicate(const std::string &table)
  {
    const auto it = scan_pred_.find(cella::cella_toUpper(table));
    if (it == scan_pred_.end())
    {
      return nullptr;
    }
    const cella::CELLA_Expr *p = it->second;
    scan_pred_.erase(it);
    return p;
  }

  // 把谓词登记到一棵子树里的所有表访问节点上。
  // 遇到 Join 就停止下推：Join 的 ON 条件与外层谓词都不能简单地归给某一张表，
  // 强行登记会让区间失真。Join 之下各表若无谓词 → 各自全表扫描（正确且安全）。
  void Executor::PushPredicateToScans(const cella::CELLA_PlanNode &node,
                                      const cella::CELLA_Expr *pred)
  {
    if (pred == nullptr)
    {
      return;
    }
    if (node.op == "SeqScan" || node.op == "IndexScan")
    {
      std::string name;
      std::string alias;
      if (node.tableRef != nullptr)
      {
        name = node.tableRef->name;
      }
      else
      {
        ParseTableDisplay(node.detail, &name, &alias);
      }
      if (!name.empty())
      {
        PushScanPredicate(name, pred);
      }
      return;
    }
    if (node.op == "Join")
    {
      return;  // 不下推（见上）
    }
    for (const auto &c : node.children)
    {
      PushPredicateToScans(*c, pred);
    }
  }

  void Executor::PushScanPredicate(const std::string &table, const cella::CELLA_Expr *pred)
  {
    if (pred != nullptr)
    {
      scan_pred_[cella::cella_toUpper(table)] = pred;
    }
  }

  // 递归收集表达式里引用的列名（去重，保留原拼写）
  namespace
  {
    void CollectExprColumns(const cella::CELLA_Expr *e, std::vector<std::string> *out)
    {
      if (e == nullptr)
      {
        return;
      }
      if (e->kind == cella::CELLA_Expr::Kind::COLUMN_REF)
      {
        bool seen = false;
        for (const std::string &c : *out)
        {
          if (cella::cella_toUpper(c) == cella::cella_toUpper(e->column))
          {
            seen = true;
            break;
          }
        }
        if (!seen)
        {
          out->push_back(e->column);
        }
      }
      CollectExprColumns(e->left.get(), out);
      CollectExprColumns(e->right.get(), out);
      CollectExprColumns(e->child.get(), out);
    }
  }  // namespace

  void Executor::RegisterNeededColumns(const cella::CELLA_PlanNode &plan)
  {
    needed_cols_.clear();
    CollectNeededColumns(plan, this);
  }

  void Executor::CollectNeededColumns(const cella::CELLA_PlanNode &node, Executor *self)
  {
    (void)self;
    // 表访问节点：自身不带列引用，交给上层算子收集（见下）
    if (node.op == "SeqScan" || node.op == "IndexScan")
    {
      return;
    }
    // 只在「单表」语句里做精确登记 —— 多表 Join 时列归属无法从名称可靠判断
    // （同名列会串表），此时把全部列登记给每张表（保守回表，绝不漏列）。
    std::vector<std::string> cols;
    if (node.pred != nullptr)
    {
      CollectExprColumns(node.pred.get(), &cols);
    }
    if (node.onExpr != nullptr)
    {
      CollectExprColumns(node.onExpr.get(), &cols);
    }
    for (const auto &e : node.exprs)
    {
      CollectExprColumns(e.get(), &cols);
    }
    for (const auto &e : node.aggExprs)
    {
      CollectExprColumns(e.get(), &cols);
    }
    for (const cella::CELLA_ColName &k : node.sortKeys)
    {
      cols.push_back(k.column);
    }
    for (const cella::CELLA_ColName &k : node.groupKeys)
    {
      cols.push_back(k.column);
    }
    // 把收集到的列登记到本子树里的每一张表上（单表语句就是那一张表；
    // 多表 Join 时保守地给所有表都登记 —— 宁可多登记导致回表，不可漏列）。
    for (const auto &c : node.children)
    {
      CollectNeededColumns(*c, self);
    }
    if (!cols.empty())
    {
      RegisterColumnsForSubtree(node, cols);
    }
  }

  void Executor::RegisterColumnsForSubtree(const cella::CELLA_PlanNode &node,
                                           const std::vector<std::string> &cols)
  {
    if (node.op == "SeqScan" || node.op == "IndexScan")
    {
      std::string name;
      std::string alias;
      if (node.tableRef != nullptr)
      {
        name = node.tableRef->name;
      }
      else
      {
        ParseTableDisplay(node.detail, &name, &alias);
      }
      if (!name.empty())
      {
        const std::string key = cella::cella_toUpper(name);
        std::vector<std::string> &dst = needed_cols_[key];
        for (const std::string &c : cols)
        {
          bool seen = false;
          for (const std::string &d : dst)
          {
            if (cella::cella_toUpper(d) == cella::cella_toUpper(c))
            {
              seen = true;
              break;
            }
          }
          if (!seen)
          {
            dst.push_back(c);
          }
        }
      }
      return;
    }
    for (const auto &c : node.children)
    {
      RegisterColumnsForSubtree(*c, cols);
    }
  }

  void Executor::SetNeededColumns(const std::string &table, std::vector<std::string> columns)
  {
    needed_cols_[cella::cella_toUpper(table)] = std::move(columns);
  }

  bool Executor::StmtNeedsOtherColumn(const std::string &table,
                                      const std::vector<int> &index_columns) const
  {
    const auto it = needed_cols_.find(cella::cella_toUpper(table));
    if (it == needed_cols_.end())
    {
      // 没有登记（例如直接构造计划驱动算子，不经过 ExecGet）→ 保守回表。
      return true;
    }
    const CatalogTable *meta = catalog_->FindTable(table);
    if (meta == nullptr)
    {
      return true;
    }
    for (const std::string &col : it->second)
    {
      const int ci = meta->ColumnIndex(col);
      // 用到了「索引列集合之外」的列 → 必须回表（复合索引：集合内任一列都覆盖）
      const bool inside =
          std::find(index_columns.begin(), index_columns.end(), ci) != index_columns.end();
      if (ci >= 0 && !inside)
      {
        return true;
      }
    }
    return false;
  }

  DbStatus Executor::OpSeqScan(const cella::CELLA_PlanNode &node, const ExecContext &ctx,
                               RowSet *out)
  {
    return OpTableAccess(node, ctx, out, /*explicit_index_scan=*/false);
  }

  DbStatus Executor::OpIndexScan(const cella::CELLA_PlanNode &node, const ExecContext &ctx,
                                 RowSet *out)
  {
    return OpTableAccess(node, ctx, out, /*explicit_index_scan=*/true);
  }
  DbStatus Executor::RunSingleChild(const cella::CELLA_PlanNode &node, const ExecContext &ctx,
                                    RowSet *out)
  {
    if (node.children.size() != 1)
    {
      return DbStatus::Error(DbCode::kInternal,
                             node.op + " 期望恰好 1 个子算子，实际 " +
                                 std::to_string(node.children.size()));
    }
    return Run(*node.children[0], ctx, out);
  }

  DbStatus Executor::OpFilter(const cella::CELLA_PlanNode &node, const ExecContext &ctx,
                              RowSet *out)
  {
    // 索引下推的「交接点」：Filter 在驱动子算子之前，把自己的谓词登记到
    // 下方每个表访问节点上。这样叶子处的 Scan 就能据谓词选索引区间。
    // 正确性不依赖这里的判断是否精准 —— Filter 本身仍会完整求值一遍，
    // 索引只负责缩小候选集，绝不可能漏行。
    if (node.pred != nullptr && !node.children.empty())
    {
      PushPredicateToScans(*node.children[0], node.pred.get());
    }
    RowSet in;
    const DbStatus s = RunSingleChild(node, ctx, &in);
    // 无论成功与否都清掉登记：失败路径若残留，会污染后续语句的选择
    ResetScanPredicates();
    if (!s.ok())
    {
      return s;
    }
    out->fields = std::move(in.fields);
    if (node.pred == nullptr)
    {
      out->rows = std::move(in.rows); // 无谓词（不应出现）→ 透传
      return DbStatus::Ok();
    }
    out->rows.reserve(in.rows.size());
    for (const auto &row : in.rows)
    {
      EvalRow er;
      er.fields = &out->fields;
      er.values = &row;
      bool pass = false;
      const DbStatus es = ExprEval::EvalPredicate(*node.pred, er, &pass);
      if (!es.ok())
      {
        return es;
      }
      if (pass)
      {
        out->rows.push_back(row);
      }
    }
    return DbStatus::Ok();
  }

  DbStatus Executor::OpProject(const cella::CELLA_PlanNode &node, const ExecContext &ctx,
                               RowSet *out)
  {
    RowSet in;
    const DbStatus s = RunSingleChild(node, ctx, &in);
    if (!s.ok())
    {
      return s;
    }
    if (node.exprs.empty())
    {
      *out = std::move(in); // 防御：无投影表达式时透传
      visible_fields_ = 0;
      return DbStatus::Ok();
    }

    out->fields.clear();
    out->fields.reserve(node.exprs.size());
    for (size_t i = 0; i < node.exprs.size(); ++i)
    {
      FieldRef f;
      f.qualifier.clear();
      f.name = ExprEval::OutputName(*node.exprs[i],
                                    i < node.selectAliases.size() ? node.selectAliases[i] : std::string());
      out->fields.push_back(std::move(f));
    }
    const size_t visible = out->fields.size();

    // 隐藏排序键列（仅当本 Project 位于某个 Sort 之下）：每个键追加一列，
    // 顺序与 node.sortKeys 一致，便于 OpSort 按位取用。
    std::vector<cella::CELLA_Expr> hidden;
    if (sort_hint_ != nullptr)
    {
      hidden.reserve(sort_hint_->size());
      for (const auto &k : *sort_hint_)
      {
        cella::CELLA_Expr ref;
        ref.kind = cella::CELLA_Expr::Kind::COLUMN_REF;
        ref.table = k.table;
        ref.column = k.column;
        hidden.push_back(std::move(ref));
        FieldRef f;
        f.qualifier.clear();
        f.name = k.table.empty() ? k.column : (k.table + "." + k.column);
        out->fields.push_back(std::move(f));
      }
      visible_fields_ = visible;
    }
    else
    {
      visible_fields_ = 0;
    }

    out->rows.reserve(in.rows.size());
    for (const auto &row : in.rows)
    {
      EvalRow er;
      er.fields = &in.fields;
      er.values = &row;
      std::vector<storage::Value> produced;
      produced.reserve(visible + hidden.size());
      for (const auto &e : node.exprs)
      {
        storage::Value v;
        const DbStatus es = ExprEval::Eval(*e, er, &v);
        if (!es.ok())
        {
          return es;
        }
        produced.push_back(std::move(v));
      }
      for (const auto &he : hidden)
      {
        storage::Value v;
        const DbStatus es = ExprEval::Eval(he, er, &v);
        if (!es.ok())
        {
          // 未投影列有时确实无法在输入行里解析（如形似列名的表达式）；
          // 置 NULL 让 OpSort 优先用可见列，实在缺列时由 OpSort 报错。
          v = storage::Value::Null();
        }
        produced.push_back(std::move(v));
      }
      out->rows.push_back(std::move(produced));
    }
    return DbStatus::Ok();
  }

  DbStatus Executor::OpDistinct(const cella::CELLA_PlanNode &node, const ExecContext &ctx,
                                RowSet *out)
  {
    RowSet in;
    const DbStatus s = RunSingleChild(node, ctx, &in);
    if (!s.ok())
    {
      return s;
    }
    out->fields = std::move(in.fields);
    std::vector<int> idx;
    // DISTINCT 只看可见列：隐藏排序键列不参与去重语义
    const size_t visible = (visible_fields_ == 0) ? out->fields.size() : visible_fields_;
    idx.reserve(visible);
    for (size_t i = 0; i < visible; ++i)
    {
      idx.push_back(static_cast<int>(i));
    }
    std::vector<std::string> seen;
    seen.reserve(in.rows.size());
    for (const auto &row : in.rows)
    {
      const std::string key = KeyOfRow(row, idx);
      bool dup = false;
      for (const auto &k : seen)
      {
        if (k == key)
        {
          dup = true;
          break;
        }
      }
      if (dup)
      {
        continue;
      }
      seen.push_back(key);
      out->rows.push_back(row);
    }
    return DbStatus::Ok();
  }

  // 聚合函数求值：目前只支持 COUNT(*) / COUNT(col)。
  // 输入：分组内的全部行；输出：单个计数值（INT64）。
  DbStatus EvalAggregate(const cella::CELLA_Expr &agg,
                         const std::vector<std::vector<storage::Value>> &rows,
                         const std::vector<int> &colIdx, storage::Value *out)
  {
    long long cnt = 0;
    if (agg.aggStar)
    {
      cnt = static_cast<long long>(rows.size());
    }
    else
    {
      // COUNT(col)：只统计非 NULL 值（SQL 标准语义）
      if (colIdx.empty() || colIdx[0] < 0)
      {
        return DbStatus::Error(DbCode::kColumnNotFound,
                               "聚合列不存在: " +
                                   (agg.table.empty() ? agg.column : agg.table + "." + agg.column));
      }
      const int i = colIdx[0];
      for (const auto &row : rows)
      {
        if (static_cast<size_t>(i) < row.size() && !row[static_cast<size_t>(i)].IsNull())
        {
          ++cnt;
        }
      }
    }
    *out = storage::Value::BigInt(cnt);
    return DbStatus::Ok();
  }

  // 在输入字段里解析一个聚合表达式的引用列下标（COUNT(*) 返回空）
  DbStatus ResolveAggregateColumns(const cella::CELLA_Expr &agg, const RowSet &in,
                                   const std::vector<FieldRef> &fields, std::vector<int> *out)
  {
    (void)in;
    if (agg.aggStar)
    {
      return DbStatus::Ok();
    }
    const cella::CELLA_ColName key{agg.table, agg.column, agg.line, agg.col};
    const int i = ResolveInRange(fields, key, fields.size());
    if (i < 0)
    {
      return DbStatus::Error(DbCode::kColumnNotFound, "聚合列不存在: " + KeyName(key));
    }
    out->push_back(i);
    return DbStatus::Ok();
  }

  DbStatus Executor::OpAggregate(const cella::CELLA_PlanNode &node, const ExecContext &ctx,
                                 RowSet *out)
  {
    RowSet in;
    const DbStatus s = RunSingleChild(node, ctx, &in);
    if (!s.ok())
    {
      return s;
    }
    // 注意：fields 之后会被搬进 out->fields，解析一律基于本地副本
    const std::vector<FieldRef> inFields = in.fields;

    // 无分组键也无聚合 → 原样透传（理论上计划器不会这样发，防御性保留）
    if (node.groupKeys.empty() && node.aggExprs.empty())
    {
      out->fields = std::move(in.fields);
      out->rows = std::move(in.rows);
      return DbStatus::Ok();
    }

    // ① 解析分组键下标
    std::vector<int> keyIdx;
    keyIdx.reserve(node.groupKeys.size());
    for (const auto &k : node.groupKeys)
    {
      const int i = ResolveInRange(inFields, k, inFields.size());
      if (i < 0)
      {
        return DbStatus::Error(DbCode::kColumnNotFound, "分组列不存在: " + KeyName(k));
      }
      keyIdx.push_back(i);
    }

    // ② 解析聚合项引用列下标（每项一个下标数组，COUNT(*) 为空）
    std::vector<std::vector<int>> aggColIdx(node.aggExprs.size());
    for (size_t a = 0; a < node.aggExprs.size(); ++a)
    {
      const DbStatus rs = ResolveAggregateColumns(*node.aggExprs[a], in, inFields, &aggColIdx[a]);
      if (!rs.ok())
      {
        return rs;
      }
    }

    // ③ 按分组键分桶（保持首次出现顺序；NULL 归入同一桶）
    //    无分组键时：所有行归为唯一一组，全表聚合成单行（COUNT 对空表也输出 0）
    struct Group
    {
      std::vector<std::vector<storage::Value>> rows;
    };
    std::vector<std::pair<std::string, Group>> groups;
    if (node.groupKeys.empty())
    {
      groups.emplace_back(std::string(), Group{});
      groups.back().second.rows = in.rows;
    }
    else
    {
      for (const auto &row : in.rows)
      {
        const std::string key = KeyOfRow(row, keyIdx);
        Group *g = nullptr;
        for (auto &kv : groups)
        {
          if (kv.first == key)
          {
            g = &kv.second;
            break;
          }
        }
        if (g == nullptr)
        {
          groups.emplace_back(key, Group{});
          g = &groups.back().second;
        }
        g->rows.push_back(row);
      }
    }

    // ④ 输出布局：分组键列 + 聚合结果列（聚合在分组键之后，与投影顺序无关）
    out->fields.clear();
    for (const auto &k : node.groupKeys)
    {
      out->fields.push_back(FieldRef{"", k.column});
    }
    for (const auto &a : node.aggExprs)
    {
      const std::string fn = a->aggFunc.empty() ? "COUNT" : a->aggFunc;
      std::string name = a->aggStar
                             ? fn + "(*)"
                             : fn + "(" + (a->table.empty() ? a->column : a->table + "." + a->column) + ")";
      out->fields.push_back(FieldRef{"", name});
    }

    for (const auto &kv : groups)
    {
      std::vector<storage::Value> outRow;
      if (!node.groupKeys.empty())
      {
        // 该组首行的分组键值（同组内相等）
        const std::vector<storage::Value> &first = kv.second.rows.front();
        for (int i : keyIdx)
        {
          outRow.push_back(static_cast<size_t>(i) < first.size() ? first[static_cast<size_t>(i)]
                                                                 : storage::Value::Null());
        }
      }
      for (size_t a = 0; a < node.aggExprs.size(); ++a)
      {
        storage::Value v;
        const DbStatus es = EvalAggregate(*node.aggExprs[a], kv.second.rows, aggColIdx[a], &v);
        if (!es.ok())
        {
          return es;
        }
        outRow.push_back(std::move(v));
      }
      out->rows.push_back(std::move(outRow));
    }
    return DbStatus::Ok();
  }

  DbStatus Executor::OpSort(const cella::CELLA_PlanNode &node, const ExecContext &ctx, RowSet *out)
  {
    // 告诉下方的 Project「这些键要参与排序」，它会在可见列之后追加隐藏键列
    sort_hint_ = &node.sortKeys;
    visible_fields_ = 0;
    RowSet in;
    const DbStatus s = RunSingleChild(node, ctx, &in);
    sort_hint_ = nullptr;
    if (!s.ok())
    {
      visible_fields_ = 0;
      return s;
    }
    out->fields = std::move(in.fields);
    out->rows = std::move(in.rows);

    const size_t visible = (visible_fields_ == 0) ? out->fields.size() : visible_fields_;
    visible_fields_ = 0;

    if (node.sortKeys.empty())
    {
      return DbStatus::Ok();
    }

    std::vector<int> idx;
    std::vector<bool> asc = node.sortAsc;
    idx.reserve(node.sortKeys.size());
    for (size_t j = 0; j < node.sortKeys.size(); ++j)
    {
      const cella::CELLA_ColName &k = node.sortKeys[j];
      // ① 先在可见列里找（限定名优先，失败退化为列名）
      int i = ResolveInRange(out->fields, k, visible);
      // ② 可见列里没有（ORDER BY 引用了未投影列）→ 取同序的隐藏键列
      if (i < 0 && visible + j < out->fields.size())
      {
        i = static_cast<int>(visible + j);
        DbLogDebug(logcat::kExec, "排序键 " + KeyName(k) + " 未投影，使用隐藏排序列");
      }
      if (i < 0)
      {
        return DbStatus::Error(DbCode::kColumnNotFound, "排序列不存在: " + KeyName(k));
      }
      idx.push_back(i);
    }
    while (asc.size() < idx.size())
    {
      asc.push_back(true);
    }

    std::stable_sort(out->rows.begin(), out->rows.end(),
                     [&](const std::vector<storage::Value> &a,
                         const std::vector<storage::Value> &b)
                     {
                       for (size_t k = 0; k < idx.size(); ++k)
                       {
                         const size_t i = static_cast<size_t>(idx[k]);
                         const storage::Value &x = a[i];
                         const storage::Value &y = b[i];
                         if (x.IsNull() && y.IsNull())
                         {
                           continue;
                         }
                         if (x.IsNull())
                         {
                           return false; // NULL 排最后
                         }
                         if (y.IsNull())
                         {
                           return true;
                         }
                         const int c = CompareValues(x, y, nullptr);
                         if (c == 0)
                         {
                           continue;
                         }
                         return asc[k] ? (c < 0) : (c > 0);
                       }
                       return false;
                     });

    // 裁掉隐藏排序键列，对外只保留可见列
    if (visible < out->fields.size())
    {
      out->fields.resize(visible);
      for (auto &row : out->rows)
      {
        if (row.size() > visible)
        {
          row.resize(visible);
        }
      }
    }
    return DbStatus::Ok();
  }

  DbStatus Executor::OpLimit(const cella::CELLA_PlanNode &node, const ExecContext &ctx, RowSet *out)
  {
    const DbStatus s = RunSingleChild(node, ctx, out);
    if (!s.ok())
    {
      return s;
    }
    if (node.rowLimit >= 0 && out->rows.size() > static_cast<size_t>(node.rowLimit))
    {
      out->rows.resize(static_cast<size_t>(node.rowLimit));
    }
    return DbStatus::Ok();
  }

  DbStatus Executor::OpPage(const cella::CELLA_PlanNode &node, const ExecContext &ctx, RowSet *out)
  {
    RowSet in;
    const DbStatus s = RunSingleChild(node, ctx, &in);
    if (!s.ok())
    {
      return s;
    }
    out->fields = std::move(in.fields);
    const long long offset = node.pageOffset < 0 ? 0 : node.pageOffset;
    const long long size = node.pageSize < 0 ? static_cast<long long>(in.rows.size()) : node.pageSize;
    const size_t begin = static_cast<size_t>(std::min<long long>(offset, static_cast<long long>(in.rows.size())));
    const size_t end = static_cast<size_t>(std::min<long long>(offset + size, static_cast<long long>(in.rows.size())));
    for (size_t i = begin; i < end; ++i)
    {
      out->rows.push_back(in.rows[i]);
    }
    return DbStatus::Ok();
  }

  DbStatus Executor::OpJoin(const cella::CELLA_PlanNode &node, const ExecContext &ctx, RowSet *out)
  {
    if (node.children.size() != 2)
    {
      return DbStatus::Error(DbCode::kInternal, "Join 期望 2 个子算子");
    }
    RowSet left;
    RowSet right;
    DbStatus s = Run(*node.children[0], ctx, &left);
    if (!s.ok())
    {
      return s;
    }
    s = Run(*node.children[1], ctx, &right);
    if (!s.ok())
    {
      return s;
    }

    out->fields = left.fields;
    for (const auto &f : right.fields)
    {
      out->fields.push_back(f);
    }
    const size_t ln = left.fields.size();
    const size_t rn = right.fields.size();
    const std::string kind = node.joinKind.empty() ? "middle" : node.joinKind;
    const bool want_left = (kind == "left" || kind == "middle");
    const bool want_right_outer = (kind == "right");
    const bool is_inner = (kind == "middle");

    std::vector<bool> right_matched(right.rows.size(), false);

    // 嵌套循环连接：ON 条件为空的退化情形按笛卡尔积处理
    for (const auto &lrow : left.rows)
    {
      bool matched = false;
      for (size_t j = 0; j < right.rows.size(); ++j)
      {
        const auto &rrow = right.rows[j];
        std::vector<storage::Value> combined;
        combined.reserve(ln + rn);
        combined.insert(combined.end(), lrow.begin(), lrow.end());
        combined.insert(combined.end(), rrow.begin(), rrow.end());

        bool pass = true;
        if (node.onExpr != nullptr)
        {
          EvalRow er;
          er.fields = &out->fields;
          er.values = &combined;
          const DbStatus es = ExprEval::EvalPredicate(*node.onExpr, er, &pass);
          if (!es.ok())
          {
            return es;
          }
        }
        if (pass)
        {
          out->rows.push_back(std::move(combined));
          matched = true;
          right_matched[j] = true;
        }
      }
      if (!matched && !is_inner && want_left)
      {
        std::vector<storage::Value> combined = lrow;
        combined.insert(combined.end(), rn, storage::Value::Null());
        out->rows.push_back(std::move(combined));
      }
    }

    if (want_right_outer)
    {
      for (size_t j = 0; j < right.rows.size(); ++j)
      {
        if (right_matched[j])
        {
          continue;
        }
        std::vector<storage::Value> combined(ln, storage::Value::Null());
        combined.insert(combined.end(), right.rows[j].begin(), right.rows[j].end());
        out->rows.push_back(std::move(combined));
      }
    }
    DbLogDebug(logcat::kExec, "Join(" + kind + ") 输出 " + std::to_string(out->rows.size()) + " 行");
    return DbStatus::Ok();
  }

  DbStatus Executor::OpUnion(const cella::CELLA_PlanNode &node, const ExecContext &ctx, RowSet *out)
  {
    if (node.children.size() != 2)
    {
      return DbStatus::Error(DbCode::kInternal, "Union 期望 2 个子算子");
    }
    RowSet left;
    RowSet right;
    DbStatus s = Run(*node.children[0], ctx, &left);
    if (!s.ok())
    {
      return s;
    }
    s = Run(*node.children[1], ctx, &right);
    if (!s.ok())
    {
      return s;
    }
    // 编译器未强制两臂列数一致（README 实现决策 #5）：此处按左臂列数对齐，
    // 不足补 NULL、多余截断，并记录一条告警。
    if (left.fields.size() != right.fields.size())
    {
      DbLogWarn(logcat::kExec, "UNION 两臂列数不一致（" + std::to_string(left.fields.size()) +
                                   " vs " + std::to_string(right.fields.size()) + "），按左臂对齐");
    }
    out->fields = std::move(left.fields);
    const size_t arity = out->fields.size();
    out->rows = std::move(left.rows);
    for (const auto &rrow : right.rows)
    {
      std::vector<storage::Value> aligned(arity, storage::Value::Null());
      for (size_t i = 0; i < arity && i < rrow.size(); ++i)
      {
        aligned[i] = rrow[i];
      }
      out->rows.push_back(std::move(aligned));
    }
    return DbStatus::Ok();
  }

  // ═════════════════════════════════════════════════════════════
  // WAL：行变更记录的写入（P2.1 / P2.2）
  // ═════════════════════════════════════════════════════════════

  void Executor::AppendWalRow(txn_id_t txn, wal::RecordType type, const std::string &table,
                              const storage::Rid &rid,
                              const std::vector<storage::Value> &before,
                              const std::vector<storage::Value> &after)
  {
    if (wal_ == nullptr || txn == kInvalidTxnId)
    {
      return;
    }
    wal::WalRecord r;
    r.type = type;
    r.txn_id = txn;
    r.table = table;
    r.page_id = rid.page_id;
    r.before = before;
    r.after = after;
    (void)wal_->Append(r);
  }

  // ── 恢复期索引处理：清空 / 重建（P2）────────────────────────

  DbStatus Executor::ClearIndexes(const std::set<std::string> &tables)
  {
    StorageGuard guard(storage_mutex_);
    for (const auto &t : tables)
    {
      const CatalogTable *meta = catalog_->FindTable(t);
      if (meta == nullptr || CatalogManager::IsProtectedSystemTable(meta->name))
      {
        continue;
      }
      std::vector<IndexHandle> indexes;
      const DbStatus os = OpenTableIndexes(*meta, &indexes);
      if (!os.ok())
      {
        continue; // 索引打不开就算了，重建阶段会再试一次并真正报错
      }
      for (auto &ix : indexes)
      {
        if (ix.tree == nullptr)
        {
          continue;
        }
        std::vector<std::string> keys;
        const storage::Status ss = ix.tree->ScanAll(&keys);
        if (!ss.ok())
        {
          continue;
        }
        for (const auto &k : keys)
        {
          bool removed = false;
          (void)ix.tree->Remove(k, &removed);
        }
      }
    }
    return DbStatus::Ok();
  }

  DbStatus Executor::RebuildIndexes(const std::set<std::string> &tables)
  {
    const DbStatus cs = ClearIndexes(tables); // 先清干净，再按表数据逐行重建
    if (!cs.ok())
    {
      return cs;
    }
    StorageGuard guard(storage_mutex_);
    for (const auto &t : tables)
    {
      const CatalogTable *meta = catalog_->FindTable(t);
      if (meta == nullptr || CatalogManager::IsProtectedSystemTable(meta->name))
      {
        continue;
      }
      std::vector<IndexHandle> indexes;
      const DbStatus os = OpenTableIndexes(*meta, &indexes);
      if (!os.ok())
      {
        return os;
      }
      if (indexes.empty())
      {
        continue;
      }
      std::shared_ptr<storage::TableHeap> heap;
      const storage::Status oh = storage_->open_table(meta->name, &heap);
      if (!oh.ok())
      {
        return FromStorage(oh, "恢复后重建索引 " + meta->name);
      }
      for (auto it = heap->begin(); it != heap->end(); ++it)
      {
        const DbStatus is = IndexRowInsert(&indexes, *meta, it->values(), it.rid());
        if (!is.ok())
        {
          return is;
        }
      }
    }
    return DbStatus::Ok();
  }

  // ═════════════════════════════════════════════════════════════
  // 恢复期重放（P2.4 redo）
  // ═════════════════════════════════════════════════════════════

  bool Executor::LocateRowForRecovery(const CatalogTable &table,
                                      const std::vector<storage::Value> &values,
                                      storage::Rid *rid, storage::Record *row)
  {
    std::shared_ptr<storage::TableHeap> heap;
    const storage::Status os = storage_->open_table(table.name, &heap);
    if (!os.ok())
    {
      return false;
    }
    const int pk_idx = table.PrimaryKeyColumnIndex();
    const bool by_pk = pk_idx >= 0 && static_cast<size_t>(pk_idx) < values.size();
    for (auto it = heap->begin(); it != heap->end(); ++it)
    {
      const std::vector<storage::Value> &vals = it->values();
      bool hit = false;
      if (by_pk && static_cast<size_t>(pk_idx) < vals.size())
      {
        // 有主键：按主键定位 —— 主键在 UPDATE 前后通常不变，这是唯一可靠的逻辑身份
        hit = wal::ValueEqual(vals[static_cast<size_t>(pk_idx)],
                              values[static_cast<size_t>(pk_idx)]);
      }
      else
      {
        hit = wal::ValuesEqual(vals, values);
      }
      if (hit)
      {
        if (rid != nullptr)
        {
          *rid = it.rid();
        }
        if (row != nullptr)
        {
          *row = *it;
        }
        return true;
      }
    }
    return false;
  }

  DbStatus Executor::RecoveryInsertRow(const std::string &table,
                                       const std::vector<storage::Value> &values)
  {
    StorageGuard guard(storage_mutex_);
    const CatalogTable *meta = catalog_->FindTable(table);
    if (meta == nullptr)
    {
      return DbStatus::Error(DbCode::kTableNotFound, "恢复重做：表不存在 " + table);
    }
    if (CatalogManager::IsProtectedSystemTable(meta->name))
    {
      // 系统表（目录/索引元数据）不进 WAL —— DDL 后必定紧跟一次存盘点，它已经落盘了
      return DbStatus::Ok();
    }
    storage::Rid dummy;
    if (LocateRowForRecovery(*meta, values, &dummy, nullptr))
    {
      return DbStatus::Ok(); // 已经生效过，重放什么也不做
    }
    std::vector<IndexHandle> indexes;
    const DbStatus os = OpenTableIndexes(*meta, &indexes);
    if (!os.ok())
    {
      return os;
    }
    storage::Record rec;
    for (const auto &v : values)
    {
      rec.AddValue(v);
    }
    storage::Rid new_rid;
    const storage::Status s = storage_->insert_record(meta->name, rec, &new_rid);
    if (!s.ok())
    {
      return FromStorage(s, "恢复重做插入 " + meta->name);
    }
    return IndexRowInsert(&indexes, *meta, rec.values(), new_rid);
  }

  DbStatus Executor::RecoveryDeleteRow(const std::string &table,
                                       const std::vector<storage::Value> &before)
  {
    StorageGuard guard(storage_mutex_);
    const CatalogTable *meta = catalog_->FindTable(table);
    if (meta == nullptr)
    {
      return DbStatus::Error(DbCode::kTableNotFound, "恢复重做：表不存在 " + table);
    }
    if (CatalogManager::IsProtectedSystemTable(meta->name))
    {
      return DbStatus::Ok();
    }
    storage::Rid rid;
    storage::Record row;
    if (!LocateRowForRecovery(*meta, before, &rid, &row))
    {
      return DbStatus::Ok(); // 已经被删掉了（或后续还有记录会删它）→ 无需动作
    }
    std::vector<IndexHandle> indexes;
    const DbStatus os = OpenTableIndexes(*meta, &indexes);
    if (!os.ok())
    {
      return os;
    }
    const storage::Status s = storage_->delete_record(meta->name, rid);
    if (!s.ok())
    {
      return FromStorage(s, "恢复重做删除 " + meta->name);
    }
    return IndexRowDelete(&indexes, *meta, row.values(), rid);
  }

  DbStatus Executor::RecoveryReplaceRow(const std::string &table,
                                        const std::vector<storage::Value> &before,
                                        const std::vector<storage::Value> &after)
  {
    StorageGuard guard(storage_mutex_);
    const CatalogTable *meta = catalog_->FindTable(table);
    if (meta == nullptr)
    {
      return DbStatus::Error(DbCode::kTableNotFound, "恢复重做：表不存在 " + table);
    }
    if (CatalogManager::IsProtectedSystemTable(meta->name))
    {
      return DbStatus::Ok();
    }
    // ① 已经是新版本 → 本次更新早就生效了
    storage::Rid after_rid;
    storage::Record after_row;
    if (LocateRowForRecovery(*meta, after, &after_rid, &after_row) &&
        wal::ValuesEqual(after_row.values(), after))
    {
      return DbStatus::Ok();
    }
    // ② 还是旧版本 → 删旧插新（与运行时 UPDATE 同一套动作 + 同一套索引维护）
    storage::Rid old_rid;
    if (!LocateRowForRecovery(*meta, before, &old_rid, nullptr))
    {
      // 两个版本都不在：这行后来被别的记录删掉了，跳过即可
      return DbStatus::Ok();
    }
    std::vector<IndexHandle> indexes;
    const DbStatus os = OpenTableIndexes(*meta, &indexes);
    if (!os.ok())
    {
      return os;
    }
    const storage::Status ds = storage_->delete_record(meta->name, old_rid);
    if (!ds.ok())
    {
      return FromStorage(ds, "恢复重做更新(删旧) " + meta->name);
    }
    storage::Record rec;
    for (const auto &v : after)
    {
      rec.AddValue(v);
    }
    storage::Rid new_rid;
    const storage::Status is = storage_->insert_record(meta->name, rec, &new_rid);
    if (!is.ok())
    {
      return FromStorage(is, "恢复重做更新(插新) " + meta->name);
    }
    return IndexRowUpdate(&indexes, *meta, before, rec.values(), old_rid, new_rid);
  }

} // namespace cella::db
