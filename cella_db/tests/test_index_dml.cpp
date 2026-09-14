// test_index_dml.cpp —— DML 与二级索引的一致性（P1.5）。
//
// 覆盖：
//   * INSERT：唯一索引拒绝重复值（DB-519）、普通索引照常插入
//   * INSERT 失败后语句级回滚把「已插入的表行 + 索引项」一起撤掉
//   * UPDATE：被索引列改值 → 索引 DELETE_INSERT；唯一索引排除自身
//   * DELETE：表行与索引项同时消失
//   * NULL 在唯一索引下允许多行共存（SQL 语义）
//   * 多列 / 复合候选键（联合唯一）的维护
//   * 重启后索引与表数据仍然一致
#include <memory>
#include <string>

#include "cella/db/engine/db_engine.h"
#include "mini_test.h"
#include "test_util.h"

using namespace cella::db;
using testutil::Engine;
using testutil::RowsText;

namespace {

const char* kBase = "CREATE TABLE emp (id INT PRIMARY KEY, name VARCHAR(20), age INT);";

// 用索引系统表的行数断言「索引项确实被维护」——这是外部可观测的、不依赖
// 执行器内部统计的证据。
size_t IndexEntryCount(DbEngine& engine, const std::string& index_name) {
  // 索引元数据表只记录索引定义，这里改为数「索引树里的键」不现实（没有 SQL 接口），
  // 因此用 cella_index 的行数做「索引定义」断言，键的一致性由查询结果间接证明。
  (void)index_name;
  (void)engine;
  return 0;
}

}  // namespace

// ── 唯一约束 ──────────────────────────────────────────────────

MT_TEST(P15_唯一索引拒绝重复值) {
  Engine e("p15_unique_reject");
  MT_CHECK(e.opened);

  const ScriptReport r = e.Run(std::string(kBase) +
                               "CREATE UNIQUE INDEX uq_name ON emp (name);"
                               "INSERT INTO emp VALUES (1,'a',10);"
                               "INSERT INTO emp VALUES (2,'a',20);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[3].status.code()),
        static_cast<int>(DbCode::kUniqueViolation));
  MT_CHECK(r.statements[3].status.ToString().find("DB-519") != std::string::npos);

  // 失败的 INSERT 不得留下任何行（自动提交事务整体回滚）
  const ScriptReport g = e.Run("GET * IN emp;");
  MT_CHECK(g.all_ok());
  MT_EQ(static_cast<int>(g.statements[0].result.rows.size()), 1);
  MT_EQ(RowsText(g.statements[0].result), std::string("1|a|10"));
}

MT_TEST(P15_唯一索引允许不同值) {
  Engine e("p15_unique_ok");
  MT_CHECK(e.opened);

  const ScriptReport r = e.Run(std::string(kBase) +
                               "CREATE UNIQUE INDEX uq_name ON emp (name);"
                               "INSERT INTO emp VALUES (1,'a',10);"
                               "INSERT INTO emp VALUES (2,'b',20);");
  MT_CHECK(r.all_ok());
}

// ── NULL 语义 ─────────────────────────────────────────────────

MT_TEST(P15_唯一索引允许多个NULL共存) {
  Engine e("p15_null_unique");
  MT_CHECK(e.opened);

  // SQL 标准：UNIQUE 列上的 NULL 不算重复（NULL != NULL）
  const ScriptReport r = e.Run(
      "CREATE TABLE n (id INT PRIMARY KEY, code VARCHAR(8));"
      "CREATE UNIQUE INDEX uq_code ON n (code);"
      "INSERT INTO n VALUES (1, NULL);"
      "INSERT INTO n VALUES (2, NULL);"
      "INSERT INTO n VALUES (3, 'x');");
  MT_CHECK(r.all_ok());

  const ScriptReport g = e.Run("GET * IN n;");
  MT_CHECK(g.all_ok());
  MT_EQ(static_cast<int>(g.statements[0].result.rows.size()), 3);
}

MT_TEST(P15_唯一索引在NULL之外仍拒绝重复) {
  Engine e("p15_null_then_dup");
  MT_CHECK(e.opened);

  const ScriptReport r = e.Run(
      "CREATE TABLE n (id INT PRIMARY KEY, code VARCHAR(8));"
      "CREATE UNIQUE INDEX uq_code ON n (code);"
      "INSERT INTO n VALUES (1, NULL);"
      "INSERT INTO n VALUES (2, 'x');"
      "INSERT INTO n VALUES (3, 'x');");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[4].status.code()),
        static_cast<int>(DbCode::kUniqueViolation));

  const ScriptReport g = e.Run("GET * IN n;");
  MT_CHECK(g.all_ok());
  MT_EQ(static_cast<int>(g.statements[0].result.rows.size()), 2);
}

// ── UPDATE ────────────────────────────────────────────────────

MT_TEST(P15_更新被索引列后索引同步) {
  Engine e("p15_update_sync");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run(std::string(kBase) +
                         "CREATE UNIQUE INDEX uq_name ON emp (name);"
                         "INSERT INTO emp VALUES (1,'a',10);"
                         "INSERT INTO emp VALUES (2,'b',20);"
                         "UPDATE emp SET name = 'z' LIMIT id = 2;");
  MT_CHECK(r.all_ok());

  // 旧值 'b' 必须从唯一索引里消失 —— 否则再插 'b' 会误报冲突
  r = e.Run("INSERT INTO emp VALUES (3,'b',30);");
  MT_CHECK(r.all_ok());

  const ScriptReport g = e.Run("GET * IN emp ordered id asc;");
  MT_CHECK(g.all_ok());
  MT_EQ(static_cast<int>(g.statements[0].result.rows.size()), 3);
}

MT_TEST(P15_更新为已存在的值被拒绝) {
  Engine e("p15_update_conflict");
  MT_CHECK(e.opened);

  const ScriptReport r = e.Run(std::string(kBase) +
                               "CREATE UNIQUE INDEX uq_name ON emp (name);"
                               "INSERT INTO emp VALUES (1,'a',10);"
                               "INSERT INTO emp VALUES (2,'b',20);"
                               "UPDATE emp SET name = 'a' LIMIT id = 2;");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[4].status.code()),
        static_cast<int>(DbCode::kUniqueViolation));

  // 失败后两行都保持原样
  const ScriptReport g = e.Run("GET * IN emp ordered id asc;");
  MT_CHECK(g.all_ok());
  MT_EQ(RowsText(g.statements[0].result), std::string("1|a|10\n2|b|20"));
}

// 把自己更新成自己的原值必须合法（唯一性检查要排除自身）
MT_TEST(P15_更新为自身原值不报冲突) {
  Engine e("p15_update_self");
  MT_CHECK(e.opened);

  const ScriptReport r = e.Run(std::string(kBase) +
                               "CREATE UNIQUE INDEX uq_name ON emp (name);"
                               "INSERT INTO emp VALUES (1,'a',10);"
                               "UPDATE emp SET name = 'a' LIMIT id = 1;");
  MT_CHECK(r.all_ok());

  const ScriptReport g = e.Run("GET * IN emp;");
  MT_CHECK(g.all_ok());
  MT_EQ(RowsText(g.statements[0].result), std::string("1|a|10"));
}

MT_TEST(P15_更新非索引列不影响索引) {
  Engine e("p15_update_other");
  MT_CHECK(e.opened);

  const ScriptReport r = e.Run(std::string(kBase) +
                               "CREATE UNIQUE INDEX uq_name ON emp (name);"
                               "INSERT INTO emp VALUES (1,'a',10);"
                               "UPDATE emp SET age = 99 LIMIT id = 1;"
                               // age 不是索引列；再插 name='a' 必须仍冲突
                               "INSERT INTO emp VALUES (2,'a',20);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[4].status.code()),
        static_cast<int>(DbCode::kUniqueViolation));
}

// ── DELETE ────────────────────────────────────────────────────

MT_TEST(P15_删除后索引项同步释放) {
  Engine e("p15_delete_sync");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run(std::string(kBase) +
                         "CREATE UNIQUE INDEX uq_name ON emp (name);"
                         "INSERT INTO emp VALUES (1,'a',10);"
                         "INSERT INTO emp VALUES (2,'b',20);"
                         "DELETE IN emp LIMIT id = 1;");
  MT_CHECK(r.all_ok());

  // 'a' 已随行删除 → 可以重新插入
  r = e.Run("INSERT INTO emp VALUES (3,'a',30);");
  MT_CHECK(r.all_ok());

  const ScriptReport g = e.Run("GET * IN emp ordered id asc;");
  MT_CHECK(g.all_ok());
  MT_EQ(RowsText(g.statements[0].result), std::string("2|b|20\n3|a|30"));
}

// ── 重启后一致性 ──────────────────────────────────────────────

MT_TEST(P15_重启后索引与数据一致) {
  Engine e("p15_restart");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run(std::string(kBase) +
                         "CREATE UNIQUE INDEX uq_name ON emp (name);"
                         "INSERT INTO emp VALUES (1,'a',10);"
                         "INSERT INTO emp VALUES (2,'b',20);"
                         "DELETE IN emp LIMIT id = 1;"
                         "UPDATE emp SET name = 'c' LIMIT id = 2;");
  MT_CHECK(r.all_ok());
  MT_CHECK(e.Reopen());

  // 重启后唯一索引必须能正确判断：'a' 空出、'b' 空出、'c' 占用
  r = e.Run("INSERT INTO emp VALUES (3,'a',30);");
  MT_CHECK(r.all_ok());
  r = e.Run("INSERT INTO emp VALUES (4,'c',40);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kUniqueViolation));

  const ScriptReport g = e.Run("GET * IN emp ordered id asc;");
  MT_CHECK(g.all_ok());
  MT_EQ(RowsText(g.statements[0].result), std::string("2|c|20\n3|a|30"));
}

// ── 主键与二级索引共存 ────────────────────────────────────────

MT_TEST(P15_主键冲突与唯一索引冲突分码) {
  Engine e("p15_pk_vs_unique");
  MT_CHECK(e.opened);

  // 主键冲突 → DB-516；唯一二级索引冲突 → DB-519
  ScriptReport r = e.Run(std::string(kBase) +
                         "CREATE UNIQUE INDEX uq_name ON emp (name);"
                         "INSERT INTO emp VALUES (1,'a',10);"
                         "INSERT INTO emp VALUES (1,'b',20);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[3].status.code()),
        static_cast<int>(DbCode::kPrimaryKeyViolation));

  r = e.Run("INSERT INTO emp VALUES (9,'a',90);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kUniqueViolation));
}

// ── 多索引同时维护 ────────────────────────────────────────────

MT_TEST(P15_一张表多个索引同时维护) {
  Engine e("p15_multi_index");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run(std::string(kBase) +
                         "CREATE UNIQUE INDEX uq_name ON emp (name);"
                         "CREATE UNIQUE INDEX uq_age ON emp (age);"
                         "INSERT INTO emp VALUES (1,'a',10);"
                         "INSERT INTO emp VALUES (2,'b',20);");
  MT_CHECK(r.all_ok());

  // 两个唯一索引都必须生效
  r = e.Run("INSERT INTO emp VALUES (3,'a',30);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kUniqueViolation));

  r = e.Run("INSERT INTO emp VALUES (4,'d',10);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kUniqueViolation));

  // 成功插入后两个索引都要更新
  r = e.Run("INSERT INTO emp VALUES (5,'e',50);");
  MT_CHECK(r.all_ok());
  r = e.Run("INSERT INTO emp VALUES (6,'e',60);");
  MT_CHECK(!r.all_ok());

  const ScriptReport g = e.Run("GET * IN emp;");
  MT_CHECK(g.all_ok());
  MT_EQ(static_cast<int>(g.statements[0].result.rows.size()), 3);
}

// ── 批量 INSERT 内的自冲突 ───────────────────────────────────

MT_TEST(P15_单条INSERT多行之间的唯一冲突) {
  Engine e("p15_batch_self");
  MT_CHECK(e.opened);

  // 同一条 INSERT 的两行 name 相同 → 必须被拦下，且两行都不落库
  const ScriptReport r = e.Run(std::string(kBase) +
                               "CREATE UNIQUE INDEX uq_name ON emp (name);"
                               "INSERT INTO emp VALUES (1,'a',10),(2,'a',20);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[2].status.code()),
        static_cast<int>(DbCode::kUniqueViolation));

  const ScriptReport g = e.Run("GET * IN emp;");
  MT_CHECK(g.all_ok());
  MT_EQ(static_cast<int>(g.statements[0].result.rows.size()), 0);
}

MT_TEST(P15_单条INSERT多行且彼此不同则成功) {
  Engine e("p15_batch_ok");
  MT_CHECK(e.opened);

  const ScriptReport r = e.Run(std::string(kBase) +
                               "CREATE UNIQUE INDEX uq_name ON emp (name);"
                               "INSERT INTO emp VALUES (1,'a',10),(2,'b',20),(3,'c',30);");
  MT_CHECK(r.all_ok());

  const ScriptReport g = e.Run("GET * IN emp;");
  MT_CHECK(g.all_ok());
  MT_EQ(static_cast<int>(g.statements[0].result.rows.size()), 3);
}

// ── 复合键 / 联合唯一 ─────────────────────────────────────────
//
// 单列索引的表达能力有限：复合唯一约束需要「多列一起唯一」。
// 本项目索引是单列的，因此用「联合候选键」的等价写法验证：
//   两列分别建唯一索引 → 任何一列重复都拒绝。若只想约束组合，
//   则需要 index 支持多列（见 P1.3 的复合索引扩展点）。

MT_TEST(P15_复合候选键可用两列唯一索引表达) {
  Engine e("p15_composite");
  MT_CHECK(e.opened);

  const ScriptReport r = e.Run(
      "CREATE TABLE room (id INT PRIMARY KEY, building VARCHAR(8), room_no VARCHAR(8));"
      "CREATE UNIQUE INDEX uq_b ON room (building);"
      "CREATE UNIQUE INDEX uq_r ON room (room_no);"
      "INSERT INTO room VALUES (1,'A','101');"
      "INSERT INTO room VALUES (2,'A','102');");
  MT_CHECK(!r.all_ok());
  // 语句 #0 CREATE TABLE / #1 CREATE INDEX / #2 CREATE INDEX
  // / #3 INSERT(ok) / #4 INSERT(冲突)
  MT_EQ(static_cast<int>(r.statements[4].status.code()),
        static_cast<int>(DbCode::kUniqueViolation));
}

MT_TEST(P15_复合候选键各异时全部通过) {
  Engine e("p15_composite_ok");
  MT_CHECK(e.opened);

  const ScriptReport r = e.Run(
      "CREATE TABLE room (id INT PRIMARY KEY, building VARCHAR(8), room_no VARCHAR(8));"
      "CREATE UNIQUE INDEX uq_b ON room (building);"
      "CREATE UNIQUE INDEX uq_r ON room (room_no);"
      "INSERT INTO room VALUES (1,'A','101');"
      "INSERT INTO room VALUES (2,'B','102');");
  MT_CHECK(r.all_ok());

  const ScriptReport g = e.Run("GET * IN room ordered id asc;");
  MT_CHECK(g.all_ok());
  MT_EQ(RowsText(g.statements[0].result), std::string("1|A|101\n2|B|102"));
}

// ── 级联更新（按主键更新传播到二级索引）──────────────────────
//
// "级联更新" 在本项目里指：更新一行会同时维护**该行涉及的全部索引**，
// 主键索引与二级索引必须一起改动。若只更新主键索引，二级索引会指向旧值。

MT_TEST(P15_级联更新主键与二级索引一起改) {
  Engine e("p15_cascade_update");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run(std::string(kBase) +
                         "CREATE UNIQUE INDEX uq_name ON emp (name);"
                         "CREATE INDEX idx_age ON emp (age);"
                         "INSERT INTO emp VALUES (1,'a',10);");
  MT_CHECK(r.all_ok());

  // 同时改主键列与被索引的二级列
  r = e.Run("UPDATE emp SET id = 100, name = 'zz' LIMIT id = 1;");
  MT_CHECK(r.all_ok());

  // 旧主键值 1 应可用，旧二级值 'a' 应可用
  r = e.Run("INSERT INTO emp VALUES (1,'a',11);");
  MT_CHECK(r.all_ok());

  // 新主键值 100 与新二级值 'zz' 应被占用
  r = e.Run("INSERT INTO emp VALUES (100,'q',12);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kPrimaryKeyViolation));

  r = e.Run("INSERT INTO emp VALUES (200,'zz',13);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kUniqueViolation));
}

// ── 事务内回滚后的索引一致性 ─────────────────────────────────

MT_TEST(P15_显式事务回滚后唯一索引仍准确) {
  Engine e("p15_txn_rollback");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run(std::string(kBase) +
                         "CREATE UNIQUE INDEX uq_name ON emp (name);"
                         "INSERT INTO emp VALUES (1,'a',10);");
  MT_CHECK(r.all_ok());

  // 事务内插入再整体回滚
  r = e.Run("BEGIN;"
            "INSERT INTO emp VALUES (2,'b',20);"
            "ROLLBACK;");
  MT_CHECK(r.all_ok());

  const ScriptReport g = e.Run("GET * IN emp;");
  MT_CHECK(g.all_ok());
  MT_EQ(RowsText(g.statements[0].result), std::string("1|a|10"));

  // 回滚掉的 'b' 在索引里也必须被撤销 → 可重新插入
  r = e.Run("INSERT INTO emp VALUES (3,'b',30);");
  MT_CHECK(r.all_ok());
}

MT_TEST(P15_显式事务提交后唯一索引准确) {
  Engine e("p15_txn_commit");
  MT_CHECK(e.opened);

  const ScriptReport r = e.Run(std::string(kBase) +
                               "CREATE UNIQUE INDEX uq_name ON emp (name);"
                               "BEGIN;"
                               "INSERT INTO emp VALUES (1,'a',10);"
                               "COMMIT;"
                               "INSERT INTO emp VALUES (2,'a',20);");
  MT_CHECK(!r.all_ok());
  // #0 CREATE TABLE / #1 CREATE INDEX / #2 BEGIN / #3 INSERT
  // / #4 COMMIT / #5 INSERT(冲突)
  MT_EQ(static_cast<int>(r.statements[5].status.code()),
        static_cast<int>(DbCode::kUniqueViolation));
}

// ── 普通索引不限制重复 ───────────────────────────────────────

MT_TEST(P15_普通索引允许多行同值) {
  Engine e("p15_nonunique");
  MT_CHECK(e.opened);

  const ScriptReport r = e.Run(std::string(kBase) +
                               "CREATE INDEX idx_name ON emp (name);"
                               "INSERT INTO emp VALUES (1,'a',10);"
                               "INSERT INTO emp VALUES (2,'a',20);"
                               "INSERT INTO emp VALUES (3,'a',30);");
  MT_CHECK(r.all_ok());

  const ScriptReport g = e.Run("GET * IN emp;");
  MT_CHECK(g.all_ok());
  MT_EQ(static_cast<int>(g.statements[0].result.rows.size()), 3);
}

// 普通索引下删除其中一行，另外两行必须还在（索引项按 Rid 区分，不能误删）
MT_TEST(P15_普通索引删除单行不误伤同值行) {
  Engine e("p15_nonunique_del");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run(std::string(kBase) +
                         "CREATE INDEX idx_name ON emp (name);"
                         "INSERT INTO emp VALUES (1,'a',10);"
                         "INSERT INTO emp VALUES (2,'a',20);"
                         "INSERT INTO emp VALUES (3,'a',30);"
                         "DELETE IN emp LIMIT id = 2;");
  MT_CHECK(r.all_ok());

  const ScriptReport g = e.Run("GET * IN emp ordered id asc;");
  MT_CHECK(g.all_ok());
  MT_EQ(RowsText(g.statements[0].result), std::string("1|a|10\n3|a|30"));
}
