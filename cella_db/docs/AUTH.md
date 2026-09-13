# 访问控制（认证 + 授权）

> 面向使用者与二次开发者的参考。设计取舍见 [INTEGRATION.md](INTEGRATION.md) §5，
> 实现计划见 [PLAN_access_control.md](PLAN_access_control.md)。

## 1. 总览

cella 的访问控制分两层：

| 层 | 回答的问题 | 载体 |
| --- | --- | --- |
| **认证**（你是谁） | 登录、用户管理 | 身份库 `cella_auth.db` 的 `cella_users` 表 |
| **授权**（你能做什么） | `GRANT` / `REVOKE`、权限判定 | 同库的 `cella_privileges` 表 |

**默认关闭**。`EngineConfig::enable_auth = false`（CLI 无 `--auth`）时跳过全部权限检查、
无需登录，行为与引入本特性前完全一致；而用户/权限语句本身始终可用（会按需创建身份库），
便于「先把用户建好，再打开开关」。

## 2. 打开开关与登录

```bash
cella_db --data ./mydata --auth                 # 交互式：提示输入用户名与口令
cella_db --data ./mydata --auth -u root -p pw   # 非交互（注意口令会出现在进程列表里）
```

* 首次开启认证时若身份库中没有任何用户，会**自动创建管理员 `root`（口令为空）**，
  并打印醒目安全提示；请立即用 `SET PASSWORD` 修改。
* 认证启用而未登录时，任何语句都会被拒（`DB-806`）。
* REPL 提示符会带上用户名：`cella(main) root>`。

REPL 元命令：`\whoami`（当前身份）、`\users`（用户清单）、`\passwd <新口令>`（改自己的口令）。

## 3. 用户管理

```sql
CREATE USER alice IDENTIFIED BY 'pwd';   -- 口令可省略（= 空口令）
DROP USER alice;
SHOW USERS;                              -- user | admin | created_at
SET PASSWORD = 'newpwd';                 -- 改自己的口令
SET PASSWORD FOR alice = 'newpwd';       -- 管理员代改他人
```

需要管理员（`DB-805`）：`CREATE USER` / `DROP USER` / `SET PASSWORD FOR 他人`。
`SET PASSWORD`（不带 `FOR`）任何人都可执行。

管理员保护：
* 不能删除最后一个管理员（`DB-804`）；
* 不能删除当前登录的用户自己（`DB-804`）。

## 4. 权限模型

| 维度 | 取值 |
| --- | --- |
| 权限 `priv` | `GET`（读；`SELECT` 是等价写法）/ `INSERT` / `UPDATE` / `DELETE` / `CREATE` / `DROP` / `ALL` / `ADMIN` |
| 作用域 `scope` | `*.*` 全局 / `lib.*` 整库 / `lib.tbl` 单表 / `tbl` 当前库的单表 |

判定规则：**取最具体的一条**（表级 > 库级 > 全局）；管理员（`ADMIN`）全放行；
系统表 `cella_catalog` 读放行（写仍被 `DB-512` 拦）。

`GET` 对应读（`get` 语句，含 join 的每一张表）；`INSERT`/`UPDATE`/`DELETE` 分别对应三种写；
`CREATE`/`DROP` 是**建表/删表**（库级）。

## 5. 授权

```sql
GRANT  get, insert ON main.student TO alice;      -- 单表读 + 写
GRANT  all          ON main.*        TO alice, bob;
GRANT  get          ON *.*           TO reporter; -- 全局只读
GRANT  admin                         TO dba;      -- 提升为管理员
REVOKE insert       ON main.student  FROM alice;
SHOW GRANTS;                                      -- 自己的
SHOW GRANTS FOR alice;
```

* `GRANT ... ON 表名`（不带库）按**执行时的当前库**记名，便于脚本 `USE` 之后直接授权。
* `GRANT ADMIN` 把用户提升为管理员；`REVOKE ADMIN` 降级，但不允许把管理员降为零个（`DB-804`）。
* 授权/撤销需要管理员（`DB-805`），且事务中不允许（`DB-513`）。

## 6. 存储

身份库 = `<data_dir>/cella_auth.db`，独立于当前库（`USE` 切库不影响身份），
走与 `cella_catalog` 相同的页式存储：

```
cella_users       (name VARCHAR(64), pwd VARCHAR(0), is_admin INT, created_at INT)
cella_privileges  (user VARCHAR(64), scope_db VARCHAR(64), scope_table VARCHAR(64),
                   priv VARCHAR(16), grantable INT)
```

* `pwd` 存 `sha256$<迭代>$<盐 hex>$<摘要 hex>`：每用户独立随机盐 + 10000 轮迭代 + 常数时间比较，
  明文口令不落盘。
* `cella_auth` 是**保留库名**（不可 `CREATE DATABASE cella_auth`），也不出现在 `SHOW DATABASES` 里。

## 7. 错误码

| 码 | 含义 |
| --- | --- |
| `DB-801` | 认证失败（用户名或口令错误，不区分二者，避免用户名枚举） |
| `DB-802` | 权限不足 |
| `DB-803` | 用户不存在 / 已存在 / 名字非法 |
| `DB-804` | 不能删除最后一个管理员 / 不能删除当前登录用户 |
| `DB-805` | 无权授予或撤销（需要管理员）/ 非管理员做用户管理 |
| `DB-806` | 认证已启用但尚未登录 |

## 8. 安全说明与边界

* **强度定位**：SHA-256 + 盐 + 迭代属于「有诚意的教学强度」，不是生产级别；
  生产应换 bcrypt / scrypt / argon2。
* `root` 初始为**空口令**，仅用于首次进入；请立刻改口令。
* 本方言**没有**角色（role）、列级权限、口令有效期、失败锁定与连接审计（见计划文档 §10）。
* 客户端（`cella_web`）开启认证后需先 `POST /api/login` 换令牌；
  服务端目前是**单会话 + 全局串行**，多会话/连接池留待后续。
