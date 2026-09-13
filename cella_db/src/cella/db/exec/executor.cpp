#include "cella/db/exec/executor.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include "cella/db/auth/auth_store.h"
#include "cella/db/common/db_logger.h"
#include "cella/db/common/value_bridge.h"
#include "cella/db/exec/expr_eval.h"
#include "cella/storage/common/status.h"
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

    out->tag = "CREATE TABLE " + entry.name;
    DbLogInfo(logcat::kCatalog, out->tag + "（" + std::to_string(entry.columns.size()) + " 列）");
    return DbStatus::Ok();
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
    if (CatalogManager::IsSystemTable(name))
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
    }
    out->tag = "DROP TABLE " + name;
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
    if (CatalogManager::IsSystemTable(name))
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

    // ── 主键唯一性：先一次性收集已有行的键，插入过程中累积比对 ──
    // 语句内累积（而不是逐行重扫）避免批量插入退化为 O(N²)；冲突即返回，
    // 语句级回滚会撤销本语句已插入的行。
    const int pk_idx = meta->PrimaryKeyColumnIndex();
    std::set<std::string> pk_keys;
    if (pk_idx >= 0)
    {
      std::vector<std::pair<storage::Rid, storage::Record>> existing;
      const DbStatus ss = ScanMatching(*meta, nullptr, ctx.with_rowid, &existing);
      if (!ss.ok())
      {
        return ss;
      }
      for (const auto &er : existing)
      {
        pk_keys.insert(KeyOfRow(er.second.values(), std::vector<int>{pk_idx}));
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

      if (pk_idx >= 0)
      {
        const std::string key = KeyOfRow(rec.values(), std::vector<int>{pk_idx});
        if (!pk_keys.insert(key).second)
        {
          return DbStatus::Error(DbCode::kPrimaryKeyViolation,
                                 "主键冲突: 列 " + meta->columns[static_cast<size_t>(pk_idx)].name +
                                     " 的值已存在（表 " + name + "）");
        }
      }

      storage::Rid rid;
      const storage::Status s = storage_->insert_record(name, rec, &rid);
      if (!s.ok())
      {
        return FromStorage(s, "插入 " + name);
      }
      if (ctx.recording())
      {
        UndoRecord u;
        u.kind = UndoRecord::Kind::kInsert;
        u.table = name;
        u.rid = rid;
        ctx.txn->AddUndo(std::move(u));
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
    if (CatalogManager::IsSystemTable(name))
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
    size_t removed = 0;
    for (const auto &h : hits)
    {
      const storage::Status s = storage_->delete_record(name, h.first);
      if (!s.ok())
      {
        return FromStorage(s, "删除 " + name);
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
    if (CatalogManager::IsSystemTable(name))
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

    // ── 主键唯一性（仅当更新涉及主键列时检查）──
    // 语义是「把命中行的主键值改成新值」：新值不能与**未命中行**冲突，
    // 命中行之间也不能改成同一个新值；「排除自身」用命中 Rid 集合实现。
    const int pk_idx = meta->PrimaryKeyColumnIndex();
    bool pk_assigned = false;
    for (const auto &a : assigns)
    {
      if (a.index == pk_idx && pk_idx >= 0)
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
        pk_taken.insert(KeyOfRow(row.second.values(), std::vector<int>{pk_idx}));
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
        const std::string key = KeyOfRow(fresh.values(), std::vector<int>{pk_idx});
        if (!pk_taken.insert(key).second)
        {
          return DbStatus::Error(DbCode::kPrimaryKeyViolation,
                                 "主键冲突: 更新后的 " +
                                     meta->columns[static_cast<size_t>(pk_idx)].name +
                                     " 值与其它行重复（表 " + name + "）");
        }
      }

      const storage::Status ds = storage_->delete_record(name, h.first);
      if (!ds.ok())
      {
        return FromStorage(ds, "更新(删旧) " + name);
      }
      storage::Rid new_rid;
      const storage::Status is = storage_->insert_record(name, fresh, &new_rid);
      if (!is.ok())
      {
        return FromStorage(is, "更新(插新) " + name);
      }
      if (ctx.recording())
      {
        UndoRecord u;
        u.kind = UndoRecord::Kind::kUpdate;
        u.table = name;
        u.rid = new_rid;     // 回滚时先删这一行
        u.before = h.second; // 再重插旧内容
        ctx.txn->AddUndo(std::move(u));
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

  DbStatus Executor::OpSeqScan(const cella::CELLA_PlanNode &node, const ExecContext &ctx,
                               RowSet *out)
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
      return DbStatus::Error(DbCode::kInternal, "SeqScan 缺少表信息: " + node.detail);
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

    StorageGuard guard(storage_mutex_);
    std::shared_ptr<storage::TableHeap> heap;
    const storage::Status os = storage_->open_table(real, &heap);
    if (!os.ok())
    {
      return FromStorage(os, "打开表 " + real);
    }
    const std::string qualifier = alias.empty() ? real : alias;
    out->fields = MakeFields(*meta, qualifier, ctx.with_rowid);
    for (auto it = heap->begin(); it != heap->end(); ++it)
    {
      // 只有语句引用了 rowid 时才在末尾补上（与 MakeFields 的伪列对齐）
      out->rows.push_back(ValuesWithRowid(it->values(), it.rid(), ctx.with_rowid));
    }
    return DbStatus::Ok();
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
    RowSet in;
    const DbStatus s = RunSingleChild(node, ctx, &in);
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

  DbStatus Executor::OpAggregate(const cella::CELLA_PlanNode &node, const ExecContext &ctx,
                                 RowSet *out)
  {
    RowSet in;
    const DbStatus s = RunSingleChild(node, ctx, &in);
    if (!s.ok())
    {
      return s;
    }
    out->fields = std::move(in.fields);
    if (node.groupKeys.empty())
    {
      out->rows = std::move(in.rows);
      return DbStatus::Ok();
    }
    std::vector<int> idx;
    idx.reserve(node.groupKeys.size());
    // 注意：in.fields 已被搬进 out->fields，故后续解析一律基于 out->fields
    for (const auto &k : node.groupKeys)
    {
      const int i = ResolveSortKey(*out, k);
      if (i < 0)
      {
        return DbStatus::Error(DbCode::kColumnNotFound, "分组列不存在: " + KeyName(k));
      }
      idx.push_back(i);
    }
    // 本方言无聚合函数：GROUP BY 实现为「按分组键去重，每组保留首行」
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

} // namespace cella::db
