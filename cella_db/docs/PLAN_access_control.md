# 访问控制实现计划（认证 + 授权 + 客户端）

> 目标：为 cella 引入「用户」这一层——认证（你是谁）+ 授权（你能做什么），
> 并把 web 客户端接进来（登录 → 令牌 → 每请求身份）。
>
> 前置事实（已到代码里核实）：
> - 代码库中**没有任何** `user` / `password` / `GRANT` 痕迹 —— 全新特性。
> - 会话层已有成熟的管理语句拦截模式：`IsTxnControl` / `IsDatabaseControl`
>   （`engine/sql_text.{h,cpp}`），**编译器不认识这些语句**。
> - **`Executor::LockTable` 是所有表访问的必经之路**：CREATE/DROP TABLE、INSERT、UPDATE、DELETE
>   走 X 锁，GET（含 join 的每张表）走 S 锁 —— 共 7 个调用点覆盖全部算子。
> - `cella_client` 已落地（HTTP + REST，已有 `/api/session` 与行编辑端点），登录/令牌有现成接入点。
> - 编译器目录（`cella_sql`）与存储层（`cella_storage`）**本特性零改动**。

---

## 0. 验收标准

| 能力 | 验收 |
| --- | --- |
| 认证 | `CREATE USER alice IDENTIFIED BY 'pw';` 后，`--auth` 下用 `alice/pw` 登录成功、错口令失败（DB-801）；`SET PASSWORD` 后旧口令失效；`SHOW USERS` 可见 |
| 授权 | `GRANT get ON main.student TO alice;` 后 alice 可 `get`、`INSERT` 被拒（DB-802）；`REVOKE` 后立即失效；`GRANT ... ON main.*` / `ON *.*` 分级生效；管理员全放行 |
| 管理员 | 首次开启认证自动建 `root`（空口令 + 醒目警告）；不能删最后一个管理员（DB-804）；普通用户执行 `CREATE USER`/`GRANT` → DB-805 |
| 客户端 | 浏览器登录后可用全部功能；未带令牌访问受保护端点 → 401；登出后令牌失效 |
| 兼容 | `enable_auth=false` 时行为与今天**完全一致**：既有 87 整合 / 30 存储 / 33 golden / 3 demo / 2 示例全绿，无需任何登录 |

---

## 1. 已拍板的决策

| 决策点 | 结论 |
| --- | --- |
| 实现范围 | **认证 + 授权一次做完**（M1+M2），客户端同期接入（M3） |
| 身份数据位置 | **专用全局 auth 文件**：`<data_dir>/cella_auth.db`（页式存储） |
| 首个管理员 | 首次开启认证自动建 `root`，**空口令 + 醒目警告**，随后用 `SET PASSWORD` 修改 |
| 客户端 | **本期一起接**：`/api/login` + 令牌校验 + 前端登录层 |
| 强制开关 | `EngineConfig::enable_auth` / CLI `--auth` / 服务端 `--auth`，**默认关** |

---

## 2. 总体设计

### 2.1 身份与权限的存放（`cella_auth.db`）

复用页式存储，做法与 `cella_catalog` 完全一致（`storage::Schema` + `create_table` +
`TableHeap`）。两张系统表：

```
cella_users       (user VARCHAR(64), pwd TEXT, is_admin INT, created_at INT)
cella_privileges  (user VARCHAR(64), scope_db VARCHAR(64), scope_table VARCHAR(64),
                   priv VARCHAR(16), grantable INT)
```

* `scope_db = '*'` 表示全局；`scope_table = '*'` 表示整库；否则为单表。
* 键为「大写 user + db + table + priv」，重复 GRANT 即幂等更新（`grantable` 取或）。
* **独立于当前库** → 用户是「服务器级」：`USE` 切库不换身份，授权天然可跨库。
* 该文件**不参与** `SHOW DATABASES` 列表，且 `cella_auth` 为**保留库名**（不可
  `CREATE DATABASE cella_auth`）。

### 2.2 权限模型

| 维度 | 取值 |
| --- | --- |
| priv | `GET`（读；`SELECT` 作为等价写法）/ `INSERT` / `UPDATE` / `DELETE` / `CREATE` / `DROP` / `ALL` / `ADMIN` |
| scope | `*.*`（全局） / `db.*`（整库） / `db.table`（单表） / `table`（当前库的单表简写） |
| 判定 | 取**最具体**的一条：表级 > 库级 > 全局；`ADMIN` 全放行 |
| 例外 | 系统表 `cella_catalog` 读放行（写已被 DB-512 拦）；admin 全部放行 |

### 2.3 语句集（全部由会话层拦截，编译器不识别）

```sql
-- 认证 / 用户管理（管理员）
CREATE USER alice IDENTIFIED BY 'pw';
DROP USER alice;
SHOW USERS;

-- 口令（自己，或管理员代改他人）
SET PASSWORD = 'newpw';
SET PASSWORD FOR alice = 'newpw';

-- 授权（管理员）
GRANT  get, insert ON main.student TO alice;
GRANT  all          ON main.*       TO alice, bob;
GRANT  get          ON *.*          TO reporter;      -- 全局读
GRANT  admin                        TO dba;           -- 提升管理员
REVOKE insert       ON main.student FROM alice;
SHOW GRANTS;
SHOW GRANTS FOR alice;
```

### 2.4 强制点（两个）

```
SQL 文本
   │
   ▼
会话层 Session ──① 管理语句与 DDL：USE(CONNECT) / CREATE|DROP TABLE / DATABASE / 用户管理
   │
   ▼
执行器 Executor ─② LockTable：表级读/写（S→GET，X→INSERT|UPDATE|DELETE）
   │
   ▼
存储层 IStorage
```

* **①会话层**：管理语句与 DDL 的权限在此判定（此时已编译，语句种类与表名已知）。
* **②执行器 `LockTable`**：给锁再补一个「本次锁所需的数据权限」参数，表级读写在此判定。
  选这里是因为**它是所有表访问的必经之路**，将来新增算子也不会漏。

### 2.5 口令存储

零第三方依赖 → 自实现 **SHA-256 + 每用户随机盐 + 迭代**（默认 10000 轮），
存为 `sha256$<iter>$<salt_hex>$<hash_hex>`；校验用**常数时间比较**。明文字节不落盘。

---

## 3. 阶段一（M1）：认证

| # | 步骤 |
| --- | --- |
| 1 | 错误码 `DB-8xx`（见 §6） |
| 2 | `auth/password.{h,cpp}`：SHA-256 + 盐 + 迭代，`HashPassword` / `VerifyPassword` |
| 3 | `auth/auth_store.{h,cpp}`：打开 auth 文件、建两张系统表、用户 CRUD、口令校验、内存缓存 |
| 4 | `EngineConfig::enable_auth` / `auth_file`；`DbEngine` 持有 `auth_storage_` + `AuthStore`；`Authenticate()` |
| 5 | `sql_text` + `auth/auth_parser`：识别 `CREATE/DROP USER`、`SET PASSWORD`、`SHOW USERS` |
| 6 | `Session`：`user_` / `is_admin_`；`ExecuteOne` 分支；管理员引导（root 空口令 + 警告）；`SET PASSWORD` 自改/代改规则 |
| 7 | CLI：`--auth`、`--user`/`--password`、缺省交互式提示、提示符显示用户、元命令 `\whoami` `\users` |
| 8 | 整合层 `tests/test_auth.cpp` + 文档 + 回归 + 提交 |

---

## 4. 阶段二（M2）：授权

| # | 步骤 |
| --- | --- |
| 1 | `auth/privilege.{h,cpp}`：权限枚举、作用域解析、最具体优先判定、`GRANT ADMIN` |
| 2 | `GRANT` / `REVOKE` / `SHOW GRANTS` 的解析与执行（DB-804 最后一个管理员、DB-805 非管理员授予） |
| 3 | `ExecContext` 加 `user` / `is_admin`；`LockTable` 加「所需数据权限」参数，7 个调用点各自传值（`kRead` / `kInsert` / `kUpdate` / `kDelete`；CREATE/DROP TABLE 传 `kNone`，已由 ①判定） |
| 4 | 会话层检查：`USE`（CONNECT）、`CREATE/DROP TABLE`（库级 CREATE/DROP）、`CREATE/DROP DATABASE`（管理员）、用户管理（管理员） |
| 5 | `SHOW DATABASES` 过滤（普通用户只列有权限的库）；保留名 `cella_auth` |
| 6 | 测试 + 文档 + 回归 + 提交 |

---

## 5. 阶段三（M3）：客户端接入

| # | 步骤 |
| --- | --- |
| 1 | `ServerConfig` 加 `--auth`；`ApiService` 增加**令牌表**（`token → {user, is_admin, expires}`） |
| 2 | 新端点 `POST /api/login`、`POST /api/logout`；`GET /api/session` 返回当前身份 |
| 3 | 其余受保护端点校验令牌（`Authorization` 头或 Cookie），缺失/过期 → 401，权限不足 → 403（走 `error_map`） |
| 4 | 每请求把令牌对应的身份注入会话；**当前仍是单会话 + `gate_` 串行**，多会话留待后续（见 §9） |
| 5 | 前端：登录层、令牌存取、401 拦截跳登录、顶栏显示当前用户 |
| 6 | 客户端测试（登录成功/失败、无令牌 401、登出） + 文档 + 回归 + 提交 |

---

## 6. 错误码（新增 DB-8xx 段）

| 码 | 含义 |
| --- | --- |
| DB-801 | 认证失败（用户名或口令错误） |
| DB-802 | 权限不足 |
| DB-803 | 用户不存在 / 已存在 / 名字非法 |
| DB-804 | 不能删除最后一个管理员 / 不能撤销自身管理员 |
| DB-805 | 无权授予或撤销（需要管理员） |
| DB-806 | 未提供凭据（认证已启用但未登录） |

---

## 7. 改动文件清单

**新增（cella_db）**

| 文件 | 内容 |
| --- | --- |
| `include/cella/db/auth/auth_store.h` + `src/.../auth_store.cpp` | auth 文件与两张系统表的读写、内存缓存（仿 `catalog_manager`） |
| `include/cella/db/auth/password.h` + `src/.../password.cpp` | SHA-256 + 盐 + 迭代 |
| `include/cella/db/auth/privilege.h` + `src/.../privilege.cpp` | 权限枚举 / 作用域 / 判定 |
| `include/cella/db/auth/auth_parser.h` + `src/.../auth_parser.cpp` | 管理语句的轻量解析（复用编译器词法器取 token） |
| `tests/test_auth.cpp` | 认证 + 授权用例 |

**修改**

| 文件 | 改动 |
| --- | --- |
| `cella_db/include/cella/db/common/db_status.h/.cpp` | 新增 `DB-8xx` |
| `cella_db/include/cella/db/engine/sql_text.h` + `src/.../sql_text.cpp` | `IsUserControl` / `IsGrantControl` 识别（与 `IsDatabaseControl` 同构） |
| `cella_db/include/cella/db/engine/db_engine.h` + `src/.../db_engine.cpp` | `EngineConfig::enable_auth` / `auth_file`；`auth_storage_` + `AuthStore`；`Authenticate()`；`Session::user_/is_admin_`；`ExecuteOne` 加管理分支与权限检查；保留库名与 `SHOW DATABASES` 过滤 |
| `cella_db/include/cella/db/exec/executor.h` + `src/.../executor.cpp` | `ExecContext` 加 `user`/`is_admin`；`LockTable` 加所需权限参数并在 7 个调用点传值 |
| `cella_db/src/main.cpp` | `--auth`、`--user`/`--password`、登录提示、提示符加用户、`\whoami` `\users` `\grants` `\passwd` |
| `cella_client/include/.../server_config.h` + `src/.../server_config.cpp` | `--auth` |
| `cella_client/include/.../api_service.h` + `src/.../api_service.cpp` | 令牌表、`/api/login`、`/api/logout`、端点鉴权 |
| `cella_client/web/**` | 登录层、令牌存取、401 处理、身份显示 |
| `cella_client/tests/test_api.cpp` | 登录 / 401 / 登出用例 |
| `run_all.ps1` | 增「认证冒烟」「授权冒烟」「客户端认证冒烟」 |

**零改动**：`cella_sql`、`cella_storage`。

---

## 8. 测试清单

| 用例 | 断言 |
| --- | --- |
| `认证_正确与错误口令` | 对/错口令分别成功/DB-801；不存在的用户 → DB-801（不泄露用户是否存在） |
| `认证_root引导` | 首次开启认证自动建 root（空口令）；打印警告；`SET PASSWORD` 后空口令失效 |
| `认证_改口令` | 自己可改；管理员可代改；改完旧口令失效 |
| `认证_未登录被拒` | 开启认证而未登录时，任意语句 → DB-806 |
| `授权_表级读写` | GRANT `get` 后 GET 通过、INSERT → DB-802；再 GRANT `insert` 后通过 |
| `授权_作用域优先级` | 库级 ALL + 表级 REVOKE → 表上被拒（最具体优先）；全局只读 + 表级写 → 表上可写 |
| `授权_撤销即时生效` | REVOKE 后同一会话下一次执行即被拒 |
| `授权_跨库` | `GRANT get ON other.* TO alice` 后 `USE other` 仍可读 |
| `授权_持久化` | 重启后用户与权限仍在（读 `cella_auth.db`） |
| `授权_管理员全放行` | admin 无任何 GRANT 也能读写任意表 |
| `授权_系统表放行` | 普通用户可 `get * in cella_catalog`；改动它仍报 DB-512 |
| `管理_非管理员被拒` | 普通用户 `CREATE USER` / `GRANT` → DB-805 |
| `管理_最后一个管理员` | 删除唯一管理员 / `REVOKE admin` 使其归零 → DB-804 |
| `管理_保留库名` | `CREATE DATABASE cella_auth` → DB-514；`SHOW DATABASES` 不含它 |
| `兼容_开关关闭` | `enable_auth=false` 时无用户亦可执行全部语句（既有用例即覆盖） |
| `客户端_登录与令牌` | 登录成功拿到令牌；无令牌 → 401；登出后令牌失效；错口令 → 401 |
| `客户端_权限透传` | 低权限用户经 API 执行越权语句 → 403 + error_map 文案 |

---

## 9. 风险与对策

| 风险 | 对策 |
| --- | --- |
| 引入认证后打破既有回归（demo / 脚本 / 测试无凭据） | **默认关**：`enable_auth=false` 时跳过全部检查；既有测试与 demo 一行不改 |
| `LockTable` 加参数牵动 7 个调用点，漏传导致静默放行 | 参数**必填**（无默认值），编译器强制每个调用点表态；新增 `kNone` 用于 DDL |
| 权限判定放错层级导致越权/误拒 | 两条独立路径（会话层管 DDL/管理语句、执行器管表级 DML），并各配一组用例；`kNone` 路径由会话层用例覆盖 |
| auth 文件损坏 / 缺失 | 缺失 → 视为「无用户」：开启认证时自动 bootstrap `root`；损坏 → 明确报错（DB-701 系），不静默当空库 |
| 口令安全性 | SHA-256 + 盐 + 迭代 + 常数时间比较；文档标注「教学强度，生产应换 bcrypt/argon2」 |
| 客户端单会话被多用户共享 | 本期明确：**每请求注入身份 + `gate_` 串行**；多会话/连接池列为后续扩展 |
| 令牌明文存内存 | 令牌随机（32 字节）+ 有过期；仅监听 127.0.0.1；不做跨机部署 |

---

## 10. 明确不做 / 留待扩展

| 项 | 说明 |
| --- | --- |
| 角色（role） | `CREATE ROLE` / `GRANT role TO user`；当前用「直接授权 + ADMIN」已能覆盖 |
| 列级权限 | `GRANT get(name) ON t TO u`；需扩展权限键与判定 |
| 失败锁定 / 审计日志 | 连续失败 N 次锁定、认证事件写 `journal.log` |
| 多会话 / 连接池 | 每个令牌一个 `Session`，支持真正的并发用户 |
| 存储加密 / 网络加密 | 超出本项目范围 |
| `ALTER USER` / 口令有效期 | 复用 `SET PASSWORD` 路径 |
