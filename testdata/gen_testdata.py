# -*- coding: utf-8 -*-
"""生成 cella 测试数据 SQL 脚本。

用法:
  python gen_testdata.py [--out testdata.sql] [--scale 1.0]

默认产出 4 张表共 10 万行:
  users    30,000 行  (主键 id, 二级索引 age)
  products  5,000 行  (主键 id)
  orders   50,000 行  (主键 id, 二级索引 user_id / status)
  events   15,000 行  (无主键 —— 纯堆表对照组)

全部确定性生成（固定种子），可重复执行得到相同数据。
"""
import argparse
import random

random.seed(42)

NAMES = ["alice", "bob", "carol", "dave", "eve", "frank", "grace", "heidi",
         "ivan", "judy", "mallory", "niaj", "olivia", "peggy", "rupert", "trent"]
CITIES = ["beijing", "shanghai", "shenzhen", "hangzhou", "chengdu", "wuhan", "xian", "nanjing"]
CATEGORIES = ["book", "phone", "lamp", "chair", "desk"]
STATUSES = ["paid", "shipped", "done", "cancel"]
TAGS = ["click", "view", "buy", "pay"]

BATCH = 500  # 每条 INSERT 的行数


def esc(v):
    return "NULL" if v is None else str(v)


def emit_insert(fh, table, cols, rows):
    for i in range(0, len(rows), BATCH):
        chunk = rows[i:i + BATCH]
        values = ",\n  ".join("(" + ", ".join(esc(v) for v in r) + ")" for r in chunk)
        fh.write(f"INSERT INTO {table} ({', '.join(cols)}) VALUES\n  {values};\n")


def gen_users(n):
    rows = []
    for i in range(1, n + 1):
        name = f"'{random.choice(NAMES)}_{i:05d}'"
        city = None if i % 97 == 0 else f"'{random.choice(CITIES)}'"
        rows.append((i, name, 18 + (i * 7) % 50, city))
    return rows


def gen_products(n):
    rows = []
    for i in range(1, n + 1):
        title = f"'item-{i:05d}-{random.choice(CATEGORIES)}'"
        price = f"{(i % 500) * 1.5 + 9.9:.2f}"
        rows.append((i, title, price, (i * 37) % 2000))
    return rows


def gen_orders(n):
    rows = []
    for i in range(1, n + 1):
        amount = f"{((i * 31) % 50000) / 100:.2f}"
        rows.append((i, 1 + (i * 13) % 30000, 1 + (i * 7) % 5000,
                     amount, f"'{random.choice(STATUSES)}'"))
    return rows


def gen_events(n):
    return [(f"'{random.choice(TAGS)}'", (i * 17) % 997) for i in range(1, n + 1)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="testdata.sql")
    ap.add_argument("--scale", type=float, default=1.0, help="行数缩放系数")
    args = ap.parse_args()
    s = args.scale

    plan = [
        ("users", ["id", "name", "age", "city"], gen_users(int(30000 * s))),
        ("products", ["id", "title", "price", "stock"], gen_products(int(5000 * s))),
        ("orders", ["id", "user_id", "product_id", "amount", "status"], gen_orders(int(50000 * s))),
        ("events", ["tag", "val"], gen_events(int(15000 * s))),
    ]

    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("-- 测试数据（由 gen_testdata.py 生成，确定性可复现）\n")
        fh.write("CREATE TABLE users(id INT PRIMARY KEY, name VARCHAR(16), age INT, city VARCHAR(16));\n")
        fh.write("CREATE TABLE products(id INT PRIMARY KEY, title VARCHAR(32), price DOUBLE, stock INT);\n")
        fh.write("CREATE TABLE orders(id INT PRIMARY KEY, user_id INT, product_id INT, amount DOUBLE, status VARCHAR(8));\n")
        fh.write("CREATE TABLE events(tag VARCHAR(8), val INT);\n")
        total = 0
        for table, cols, rows in plan:
            emit_insert(fh, table, cols, rows)
            total += len(rows)
            print(f"{table}: {len(rows)} 行")
        fh.write("CREATE INDEX idx_users_age ON users(age);\n")
        fh.write("CREATE INDEX idx_orders_user ON orders(user_id);\n")
        fh.write("CREATE INDEX idx_orders_status ON orders(status);\n")
        print(f"合计: {total} 行 -> {args.out}")

        # 自校验查询（执行脚本末尾顺带验证）
        fh.write("get id in users ordered id desc among 1;\n")
        fh.write("get id in orders ordered id desc among 1;\n")


if __name__ == "__main__":
    main()
