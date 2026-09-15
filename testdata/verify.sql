get id, name, city in users ordered id desc among 2;
get id in orders ordered id desc among 2;
get id, title in products ordered id desc among 2;
get tag, val in events ordered val desc among 2;
get id, city in users limit city is null among 2;
get id, name in users limit id = 25000;
get id, name, age in users limit age = 30 among 3;
get id, user_id in orders limit user_id = 100 among 3;
get u.name, o.amount in users u middle join orders o on u.id = o.user_id limit o.id = 12345;
