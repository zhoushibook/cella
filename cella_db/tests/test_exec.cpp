// test_exec.cpp —— 执行层测试：CRUD、类型与约束、连接、排序/分页/去重/分组/并集、
//                  计划优化对执行结果的影响、错误码与算子调度。
#include <string>

#include "mini_test.h"
#include "test_util.h"

using namespace cella::db;
using testutil::Engine;
using testutil::RowsText;

namespace {

// 建一张标准测试表并灌入 5 行
void Seed(Engine* e) {
  const ScriptReport r = e->Run(
      "CREATE TABLE student(id INT NOT NULL, name VARCHAR(16) NOT NULL, age INT, score DOUBLE);"
      "INSERT INTO student VALUES (1,'Alice',20,88.5),(2,'Bob',19,76.0),(3,'Carol',22,94.5),"
      "(4,'Dave',21,61.0),(5,'Eve',20,80.0);");
  MT_CHECK(r.all_ok());
}

}  // namespace

MT_TEST(执行_建表与目录) {
  Engine e("exec_ddl");
  MT_CHECK(e.opened);
  const ScriptReport r = e.Run("CREATE TABLE t(id INT NOT NULL, s VARCHAR(8));");
  MT_CHECK(r.all_ok());
  MT_EQ(r.statements.size(), 1u);
  MT_EQ(r.statements[0].kind, std::string("CREATE TABLE"));
  MT_EQ(r.statements[0].result.tag, std::string("CREATE TABLE t"));

  const CatalogTable* meta = e.engine.catalog().FindTable("t");
  MT_CHECK(meta != nullptr);
  MT_EQ(meta->columns.size(), 2u);
  MT_CHECK(meta->columns[0].not_null);
  MT_EQ(meta->columns[1].len, 8);
  // 首数据页已由存储层补齐（诊断用）
  MT_CHECK(meta->first_page_id != cella::storage::kInvalidPageId);

  // 重复建表（语义阶段即拦下：SEM-302）
  const ScriptReport dup = e.Run("CREATE TABLE T(id INT);");
  MT_CHECK(!dup.all_ok());
  MT_CHECK(!dup.statements[0].compile_errors.empty());
  MT_CHECK(dup.statements[0].compile_errors[0].code == std::string("SEM-302"));
  MT_CHECK(dup.statements[0].status.code() == DbCode::kSqlError);

  // 删表
  MT_CHECK(e.Run("DROP TABLE t;").all_ok());
  MT_CHECK(e.engine.catalog().FindTable("t") == nullptr);
  // 删不存在的表（语义阶段即拦下：SEM-301）
  const ScriptReport drop2 = e.Run("DROP TABLE t;");
  MT_CHECK(!drop2.all_ok());
  MT_CHECK(drop2.statements[0].compile_errors[0].code == std::string("SEM-301"));
}

MT_TEST(执行_插入与全表扫描) {
  Engine e("exec_insert");
  Seed(&e);
  const ScriptReport r = e.Run("get * in student ordered id asc;");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements[0].result),
        std::string("1|Alice|20|88.5\n2|Bob|19|76\n3|Carol|22|94.5\n4|Dave|21|61\n5|Eve|20|80"));
  MT_EQ(testutil::ColsText(r.statements[0].result), std::string("id|name|age|score"));
}

MT_TEST(执行_投影过滤排序与取前N) {
  Engine e("exec_query");
  Seed(&e);
  // 投影 + 过滤 + 排序
  MT_EQ(RowsText(e.Run("get name, age in student limit age >= 20 and score > 80 ordered age asc;")
                     .statements[0]
                     .result),
        std::string("Alice|20\nCarol|22"));
  // 排序 + among
  MT_EQ(RowsText(e.Run("get name in student ordered score desc among 2;").statements[0].result),
        std::string("Carol\nAlice"));
  // ORDER BY 引用未投影列（Sort 位于 Project 之上 → 走隐藏排序列通道）
  MT_EQ(RowsText(e.Run("get name in student ordered age desc among 2;").statements[0].result),
        std::string("Carol\nDave"));
  MT_EQ(RowsText(e.Run("get name in student ordered score asc among 1;").statements[0].result),
        std::string("Dave"));
  // 未排序时按存储顺序（插入顺序）返回
  MT_EQ(RowsText(e.Run("get id in student;").statements[0].result),
        std::string("1\n2\n3\n4\n5"));
}

MT_TEST(执行_去重与分组) {
  Engine e("exec_distinct");
  e.Run(
      "CREATE TABLE t(id INT, grp VARCHAR(8));"
      "INSERT INTO t VALUES (1,'A'),(2,'A'),(3,'B'),(4,'B'),(5,'C');");
  MT_EQ(RowsText(e.Run("get distinct grp in t ordered grp asc;").statements[0].result),
        std::string("A\nB\nC"));
  // 真 GROUP BY：投影只能是分组键或聚合函数。仅投影分组键时每组输出一行
  MT_EQ(RowsText(e.Run("get grp in t grouped grp ordered grp asc;").statements[0].result),
        std::string("A\nB\nC"));
  // having 在分组之后过滤
  MT_EQ(RowsText(e.Run("get grp in t grouped grp having grp = 'B';").statements[0].result),
        std::string("B"));
  // 非分组键、非聚合列出现在投影里 → 语义阶段拒绝（SEM-322）
  const std::string bad = e.Run("get id, grp in t grouped grp;").statements[0].status.message();
  MT_CHECK(bad.find("SEM-322") != std::string::npos);
}

MT_TEST(执行_COUNT聚合) {
  Engine e("exec_count");
  e.Run(
      "CREATE TABLE s(id INT, name VARCHAR(8), age INT);"
      "INSERT INTO s VALUES (1,'a',10),(2,'b',20),(3,'c',NULL),(4,'d',10);");
  // COUNT(*)：所有行（含 NULL）
  MT_EQ(RowsText(e.Run("get count(*) in s;").statements[0].result), std::string("4"));
  // COUNT(col)：只计非 NULL
  MT_EQ(RowsText(e.Run("get count(age) in s;").statements[0].result), std::string("3"));
  // 分组计数
  MT_EQ(RowsText(e.Run("get age, count(*) in s grouped age ordered age asc;")
                     .statements[0]
                     .result),
        std::string("10|2\n20|1\nNULL|1"));
  // 分组内 COUNT(col)（NULL 不计入）
  MT_EQ(RowsText(e.Run("get age, count(age) in s grouped age ordered age asc;")
                     .statements[0]
                     .result),
        std::string("10|2\n20|1\nNULL|0"));
  // COUNT(*) 与 COUNT(col) 同处分组
  MT_EQ(RowsText(e.Run("get age, count(*), count(name) in s grouped age ordered age asc;")
                     .statements[0]
                     .result),
        std::string("10|2|2\n20|1|1\nNULL|1|1"));
  // 空表 COUNT(*) = 0，且仍输出一行
  Engine e2("exec_count_empty");
  e2.Run("CREATE TABLE z(id INT);");
  MT_EQ(RowsText(e2.Run("get count(*) in z;").statements[0].result), std::string("0"));
}

MT_TEST(执行_连接三种方向) {
  Engine e("exec_join");
  e.Run(
      "CREATE TABLE course(id INT, title VARCHAR(16));"
      "CREATE TABLE student(id INT, name VARCHAR(16), cid INT);"
      "INSERT INTO course VALUES (1,'Math'),(2,'Physics'),(3,'Chem');"
      "INSERT INTO student VALUES (1,'Alice',1),(2,'Bob',3),(3,'Carol',2),(4,'Dave',99);");

  // 内连接：Dave(cid=99) 与 Chem 无学生 → 各被剔除
  MT_EQ(RowsText(e.Run("get s.name, c.title in student s middle join course c on s.cid = c.id "
                       "ordered s.name asc;")
                     .statements[0]
                     .result),
        std::string("Alice|Math\nBob|Chem\nCarol|Physics"));

  // 左外连接：Dave 保留，右半补 NULL
  MT_EQ(RowsText(e.Run("get s.name, c.title in student s left join course c on s.cid = c.id "
                       "ordered s.name asc;")
                     .statements[0]
                     .result),
        std::string("Alice|Math\nBob|Chem\nCarol|Physics\nDave|NULL"));

  // 右外连接：顺序按 c.title 升序（Chem < Math < Physics）
  MT_EQ(RowsText(e.Run("get s.name, c.title in student s right join course c on s.cid = c.id "
                       "ordered c.title asc;")
                     .statements[0]
                     .result),
        std::string("Bob|Chem\nAlice|Math\nCarol|Physics"));
}

MT_TEST(执行_并集) {
  Engine e("exec_union");
  Seed(&e);
  MT_EQ(RowsText(e.Run("get name in student limit id = 1 union get name in student limit id = 2;")
                     .statements[0]
                     .result),
        std::string("Alice\nBob"));
}

MT_TEST(执行_分页) {
  Engine e("exec_page");
  Seed(&e);
  MT_EQ(RowsText(e.Run("get id in student ordered id asc page 1, 2;").statements[0].result),
        std::string("1\n2"));
  MT_EQ(RowsText(e.Run("get id in student ordered id asc page 2, 2;").statements[0].result),
        std::string("3\n4"));
  MT_EQ(RowsText(e.Run("get id in student ordered id asc page 3, 2;").statements[0].result),
        std::string("5"));
  MT_EQ(RowsText(e.Run("get id in student ordered id asc page 4, 2;").statements[0].result),
        std::string(""));
  // among 先限总行数，page 在其内分页
  MT_EQ(RowsText(e.Run("get id in student ordered id asc among 3 page 2, 2;").statements[0].result),
        std::string("3"));
}

MT_TEST(执行_更新与删除) {
  Engine e("exec_update");
  Seed(&e);

  const ScriptReport up = e.Run("UPDATE student SET score = 100.0, age = 30 limit id = 1;");
  MT_CHECK(up.all_ok());
  MT_EQ(up.statements[0].result.affected, 1u);
  MT_EQ(RowsText(e.Run("get id, age, score in student limit id = 1;").statements[0].result),
        std::string("1|30|100"));

  // 未赋值列保持原值
  MT_EQ(RowsText(e.Run("get name in student limit id = 1;").statements[0].result),
        std::string("Alice"));

  // 批量更新（多行命中）
  const ScriptReport up2 = e.Run("UPDATE student SET age = 18 limit age >= 19;");
  MT_CHECK(up2.all_ok());
  MT_EQ(up2.statements[0].result.affected, 5u);
  MT_EQ(RowsText(e.Run("get distinct age in student;").statements[0].result), std::string("18"));

  // 删除
  const ScriptReport del = e.Run("DELETE in student limit age = 18;");
  MT_CHECK(del.all_ok());
  MT_EQ(del.statements[0].result.affected, 5u);
  MT_EQ(RowsText(e.Run("get id in student;").statements[0].result), std::string(""));

  // 删除零行
  const ScriptReport del2 = e.Run("DELETE in student limit id = 42;");
  MT_CHECK(del2.all_ok());
  MT_EQ(del2.statements[0].result.affected, 0u);
}

MT_TEST(执行_表达式与常量折叠) {
  Engine e("exec_expr");
  e.Run(
      "CREATE TABLE t(a INT, b INT);"
      "INSERT INTO t VALUES (10,3),(20,7);");
  // 算术：整数除法不整除时退化为浮点
  MT_EQ(RowsText(e.Run("get a + b as s, a - b as d, a * b as m, a / b as q in t ordered a asc;")
                     .statements[0]
                     .result),
        std::string("13|7|30|3.33333\n27|13|140|2.85714"));
  // 常量折叠 + 恒真 Filter 消除（结果集应与直接比较一致）
  MT_EQ(RowsText(e.Run("get a in t limit a > 5 + 4;").statements[0].result),
        std::string("10\n20"));
  MT_EQ(RowsText(e.Run("get a in t limit 1 = 1;").statements[0].result), std::string("10\n20"));
  MT_EQ(RowsText(e.Run("get a in t limit 2 = 3;").statements[0].result), std::string(""));
  // 一元负号
  MT_EQ(RowsText(e.Run("get -a as neg in t limit a = 10;").statements[0].result),
        std::string("-10"));
  // NULL 传播：未提供的列在算术中保持 NULL
  e.Run("CREATE TABLE n(a INT, b INT);INSERT INTO n(a) VALUES (1);");
  MT_EQ(RowsText(e.Run("get a + b as s in n;").statements[0].result), std::string("NULL"));
}

MT_TEST(执行_空表与空结果) {
  Engine e("exec_empty");
  MT_CHECK(e.Run("CREATE TABLE t(a INT);").all_ok());
  const ScriptReport r = e.Run("get * in t;");
  MT_CHECK(r.all_ok());
  MT_EQ(r.statements[0].result.rows.size(), 0u);
  MT_EQ(r.statements[0].result.columns.size(), 1u);
  MT_EQ(r.statements[0].result.tag, std::string("GET 0"));
}

MT_TEST(执行_约束与错误码) {
  Engine e("exec_errors");
  e.Run("CREATE TABLE t(id INT NOT NULL, name VARCHAR(4) NOT NULL);");

  // 省略 NOT NULL 列：语义阶段放行，执行阶段按 DB-506 拒收
  const ScriptReport r1 = e.Run("INSERT INTO t(id) VALUES (1);");
  MT_CHECK(!r1.all_ok());
  MT_CHECK(r1.statements[0].status.code() == DbCode::kNotNullViolation);

  // 显式 NULL 进 NOT NULL 列：语义阶段即拦下（SEM-307）
  const ScriptReport r1b = e.Run("INSERT INTO t VALUES (1,NULL);");
  MT_CHECK(!r1b.all_ok());
  MT_CHECK(!r1b.statements[0].compile_errors.empty());

  // 建表列重复（SEM-304）
  MT_CHECK(!e.Run("CREATE TABLE t2(a INT, a INT);").all_ok());

  // 表不存在（SEM-301）
  MT_CHECK(!e.Run("get * in nope;").all_ok());

  // 列不存在（SEM-303）
  MT_CHECK(!e.Run("get zzz in t;").all_ok());

  // 语法错误（SYN-201）
  const ScriptReport r2 = e.Run("SELECT * FROM t;");
  MT_CHECK(!r2.all_ok());
  MT_CHECK(!r2.statements[0].compile_errors.empty());

  // 运行期：文本超长 → DB-508
  const ScriptReport r3 = e.Run("INSERT INTO t VALUES (1,'toolongstring');");
  MT_CHECK(!r3.all_ok());
  MT_CHECK(r3.statements[0].status.code() == DbCode::kValueTooLong);

  // 运行期：小数入 INT → DB-505
  const ScriptReport r4 = e.Run("INSERT INTO t VALUES (1.5,'ok');");
  MT_CHECK(!r4.all_ok());
  MT_CHECK(r4.statements[0].status.code() == DbCode::kTypeMismatch);

  // 表仍为空（失败语句没有留下半截数据）
  MT_EQ(RowsText(e.Run("get id in t;").statements[0].result), std::string(""));

  // 除零 → DB-511（需要有行才会求值）
  MT_CHECK(e.Run("INSERT INTO t VALUES (1,'ok');").all_ok());
  const ScriptReport r5 = e.Run("get id / 0 as d in t;");
  MT_CHECK(!r5.all_ok());
  MT_CHECK(r5.statements[0].status.code() == DbCode::kDivisionByZero);
}

MT_TEST(执行_算子调度可观测) {
  Engine e("exec_ops");
  Seed(&e);
  // get * 只有 SeqScan：Execute + SeqScan
  const ScriptReport r1 = e.Run("get * in student;");
  MT_CHECK(r1.all_ok());
  MT_EQ(r1.statements[0].operator_calls, 2u);
  MT_CHECK(r1.statements[0].plan_text.find("SeqScan") != std::string::npos);

  // 投影 + 过滤：Filter -> SeqScan 共 3 次算子入口（含语句级一次）
  const ScriptReport r2 = e.Run("get name in student limit age > 19;");
  MT_CHECK(r2.all_ok());
  MT_EQ(r2.statements[0].operator_calls, 4u);  // Execute + Project + Filter + SeqScan
  MT_CHECK(r2.statements[0].plan_text.find("Project") != std::string::npos);
  MT_CHECK(r2.statements[0].plan_text.find("Filter") != std::string::npos);

  // 未优化计划里 10 + 8 保留为算术，优化后折叠为 18
  const ScriptReport r3 = e.Run("get name in student limit age > 10 + 8;");
  MT_CHECK(r3.all_ok());
  MT_CHECK(r3.statements[0].original_plan_text.find("10 + 8") != std::string::npos);
  MT_CHECK(r3.statements[0].plan_text.find("18") != std::string::npos);
  MT_CHECK(r3.statements[0].plan_text.find("10 + 8") == std::string::npos);
}

MT_TEST(执行_大小写不敏感) {
  Engine e("exec_case");
  MT_CHECK(e.Run("CrEaTe TaBlE StUdEnT(Id InT, NaMe VaRcHaR(8));").all_ok());
  MT_CHECK(e.Run("INSERT INTO STUDENT VALUES (1,'Alice');").all_ok());
  MT_EQ(RowsText(e.Run("GET ID, NAME IN student;").statements[0].result),
        std::string("1|Alice"));
  MT_EQ(RowsText(e.Run("get id, name in STUDENT limit name == 'Alice';").statements[0].result),
        std::string("1|Alice"));
}

MT_TEST(执行_列别名与表别名) {
  Engine e("exec_alias");
  Seed(&e);
  MT_EQ(testutil::ColsText(e.Run("get id as num, name as who in student;").statements[0].result),
        std::string("num|who"));
  MT_EQ(RowsText(e.Run("get s.id, s.name in student s limit s.id = 2;").statements[0].result),
        std::string("2|Bob"));
  // 带限定符的排序键（Sort 位于 Project 之上，需按列名退化匹配）
  MT_EQ(RowsText(e.Run("get s.name in student s ordered s.name desc among 1;").statements[0].result),
        std::string("Eve"));
}

MT_TEST(执行_NULL判空) {
  Engine e("exec_isnull");
  // 含 NULL 行：age 为 NULL 的人
  MT_CHECK(e
               .Run("CREATE TABLE p(id INT NOT NULL, name VARCHAR(8) NOT NULL, age INT);"
                    "INSERT INTO p VALUES (1,'Alice',20),(2,'Bob',NULL),(3,'Carol',22);")
               .all_ok());
  // IS NULL 精确筛出 NULL 行（= null 恒 UNKNOWN，查不出任何行）
  MT_EQ(RowsText(e.Run("get name in p limit age is null;").statements[0].result),
        std::string("Bob"));
  // IS NOT NULL 是其补集
  MT_EQ(RowsText(e.Run("get name in p limit age is not null ordered id asc;")
                      .statements[0]
                      .result),
        std::string("Alice\nCarol"));
  // 与普通谓词组合（AND）
  MT_EQ(RowsText(e.Run("get name in p limit age is not null and id > 1;")
                      .statements[0]
                      .result),
        std::string("Carol"));
  // NOT + IS NULL 组合
  MT_EQ(RowsText(e.Run("get name in p limit not age is null and id < 3;")
                      .statements[0]
                      .result),
        std::string("Alice"));
  // 非空值上判空
  MT_EQ(RowsText(e.Run("get name in p limit id is null;").statements[0].result),
        std::string());
}

MT_TEST(执行_主键约束) {
  Engine e("exec_pk");
  MT_CHECK(e.Run("CREATE TABLE s(id INT PRIMARY KEY, name VARCHAR(16));").all_ok());
  // 目录中记下主键，且隐含 NOT NULL
  const CatalogTable *meta = e.engine.catalog().FindTable("s");
  MT_CHECK(meta != nullptr);
  MT_EQ(meta->PrimaryKeyColumnIndex(), 0);
  MT_CHECK(meta->columns[0].not_null);

  MT_CHECK(e.Run("INSERT INTO s VALUES (1,'a'),(2,'b');").all_ok());
  // 重复主键 → DB-516
  MT_CHECK(e.Run("INSERT INTO s VALUES (1,'dup');").statements[0].status.code() ==
           DbCode::kPrimaryKeyViolation);
  // 同一语句内的自冲突也要拦，且已插入的那行随语句级回滚一起撤销
  MT_CHECK(e.Run("INSERT INTO s VALUES (3,'c'),(3,'d');").statements[0].status.code() ==
           DbCode::kPrimaryKeyViolation);
  MT_EQ(RowsText(e.Run("get id in s ordered id asc;").statements[0].result), std::string("1\n2"));
  // NULL 入主键被 NOT NULL 拦（编译期语义检查）
  MT_CHECK(!e.Run("INSERT INTO s VALUES (NULL,'x');").all_ok());

  // 更新主键撞车 → 拒绝；改成自己原值 → 允许（排除自身）
  MT_CHECK(e.Run("UPDATE s SET id = 2 limit id = 1;").statements[0].status.code() ==
           DbCode::kPrimaryKeyViolation);
  MT_CHECK(e.Run("UPDATE s SET id = 1 limit id = 1;").all_ok());
  MT_CHECK(e.Run("UPDATE s SET name = 'z' limit id = 2;").all_ok());
  // 一次更新把多行改成同一主键值 → 拒绝
  MT_CHECK(e.Run("UPDATE s SET id = 9;").statements[0].status.code() ==
           DbCode::kPrimaryKeyViolation);
  // 删除后主键值可重用
  MT_CHECK(e.Run("DELETE in s limit id = 2;").all_ok());
  MT_CHECK(e.Run("INSERT INTO s VALUES (2,'reuse');").all_ok());
  MT_EQ(RowsText(e.Run("get name in s ordered id asc;").statements[0].result),
        std::string("a\nreuse"));

  // 重启后主键定义保留，约束仍然生效
  e.Close();
  MT_CHECK(e.Reopen());
  const CatalogTable *meta2 = e.engine.catalog().FindTable("s");
  MT_CHECK(meta2 != nullptr);
  MT_EQ(meta2->PrimaryKeyColumnIndex(), 0);
  MT_CHECK(e.Run("INSERT INTO s VALUES (1,'dup');").statements[0].status.code() ==
           DbCode::kPrimaryKeyViolation);
}

MT_TEST(执行_rowid伪列) {
  Engine e("exec_rowid");
  // 两行**全列相同**：这正是全列匹配无法区分、只能靠 rowid 精确定位的场景
  MT_CHECK(e
               .Run("CREATE TABLE d(id INT, name VARCHAR(8));"
                    "INSERT INTO d VALUES (1,'same'),(1,'same'),(2,'x');")
               .all_ok());

  // ① get * 不受影响：rowid 不会混进星号展开
  const ScriptReport star = e.Run("get * in d;");
  MT_CHECK(star.all_ok());
  MT_EQ(testutil::ColsText(star.statements[0].result), std::string("id|name"));

  // ② 投影 rowid：两行同值但 rowid 不同
  const ScriptReport r = e.Run("get rowid, name in d;");
  MT_CHECK(r.all_ok());
  MT_EQ(testutil::ColsText(r.statements[0].result), std::string("rowid|name"));
  MT_EQ(r.statements[0].result.rows.size(), 3u);
  const int64_t rid0 = r.statements[0].result.rows[0][0].int64_val;
  const int64_t rid1 = r.statements[0].result.rows[1][0].int64_val;
  MT_CHECK(rid0 > 0 && rid1 > 0);
  MT_CHECK(rid0 != rid1); // 同值行靠 rowid 区分

  // ③ 按 rowid 只删掉其中一行（重复行场景的杀手锏）
  MT_CHECK(e.Run("delete in d limit rowid = " + std::to_string(rid0) + ";").all_ok());
  MT_EQ(RowsText(e.Run("get name in d ordered name asc;").statements[0].result),
        std::string("same\nx"));
  // 另一行的 rowid 仍在
  MT_CHECK(e.Run("get id in d limit rowid = " + std::to_string(rid1) + ";").all_ok());

  // ④ 按 rowid 精确改一行，且改后 rowid 会变（UPDATE = 删旧 + 插新，文档化行为）
  MT_CHECK(e.Run("update d set name = 'z' limit rowid = " + std::to_string(rid1) + ";").all_ok());
  const ScriptReport after = e.Run("get rowid, name in d ordered name asc;");
  MT_CHECK(after.all_ok());
  bool found_old_rid = false;
  for (const auto &row : after.statements[0].result.rows) {
    if (row[0].int64_val == rid1) {
      found_old_rid = true;
    }
  }
  MT_CHECK(!found_old_rid); // 旧 rowid 不再存在

  // ⑤ 不存在的 rowid：空结果、不报错
  MT_CHECK(e.Run("get id in d limit rowid = 999999999;").all_ok());
  MT_EQ(RowsText(e.Run("get id in d limit rowid = 999999999;").statements[0].result),
        std::string());
  const ScriptReport del_none = e.Run("delete in d limit rowid = 999999999;");
  MT_CHECK(del_none.all_ok());
  MT_EQ(del_none.statements[0].result.affected, 0u);

  // ⑥ 非法用法：不能声明 rowid 列、不能对它赋值 / 插入
  MT_CHECK(!e.Run("CREATE TABLE bad(rowid INT);").all_ok());
  MT_CHECK(!e.Run("INSERT INTO d(rowid, id) VALUES (9, 9);").all_ok());
  MT_CHECK(!e.Run("UPDATE d SET rowid = 5;").all_ok());

  // ⑦ 排序键也能用 rowid
  MT_CHECK(e.Run("get id in d ordered rowid desc;").all_ok());
}

MT_TEST(执行_复合主键) {
  Engine e("exec_pk_composite");
  // 表级复合主键：PRIMARY KEY (sid, cid)
  MT_CHECK(e
               .Run("CREATE TABLE enroll(sid INT, cid INT, grade FLOAT, PRIMARY KEY (sid, cid));")
               .all_ok());
  // 目录：两个主键列（声明序），均隐含 NOT NULL
  const CatalogTable *meta = e.engine.catalog().FindTable("enroll");
  MT_CHECK(meta != nullptr);
  const std::vector<int> pk_cols = meta->PrimaryKeyColumns();
  MT_EQ(pk_cols.size(), 2u);
  MT_EQ(pk_cols[0], 0);
  MT_EQ(pk_cols[1], 1);
  MT_CHECK(meta->columns[0].not_null);
  MT_CHECK(meta->columns[1].not_null);
  // 复合主键现在也建唯一 B+ 树（B+ 树已支持复合键）：enroll_pk，
  // 键 = (sid, cid) 按声明序编码，唯一性由索引元组前缀比较保证
  const CatalogIndex *pk_ix = e.engine.catalog().FindIndex("enroll_pk");
  MT_CHECK(pk_ix != nullptr);
  MT_CHECK(pk_ix != nullptr && pk_ix->unique);
  MT_CHECK(pk_ix != nullptr && pk_ix->columns.size() == 2u);

  MT_CHECK(e.Run("INSERT INTO enroll VALUES (1,10,88.5),(1,11,90.0),(2,10,75.0);").all_ok());
  // 组合值重复 → DB-516（首列相同但次列不同则合法）
  MT_CHECK(e.Run("INSERT INTO enroll VALUES (1,10,50.0);").statements[0].status.code() ==
           DbCode::kPrimaryKeyViolation);
  // 任意一个主键列写 NULL 被隐含 NOT NULL 拦下（编译期）
  MT_CHECK(!e.Run("INSERT INTO enroll VALUES (NULL,10,50.0);").all_ok());
  MT_CHECK(!e.Run("INSERT INTO enroll VALUES (1,NULL,50.0);").all_ok());

  // 更新涉及主键列：撞其它行 → 拒绝；改回自身原值 → 允许；多行改成同键 → 拒绝
  // 当前行：(1,10) (1,11) (2,10)
  MT_CHECK(e.Run("UPDATE enroll SET cid = 99 limit sid = 1 and cid = 10;").all_ok()); // → (1,99)
  // (1,99) 改成 cid=11 → 与 (1,11) 的组合键 (1,11) 撞车
  MT_CHECK(e.Run("UPDATE enroll SET cid = 11 limit sid = 1 and cid = 99;").statements[0]
               .status.code() == DbCode::kPrimaryKeyViolation);
  // 改回自身原值 (1,99) → (1,99)：排除自身，放行
  MT_CHECK(e.Run("UPDATE enroll SET cid = 99 limit sid = 1 and cid = 99;").all_ok());
  // 全表改 cid=50 → (1,50) 出现两次（多行改成同一组合键）→ 拒绝
  MT_CHECK(e.Run("UPDATE enroll SET cid = 50;").statements[0].status.code() ==
           DbCode::kPrimaryKeyViolation);

  // 删除后组合键可重用；重启后目录与约束都还在
  MT_CHECK(e.Run("DELETE in enroll limit sid = 1 and cid = 99;").all_ok()); // 行：(1,11) (2,10)
  MT_CHECK(e.Run("INSERT INTO enroll VALUES (1,99,60.0);").all_ok());
  e.Close();
  MT_CHECK(e.Reopen());
  const CatalogTable *meta2 = e.engine.catalog().FindTable("enroll");
  MT_CHECK(meta2 != nullptr);
  MT_EQ(meta2->PrimaryKeyColumns().size(), 2u);
  MT_CHECK(e.Run("INSERT INTO enroll VALUES (1,99,1.0);").statements[0].status.code() ==
           DbCode::kPrimaryKeyViolation);
  MT_EQ(RowsText(e.Run("get cid in enroll ordered cid asc;").statements[0].result),
        std::string("10\n11\n99"));
}
