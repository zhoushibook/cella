// executor.h —— 计划执行器：遍历编译器产出的计划树，调度算子完成数据存取。
//
// 在整体系统中的位置：
//
//   SQL 文本 →[编译器]→ AST → 计划树（可执行 IR）→[本模块]→ 存储层 IStorage
//                                                        ↓
//                                                  结果集 QueryResult
//
// 它消费的是计划节点（而不是原始 AST）：算子种类、子节点关系、谓词表达式、
// 排序键、投影表达式、行数上限……都来自 CELLA_PlanNode 的「执行期附属信息」。
// 只有 DDL/DML 需要列定义/插入值/赋值这类全量类型化信息，此时通过节点上的
// stmt 回指取 AST（计划树里放不下，也不该重复）。
//
// 执行约定：
//   * 每个语句的执行都在一个事务上下文（ExecContext）内进行；锁在扫描/写表时
//     按需申请，由事务管理器在提交/回滚时统一释放（严格 2PL）。
//   * 存储层声明为非线程安全，故所有 IStorage 调用都在同一把递归互斥量内进行。
//     加锁顺序固定为「先数据库锁、后存储互斥量」，两条路径不会交叉等待。
//   * 计划树内的 stmt/tableRef 指针指向调用方持有的 AST，调用期间必须存活。
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string>
#include <utility>
#include <vector>

#include "cella/cella_planner.h"
#include "cella/db/auth/privilege.h"
#include "cella/db/catalog/catalog_manager.h"
#include "cella/db/exec/query_result.h"
#include "cella/db/exec/row_set.h"
#include "cella/db/txn/transaction.h"
#include "cella/db/wal/wal_types.h"
#include "cella/storage/api/i_storage.h"
#include "cella/storage/common/record.h"
#include "cella/storage/common/types.h"
#include "cella/storage/index/b_plus_tree.h"

namespace cella::db {

class AuthStore;

// ── 访问路径（P1.3 + P1.6）──────────────────────────────────
//
// 计划树里的表访问节点统一是 `SeqScan`（这是编译器打印的 golden 契约，不能动）。
// 「到底走堆扫描还是索引扫描」由执行器在**运行时**决定：只有执行层同时看得到
// 索引元数据（cella_index）、表的列类型和真实行数，而编译器看不到。
// 这样既拿到了索引加速，又保持了计划文本逐字节兼容。
enum class AccessPath {
  kSeqScan,          // 全表堆扫描 + 谓词过滤
  kIndexScan,        // 索引定位 + 回表取整行（非覆盖）
  kIndexOnlyScan,    // 索引覆盖扫描：只读索引，不回表
  kRowidLookup,      // `rowid = N` 直接物理定位（既有优化，也纳入同一套选择框架）
};

// 一次表访问的最终选择结果（也是 EXPLAIN 的输出载体）。
struct AccessPathChoice {
  AccessPath path = AccessPath::kSeqScan;
  std::string index_name;        // 走索引时用的索引名；否则为空
  std::string index_column;      // 索引列名
  std::string reason;            // 人类可读的选择理由（EXPLAIN 直接展示）
  // 索引扫描的边界（列值形态，nullptr/未设置表示无界）
  bool has_lower = false;
  bool has_upper = false;
  bool lower_inclusive = true;
  bool upper_inclusive = true;
  storage::Value lower;
  storage::Value upper;
  bool equality = false;         // 等值定位（比范围更省）
  // 复合索引「全键等值」：每列都有 `列 = 常量` 下推（按索引声明序排列）。
  // 非空时 ScanIndexRids 走前缀扫描原语（ScanPrefix），不再用 lower/upper。
  // 最左前缀规则：只约束了前几列的谓词暂不下推（第一版只做全键等值）。
  std::vector<storage::Value> eq_tuple;
  // 范围选择率 = 谓词区间宽度 / 索引列值域宽度（0 = 无统计，用经验值）。
  // 由 ChooseAccessPath 从索引的 min/max（首末叶子键）算出 —— 索引有序，
  // 这等价于一份免费的一维统计。没有它，窄范围与宽范围会被估成同一选择率。
  double range_span = 0.0;
  // 代价估算（P1.6 填充；P1.3 阶段先用简化估算，见 EstimateAccessPath）
  double est_rows = 0.0;         // 预计产出行数
  double est_cost = 0.0;         // 预计代价（抽象单位）

  bool uses_index() const {
    return path == AccessPath::kIndexScan || path == AccessPath::kIndexOnlyScan;
  }
  // EXPLAIN 里的单行描述
  std::string Describe() const;
};

// 语句执行的上下文：事务 + 触碰表记录
struct ExecContext {
  txn_id_t txn_id = kInvalidTxnId;
  Transaction* txn = nullptr;  // 可为空（只读探测场景），此时不记录 undo
  // 锁资源名的库前缀（当前库名）：锁资源记作 "<db>.<table>"，
  // 避免引擎切换数据库后两个库的同名表共享同一把锁（假冲突/假死锁）。
  std::string lock_scope;
  // 语句是否引用了 rowid 伪列：只有引用时才在扫描结果末尾追加它，
  // 从而保证 `get *` 的输出与不引用 rowid 的语句完全不受影响。
  // 放在上下文里（而不是 Executor 成员）是因为引擎可能被多会话并发使用。
  bool with_rowid = false;
  // ── 访问控制（表级读写的判定就在 LockTable 这个必经点上）──
  bool auth_enabled = false;    // 关掉时一律跳过检查
  bool is_admin = false;        // 管理员全放行
  std::string user;             // 当前登录用户（auth_enabled 时有效）

  bool recording() const { return txn != nullptr; }
};

// 语句是否在任何位置引用了 rowid 伪列（投影 / 条件 / 分组 / 排序 / SET 表达式）。
// 会话层用它决定是否让扫描附加 rowid（见 ExecContext::with_rowid）。
bool StmtRefersRowid(const cella::CELLA_Stmt* st);

class Executor : public wal::IUndoApplier {
 public:
  Executor(storage::IStorage* storage, CatalogManager* catalog, TxnManager* txn_manager,
           LockManager* locks, std::recursive_mutex* storage_mutex);

  // 执行一条语句的计划；out 必须非空。
  DbStatus Execute(const cella::CELLA_PlanNode& plan, const ExecContext& ctx, QueryResult* out);

  // 绑定身份库（访问控制判定用；不接管所有权）。引擎在打开后调用一次即可。
  void AttachAuth(const AuthStore* auth) { auth_ = auth; }

  // ── WAL 绑定（P2）──────────────────────────────────────────
  // 行变更记录由执行器在「改动刚发生」的当下写入（执行器才知道前后像），
  // 事务的 begin/commit/abort 记录由 TxnManager 写 —— 两边共用同一个 WalManager。
  void AttachWal(wal::WalManager* wal) { wal_ = wal; }
  wal::WalManager* wal() const { return wal_; }

  // ── 恢复期重放（P2.4 redo / P2.5 undo 的物理落点）──────────
  // 与运行时 DML 的唯一区别是**幂等**：重做一条已经生效的记录什么也不做，
  // 找不到目标行也只是跳过（说明后续还有一条记录会把它删掉/加回来）。
  // 幂等是必须的 —— 崩溃时哪些脏页已经落盘是不确定的，重放必然遇到「已生效」的记录。
  // 索引随表行一起维护，恢复结束后索引与表数据天然一致。
  DbStatus RecoveryInsertRow(const std::string& table,
                             const std::vector<storage::Value>& values);
  DbStatus RecoveryDeleteRow(const std::string& table,
                             const std::vector<storage::Value>& before);
  DbStatus RecoveryReplaceRow(const std::string& table,
                              const std::vector<storage::Value>& before,
                              const std::vector<storage::Value>& after);

  // ── 恢复期的索引处理（P2）──────────────────────────────────
  // WAL 只记表行的逻辑变更，索引是**派生数据**，不进日志。但索引页同样会被缓冲池
  // 淘汰到磁盘 —— 崩溃后索引可能停在「半个状态」：有的键在、有的不在。
  // 于是恢复这样处理：
  //   * 重做/撤销之前：把受影响表的索引项全部清空（避免残键导致误判唯一冲突）；
  //   * 重做/撤销之后：按最终表数据整体重建（保证索引与表逐行一致）。
  // 代价是 O(n log n)，只在真正跑恢复时付出。
  DbStatus ClearIndexes(const std::set<std::string>& tables);
  DbStatus RebuildIndexes(const std::set<std::string>& tables);

  // ── IUndoApplier：撤销 = 反向重放（P2.5）────────────────────
  DbStatus UndoInsert(const std::string& table,
                      const std::vector<storage::Value>& row) override {
    return RecoveryDeleteRow(table, row);
  }
  DbStatus UndoDelete(const std::string& table,
                      const std::vector<storage::Value>& row) override {
    return RecoveryInsertRow(table, row);
  }
  DbStatus UndoUpdate(const std::string& table, const std::vector<storage::Value>& new_row,
                      const std::vector<storage::Value>& old_row) override {
    return RecoveryReplaceRow(table, new_row, old_row);
  }

  // ── undo 补偿期间的索引维护（P1.5）──────────────────────────
  // 事务回滚只补偿表行，索引会与表数据分叉。本方法是 TxnManager::UndoIndexHooks
  // 的执行器侧实现：回滚过程中每撤销一步，就把对应索引项一并修正。
  // 由 DbEngine 在装配时注册给 TxnManager。
  class IndexUndoAdapter : public TxnManager::UndoIndexHooks {
   public:
    explicit IndexUndoAdapter(Executor* owner) : owner_(owner) {}
    void OnUndoInsertDeleted(const std::string& table, const storage::Rid& rid) override;
    void OnUndoRowRestored(const std::string& table, const storage::Rid& rid,
                           const storage::Record& record) override;

   private:
    Executor* owner_;
  };
  IndexUndoAdapter* undo_adapter() { return &undo_adapter_; }

  // 只跑查询算子子树（GET 的各类算子也可单独驱动，便于测试）
  DbStatus Run(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);

  // 统计：执行的算子次数（可观测「计划驱动」确实发生了）
  size_t operator_calls() const { return operator_calls_; }
  void ResetOperatorCalls() { operator_calls_ = 0; }

  // ── 索引维护观测（P1.5）──────────────────────────────────────
  // DML 每维护一条索引项记一次；测试据此断言「索引确实被同步维护」，
  // 而不是只能间接从查询结果推断。
  struct IndexMaintenanceStats {
    size_t inserts = 0;   // 索引项插入次数
    size_t deletes = 0;   // 索引项删除次数
    size_t updates = 0;   // 同一列值未变的「删除 + 重插」（DELETE_INSERT）
    size_t violations = 0;  // 唯一性冲突次数（含主键与唯一二级索引）
  };
  const IndexMaintenanceStats& index_stats() const { return index_stats_; }
  void ResetIndexStats() { index_stats_ = IndexMaintenanceStats{}; }

  // ── 访问路径观测（P1.3）────────────────────────────────────
  // 每发生一次表访问就记一条（含 SeqScan 与索引扫描），供 EXPLAIN 与测试断言。
  // 之所以记在执行器上而不是语句结果里：计划文本必须保持与改造前逐字节一致，
  // EXPLAIN 是**额外**通道，不能污染 QueryResult 或 plan_text。
  struct AccessPathRecord {
    std::string table;   // 真实表名
    AccessPathChoice choice;
    size_t rows_out = 0;   // 该路径实际产出的行数
    size_t pages_read = 0; // 索引路径访问的索引页数（近似，0 = 未统计）
  };
  const std::vector<AccessPathRecord>& access_paths() const { return access_paths_; }
  void ResetAccessPaths() { access_paths_.clear(); }

  // 解析一条语句的访问路径选择（不执行）：供 EXPLAIN 使用。
  // 传入的是**已优化的计划树**；返回每个表访问节点的选择（自下而上，与算子树同序）。
  DbStatus Explain(const cella::CELLA_PlanNode& plan, std::vector<AccessPathRecord>* out);

  // 把访问路径选择树渲染成缩进文本（EXPLAIN 的最终展示）。
  // 形如：
  //   Project [name]
  //     Filter (age > 18)
  //       IndexScan [student] using student_age_idx (age = 20)   rows≈2 cost≈4.0
  //
  // 非 const 的原因：它会先 RegisterNeededColumns(plan) 登记本语句的列需求，
  // 再跑 ChooseAccessPath。**这一步不能省**：ExplAIN 与真正执行必须看到
  // 同一份列需求，否则 index-only scan 在 EXPLAIN 里会被保守地降级成回表，
  // 报表与实跑不一致（P1.6 覆盖扫描用例就是这么暴露出来的）。
  // 它改的只是 needed_cols_ 这个纯缓存，不触碰任何数据。
  std::string ExplainText(const cella::CELLA_PlanNode& plan);

  // 只读版本：不做列需求登记，直接按当前缓存渲染。
  // 用于「已经登记过、只想再渲染一次」的场景（如把选择树追加到 EXPLAIN 输出）。
  std::string ExplainTextNoRegister(const cella::CELLA_PlanNode& plan) const;

 private:
  // ── 语句级算子 ──
  DbStatus ExecCreateTable(const cella::CELLA_PlanNode& plan, const ExecContext& ctx,
                           QueryResult* out);
  DbStatus ExecDropTable(const cella::CELLA_PlanNode& plan, const ExecContext& ctx, QueryResult* out);
  DbStatus ExecCreateIndex(const cella::CELLA_PlanNode& plan, const ExecContext& ctx,
                           QueryResult* out);
  DbStatus ExecDropIndex(const cella::CELLA_PlanNode& plan, const ExecContext& ctx, QueryResult* out);
  // ── P5：DDL 演进 ────────────────────────────────────────────
  DbStatus ExecAlterTable(const cella::CELLA_PlanNode& plan, const ExecContext& ctx, QueryResult* out);
  DbStatus ExecTruncateTable(const cella::CELLA_PlanNode& plan, const ExecContext& ctx,
                             QueryResult* out);
  DbStatus ExecInsert(const cella::CELLA_PlanNode& plan, const ExecContext& ctx, QueryResult* out);
  DbStatus ExecDelete(const cella::CELLA_PlanNode& plan, const ExecContext& ctx, QueryResult* out);
  DbStatus ExecUpdate(const cella::CELLA_PlanNode& plan, const ExecContext& ctx, QueryResult* out);
  DbStatus ExecGet(const cella::CELLA_PlanNode& plan, const ExecContext& ctx, QueryResult* out);

  // ── 查询算子（与计划节点一一对应）──
  DbStatus OpSeqScan(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);
  // IndexScan：由用户在计划里显式给出索引（"IndexScan" 节点）时直接驱动。
  // 正常运行时不走这里 —— 计划树里只有 SeqScan，OpSeqScan 内部会按访问路径
  // 选择结果改走索引（见 ResolveAccessPath）。本算子存在是为了：
  //   1) 单测可以直接构造 IndexScan 计划驱动索引扫描的各种边界；
  //   2) EXPLAIN 与执行共用同一条索引扫描代码路径，避免两套实现漂移。
  DbStatus OpIndexScan(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);
  DbStatus OpFilter(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);
  DbStatus OpProject(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);
  DbStatus OpDistinct(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);
  DbStatus OpAggregate(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);
  DbStatus OpSort(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);
  DbStatus OpLimit(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);
  DbStatus OpPage(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);
  DbStatus OpJoin(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);
  DbStatus OpUnion(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);

  // 只允许一个子节点的算子的统一入口
  DbStatus RunSingleChild(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);

  // ── 工具 ──
  // 加表锁。need = 本次锁对应的数据权限（访问控制关闭/管理员/系统表会短路放行）：
  // DML 各自传 kInsert/kUpdate/kDelete，读传 kRead，DDL 传 kNone（已由会话层判定）。
  // 参数**必填**，这样将来新增算子时编译器会逼着调用点明确表态，不会静默放行。
  DbStatus LockTable(const std::string& table, LockMode mode, const ExecContext& ctx,
                     TablePriv need);
  // 表级权限判定（LockTable 内部调用）
  DbStatus CheckTablePrivilege(const std::string& table, const ExecContext& ctx, TablePriv need);
  // 扫描一张表并按谓词过滤，收集 (Rid, 记录)（DELETE/UPDATE 两阶段修改用）
  DbStatus ScanMatching(const CatalogTable& table, const cella::CELLA_Expr* pred, bool with_rowid,
                        std::vector<std::pair<storage::Rid, storage::Record>>* out);
  // 谓词恰为 `rowid = <整数>` 时按物理地址直达目标行（省掉全表扫描）。
  // 返回 true = 已得出结论（可能为空）；false = 不适用/存储出错，调用方退回 ScanMatching。
  bool DirectByRowid(const CatalogTable& table, const cella::CELLA_Expr* pred,
                     std::vector<std::pair<storage::Rid, storage::Record>>* out);
  // 目标行收集：先试 rowid 直达，否则全表扫描 + 谓词过滤（DELETE/UPDATE 共用）
  DbStatus CollectTargets(const CatalogTable& table, const cella::CELLA_Expr* pred, bool with_rowid,
                          std::vector<std::pair<storage::Rid, storage::Record>>* out);
  // 解析 SeqScan 的 detail 文本 "[name alias]"（tableRef 缺失时的兜底）
  static bool ParseTableDisplay(const std::string& detail, std::string* name, std::string* alias);

  // ── 主键自动索引（P1.4）──────────────────────────────────────
  // CREATE TABLE 后为有主键的表建唯一索引（无主键则是空操作）。
  // 索引名固定为 <table>_pk，登记进 cella_index 系统表 → 自动获得持久化、
  // DROP TABLE 级联清理、以及 DML 索引维护。幂等：已存在同名索引时直接返回。
  DbStatus CreatePrimaryIndex(const CatalogTable& table);
  // 主键索引的命名规则（执行器与测试共用，避免魔法字符串散落）
  static std::string PrimaryIndexName(const std::string& table);
  // 该表当前是否已有主键自动索引
  bool HasPrimaryIndex(const std::string& table) const;

  // ── ALTER TABLE / TRUNCATE 的重建原语（P5）────────────────────
  // 「整表重建」：把 old_meta 的物理表按 new_meta 的结构重写一遍。
  //   col_map[i] = 新表第 i 列的数据来自旧表第几列；-1 = 新列（老行填 NULL）。
  //   copy_rows  = false 表示只换结构不搬数据（TRUNCATE：= 原地清空并释放数据页）。
  // 为什么必须重建（而不是原地改 schema）：记录二进制以「列数 + 位图 + 逐列值」
  // 编码，反序列化会校验列数 ⇔ schema（见 slotted_record_serializer.cpp），
  // 列数一变老字节流就解不出来；存储层也没有原地改列的能力。于是走
  // 「导出全部行 → 换 schema 重建 → 按 col_map 回填」这条最直接的路径。
  // 副作用：行定位（页号 + 槽位）全变 → 表的索引全部失效，须由调用方随后重建。
  // 须在 storage_mutex_ 临界区内调用。new_first_page 非空时回写新表首数据页。
  DbStatus RebuildTableRows(const CatalogTable& old_meta, const CatalogTable& new_meta,
                            const std::vector<int>& col_map, bool copy_rows,
                            uint32_t* new_first_page);

  // 在 table 上按 cols 建一棵索引树并回填（不改任何元数据）。
  //   unique           —— 校验列值元组无重复（重复返回 kUniqueViolation）
  //   require_not_null —— 校验索引列无 NULL（主键用；命中返回 kNotNullViolation）
  // 成功时 root_out 回写新树的根页号。须在 storage_mutex_ 临界区内调用。
  DbStatus BuildIndexTree(const CatalogTable& table, const std::vector<std::string>& cols,
                          bool unique, bool require_not_null, uint32_t* root_out);

  // 重建一张表的全部（已快照的）索引：为每条索引建新树并把元数据指向新根。
  //   rename_from/rename_to —— 表改名时同步索引的 table 字段与 <表>_pk 索引名；
  //                           空字符串表示表名未变。
  // 须在 storage_mutex_ 临界区内、且目录已换到新表之后调用。
  DbStatus RebuildTableIndexes(const CatalogTable& new_meta,
                               const std::vector<CatalogIndex>& index_metas,
                               const std::string& rename_from, const std::string& rename_to);

  // 在另一张表上执行「按表名删索引元数据」（表改名导致 <表>_pk 也要跟着改名时用）。
  // 须在 storage_mutex_ 临界区内调用。
  DbStatus RenameIndexMeta(const std::string& old_index_name, const CatalogIndex& index);

  // ALTER TABLE 六个动作的实现。共用 ExecAlterTable 已做好的前置：
  // 表存在性/写保护校验、排他锁、old_meta 与索引清单的快照。
  DbStatus AlterAddColumn(const CatalogTable& old_meta,
                          const std::vector<CatalogIndex>& indexes,
                          const cella::CELLA_Stmt& st, QueryResult* out);
  DbStatus AlterDropColumn(const CatalogTable& old_meta,
                           const std::vector<CatalogIndex>& indexes,
                           const cella::CELLA_Stmt& st, QueryResult* out);
  DbStatus AlterRenameTable(const CatalogTable& old_meta,
                            const std::vector<CatalogIndex>& indexes,
                            const cella::CELLA_Stmt& st, QueryResult* out);
  DbStatus AlterRenameColumn(const CatalogTable& old_meta,
                             const std::vector<CatalogIndex>& indexes,
                             const cella::CELLA_Stmt& st, QueryResult* out);
  DbStatus AlterAddPrimaryKey(const CatalogTable& old_meta,
                              const std::vector<CatalogIndex>& indexes,
                              const cella::CELLA_Stmt& st, QueryResult* out);
  DbStatus AlterDropPrimaryKey(const CatalogTable& old_meta,
                               const std::vector<CatalogIndex>& indexes,
                               const cella::CELLA_Stmt& st, QueryResult* out);

  // 表内行数（重建前判断「老行能否补出新列的值」用）。须在 storage_mutex_ 内调用。
  size_t CountRowsOf(const std::string& table) const;

  // ── WAL（P2）──────────────────────────────────────────────
  // 写一条行变更记录。rid 只取页号用于重建脏页表；行定位靠 before/after 的内容。
  void AppendWalRow(txn_id_t txn, wal::RecordType type, const std::string& table,
                    const storage::Rid& rid, const std::vector<storage::Value>& before,
                    const std::vector<storage::Value>& after);

  // 恢复期按「主键（有则）/ 整行内容（无则）」定位一行；命中返回 true。
  // 必须在 storage_mutex_ 临界区内调用。
  bool LocateRowForRecovery(const CatalogTable& table,
                            const std::vector<storage::Value>& values, storage::Rid* rid,
                            storage::Record* row);

  // ── 索引维护（P1.5）──────────────────────────────────────────  // 表上全部有效索引的运行时句柄：元数据 + 已 Attach 的 B+ 树。
  // 键按首个索引列下标升序，便于多索引维护时保持稳定顺序。
  struct IndexHandle {
    const CatalogIndex* meta = nullptr;
    std::unique_ptr<storage::BPlusTree> tree;
    // 索引列在表内的下标（按索引声明序；单列索引 = 1 个元素）。
    // 列已被删 → 该索引整条失效（跳过维护），不会出现部分下标无效。
    std::vector<int> columns;
  };

  // 打开一张表的全部索引（元数据 + B+ 树句柄）。须在 storage_mutex_ 临界区内调用。
  // 表上没有索引时返回空 vector 且成功。
  DbStatus OpenTableIndexes(const CatalogTable& table, std::vector<IndexHandle>* out);

  // 把一个已（反）规范化的行值编码成该索引的叶子键。
  // row_values 必须与表列一一对齐（含全部被索引列）。
  // 编码输入是 CoerceValue 截断后的同一行值 —— DELETE 旧键与 INSERT 新键
  // 两侧字节必然一致（VARCHAR 截断场景的一致性由同一数据源保证）。
  static bool IndexKeyOf(const IndexHandle& ix, const std::vector<storage::Value>& row_values,
                         const storage::Rid& rid, std::string* leaf_key);

  // 唯一性检查：行值元组是否已被「rid 之外」的行占用。
  // 实现是**字节前缀比较**（ScanPrefix 圈出同元组的叶子键），不经过解码 ——
  // 编码（含 NULL 位图）是单射，字节相等 ⇔ 元组相等。
  // NULL 语义（与单列时代一致，已文档化）：元组任一列为 NULL → 不参与唯一
  // 判定，直接放行（标准 SQL 行为；PK 列隐含 NOT NULL，不受影响）。
  DbStatus IndexValueFree(const IndexHandle& ix, const std::vector<storage::Value>& row_values,
                          const storage::Rid& rid, bool* busy);

  // 向全部索引插入某行的索引项（唯一索引先查重）。冲突返回 kUniqueViolation / kPrimaryKeyViolation。
  DbStatus IndexRowInsert(std::vector<IndexHandle>* indexes, const CatalogTable& table,
                          const std::vector<storage::Value>& row_values, const storage::Rid& rid);
  // 从全部索引删除某行的索引项。
  DbStatus IndexRowDelete(std::vector<IndexHandle>* indexes, const CatalogTable& table,
                          const std::vector<storage::Value>& row_values, const storage::Rid& rid);
  // 行内容变化时的索引维护：取值变化的索引做 DELETE_INSERT，未变的索引原地不动。
  DbStatus IndexRowUpdate(std::vector<IndexHandle>* indexes, const CatalogTable& table,
                          const std::vector<storage::Value>& before,
                          const std::vector<storage::Value>& after, const storage::Rid& old_rid,
                          const storage::Rid& new_rid);
  // UPDATE 的唯一性预检（只读，不动树）。必须在「删旧行」之前调用，这样冲突时
  // 表和索引都处于未改动状态，不需要依赖回滚来收尾。
  DbStatus IndexRowCheckUpdate(const std::vector<IndexHandle>& indexes, const CatalogTable& table,
                               const std::vector<storage::Value>& before,
                               const std::vector<storage::Value>& after,
                               const storage::Rid& old_rid);

  // 用索引判定某表在「按 rowid 定位的旧行」是否仍存在（回滚后一致性校验用）。
  DbStatus IndexVerifyRow(const std::vector<IndexHandle>& indexes, const CatalogTable& table,
                          const std::vector<storage::Value>& row_values, const storage::Rid& rid,
                          bool* present);

  // undo 钩子的内部实现：按表名重新打开索引句柄再做维护（回滚路径上没有
  // 现成的句柄可用，且可能要撤销多张表的改动）。
  void UndoIndexDropRow(const std::string& table, const storage::Rid& rid);
  void UndoIndexRebuildRow(const std::string& table, const storage::Rid& rid,
                           const storage::Record& record);

  // ── 访问路径选择（P1.3）──────────────────────────────────────
  // 判断一个谓词能否被某个索引「下推」成索引区间。能则填好 choice 的边界与
  // equality 字段并返回 true。只识别可安全下推的形状：
  //     <索引列> <op> <常量>  /  <常量> <op> <索引列>   (op ∈ =, <, <=, >, >=)
  // 以及上述条件的 AND 组合（左右两侧分别尝试，区间取交集）。
  // 不识别 OR / 函数调用 / 列与列比较 —— 下推不了就老老实实全表扫描。
  bool TryIndexRange(const CatalogTable& table, const CatalogIndex& index, int column,
                     const cella::CELLA_Expr* pred, AccessPathChoice* choice) const;

  // 复合索引的谓词下推（第一版：全键等值）。要求谓词（AND 树）为**每一列**
  // 都提供 `列 = 常量` 约束（列匹配大小写不敏感），否则不下推 ——
  // 部分前缀匹配列为第二步扩展。成功时填 eq_tuple 并返回 true。
  bool TryCompositeEquality(const CatalogTable& table, const CatalogIndex& index,
                            const std::vector<int>& columns, const cella::CELLA_Expr* pred,
                            AccessPathChoice* choice) const;

  // 谓词里是否引用了某个列（用于判断索引覆盖扫描的可行性）。
  static bool PredRefsColumn(const cella::CELLA_Expr* pred, const std::string& column);
  // 谓词是否引用了「索引列之外」的表列（引用则必须回表，不能 index-only）。
  // 接收索引列集合（单列索引 = 1 个元素），复合索引天然支持。
  static bool PredRefsColumnOutside(const cella::CELLA_Expr* pred, const CatalogTable& table,
                                    const std::vector<int>& index_columns);

  // 为一次表访问挑选访问路径。pred 是该表上方最近的过滤谓词（可为空）。
  // 决策顺序：
  //   1) rowid 等值 → kRowidLookup（最省，直接物理定位）；
  //   2) 有可用索引且索引区间能把行数压下来 → kIndexScan / kIndexOnlyScan；
  //   3) 否则 kSeqScan。
  // 「能不能压下来」由 EstimateAccessPath 的代价比较决定（P1.6 完善）。
  DbStatus ChooseAccessPath(const CatalogTable& table, const cella::CELLA_Expr* pred,
                            bool need_rowid, AccessPathChoice* out);

  // 估算某条访问路径的代价与产出行数（P1.6 代价模型）。
  //
  // 输入：
  //   row_count —— 表行数（估算的基数来源）
  //   ix        —— 索引的实测几何（高度、叶子页数）。仅当 choice 走索引时使用；
  //                全表扫描时传默认值即可。
  //
  // 为什么索引几何要**从外面传进来**而不是在函数里现算：
  //   ① 代价估算必须是纯函数（无 I/O），否则 EXPLAIN 会变得比执行还贵；
  //   ② 一次 ChooseAccessPath 里要对多个候选索引分别估算，几何可以在
  //      选路径之前一次性取好（O(树高)，见 BPlusTree::Height）。
  struct IndexStats {
    double height = 0.0;      // 树高（叶子层 = 1）；0 表示未知 → 退回默认值
    double leaf_pages = 0.0;  // 叶子页数；0 表示未知 → 按行数估算
    bool known() const { return height > 0.0; }
  };
  void EstimateAccessPath(const CatalogTable& table, size_t row_count, const IndexStats& ix,
                          const AccessPathChoice& choice, double* est_rows,
                          double* est_cost) const;

  // 收集计划树里所有 SeqScan 节点，与「其上方最近的过滤谓词」配对。
  // 索引下推的本质就是：把 Filter 的谓词交给它正下方的表访问节点。
  struct ScanPredicate {
    const cella::CELLA_PlanNode* scan = nullptr;  // SeqScan 节点
    const cella::CELLA_Expr* pred = nullptr;      // 该扫描上方最近的谓词（可空）
    bool need_rowid = false;
  };
  static void CollectScanPredicates(const cella::CELLA_PlanNode& node, const cella::CELLA_Expr* pred,
                                    bool with_rowid, std::vector<ScanPredicate>* out);

  // 查找表上可用的索引（元数据 + 列下标）。列已被删掉的索引跳过。
  DbStatus UsableIndexes(const CatalogTable& table, std::vector<IndexHandle>* out);

  // 用索引首末叶子键（= 该列 min/max）估算范围选择率；不适合时返回 0。
  double ComputeRangeSpan(storage::BPlusTree* tree, const AccessPathChoice& choice) const;

  // 表的行数（存储层没有行数统计，只能扫一遍数；空表返回 0）。
  // 代价模型需要它，P1.6 会在执行器上做缓存以避免重复计数。
  size_t CountTableRows(const CatalogTable& table);

  // ── EXPLAIN 渲染 ──
  // 递归打印计划树，表访问节点显示最终选中的访问路径（而不是计划里的字面算子）。
  // 非 const：内部要经 ChooseAccessPath（它可能刷新 needed_cols_ 缓存）。
  void ExplainNode(const cella::CELLA_PlanNode& node, const cella::CELLA_Expr* pred, int depth,
                   std::string* out) const;

  // ── 表访问算子的公共实现（P1.3）───────────────────────────────
  // SeqScan 与 IndexScan 共用：先做访问路径选择，再按结果走堆扫描或索引扫描。
  // explicit_index_scan = true 表示计划节点本身就是 IndexScan（单测/EXPLAIN 构造），
  // 此时强制走索引框架（没有可用区间时退化为整索引扫 + 回表）。
  DbStatus OpTableAccess(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out,
                         bool explicit_index_scan);

  // 在索引上按区间定位候选 Rid（等值 / 范围 / 无界全索引）。
  DbStatus ScanIndexRids(const CatalogTable& table, const IndexHandle& ix,
                         const AccessPathChoice& choice, std::vector<storage::Rid>* out);

  // 表上的第一个有效索引名（显式 IndexScan 但无区间时兜底）
  std::string FirstIndexName(const CatalogTable& table) const;

  // ── 谓词登记（Filter → 下方表访问节点）──────────────────────
  // 计划形状固定为 Filter -> Scan，但执行是自底向上的：Scan 先被调用，
  // 此时它还不知道上面的过滤条件。于是 Filter 在驱动子算子**之前**把谓词
  // 登记进来，Scan 取出后据此做索引下推。键是表名（大写）。
  // 这是执行器内部状态，必须在语句执行结束时清空 —— 见 ResetScanPredicates。
  void PushScanPredicate(const std::string& table, const cella::CELLA_Expr* pred);
  const cella::CELLA_Expr* TakeScanPredicate(const std::string& table);
  void ResetScanPredicates() { scan_pred_.clear(); }
  // 把谓词登记到一棵子树中所有表访问节点（Join 之下停止，见实现注释）
  void PushPredicateToScans(const cella::CELLA_PlanNode& node, const cella::CELLA_Expr* pred);

  // ── 语句列需求（判定 index-only scan 用）────────────────────
  // 表访问节点位于算子树最底层，看不到上方 Project/Sort 需要哪些列。
  // 因此在语句开始执行前，由 ExecGet 把「本语句该表需要的全部列」登记进来。
  // 规则：索引列的集合 ⊇ 语句需要的列 → 可以 index-only；否则必须回表。
  // 保守取向：**只要不确定就回表**（多读一次只是慢，漏列是错的）。
  void SetNeededColumns(const std::string& table, std::vector<std::string> columns);
  // 该表在本语句中是否用到了「索引列集合之外」的列（true = 需要回表）。
  // 单列与复合索引统一走列集合判定。
  bool StmtNeedsOtherColumn(const std::string& table,
                            const std::vector<int>& index_columns) const;
  // 遍历优化后计划，把各表需要的列登记进 needed_cols_
  void RegisterNeededColumns(const cella::CELLA_PlanNode& plan);
  void CollectNeededColumns(const cella::CELLA_PlanNode& node, Executor* self);
  void RegisterColumnsForSubtree(const cella::CELLA_PlanNode& node,
                                 const std::vector<std::string>& cols);

  storage::IStorage* storage_;
  CatalogManager* catalog_;
  TxnManager* txn_manager_;
  LockManager* locks_;
  std::recursive_mutex* storage_mutex_;
  wal::WalManager* wal_ = nullptr;   // 预写日志（可为空 = 不记日志、无崩溃恢复）
  const AuthStore* auth_ = nullptr;  // 访问控制判定（可为空 = 不做检查）
  size_t operator_calls_ = 0;
  IndexMaintenanceStats index_stats_;
  std::vector<AccessPathRecord> access_paths_;  // 最近一次执行的访问路径记录（P1.3）
  std::map<std::string, const cella::CELLA_Expr*> scan_pred_;  // Filter → Scan 的谓词登记表
  std::map<std::string, std::vector<std::string>> needed_cols_;  // 表 → 本语句需要的列（index-only 判定）
  bool explain_mode_ = false;  // true = 只解析访问路径不取数据（Explain 期间）
  IndexUndoAdapter undo_adapter_{this};

  // ── ORDER BY 引用未投影列时的「隐藏排序键」通道 ──
  // 计划形状固定为 Project → Distinct → Sort，而 SQL 语义里 ORDER BY 作用在
  // 投影之前（get name in student ordered age desc 是合法的）。为了不改动计划文本
  // （golden 契约），执行期这样做：
  //   OpSort 先把需要的排序键放进 sort_hint_；
  //   下方的 OpProject 在正常输出列之后追加同序的「隐藏列」（键表达式）；
  //   OpDistinct 只按可见前缀去重（隐藏列不参与 DISTINCT 语义）；
  //   OpSort 排序后把隐藏列裁掉，输出与 SQL 语义一致。
  const std::vector<cella::CELLA_ColName>* sort_hint_ = nullptr;
  size_t visible_fields_ = 0;  // Project 输出的可见列数；0 表示没有隐藏列
};

}  // namespace cella::db
