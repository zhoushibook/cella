// tree.js —— 库/表两层树 + 右键菜单（PLAN §5.2）。
// 两层：库节点（当前库展开列出表；其它库点击即切换）→ 表节点。
// 表节点右键：打开数据 / 查看结构 / 新建查询 / 生成 SELECT / 复制表名 / 删除表。

const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

export function createTree(container, { store, actions }) {
  let menuEl = null;

  function closeMenu() {
    if (menuEl) { menuEl.remove(); menuEl = null; }
  }
  document.addEventListener('click', closeMenu);
  document.addEventListener('contextmenu', (e) => {
    if (!menuEl || !menuEl.contains(e.target)) closeMenu();
  });

  function showMenu(x, y, items) {
    closeMenu();
    menuEl = document.createElement('div');
    menuEl.className = 'ctxmenu';
    for (const it of items) {
      const d = document.createElement('div');
      d.textContent = it.label;
      if (it.danger) d.className = 'danger';
      d.addEventListener('click', () => { closeMenu(); it.fn(); });
      menuEl.appendChild(d);
    }
    document.body.appendChild(menuEl);
    const r = menuEl.getBoundingClientRect();
    menuEl.style.left = Math.min(x, window.innerWidth - r.width - 8) + 'px';
    menuEl.style.top = Math.min(y, window.innerHeight - r.height - 8) + 'px';
  }

  function tableNode(s, t) {
    const sys = t.name.toLowerCase() === 'cella_catalog';
    const el = document.createElement('div');
    el.className = 'treenode tbl' + (sys ? ' sys' : '');
    el.textContent = t.name;
    el.title = `${t.name}（${(t.columns || []).length} 列）`;
    el.addEventListener('click', () => actions.openTable(t.name));
    el.addEventListener('contextmenu', (e) => {
      e.preventDefault();
      e.stopPropagation();  // 否则事件冒泡到 document 的关闭监听器，菜单弹出即被关闭
      showMenu(e.clientX, e.clientY, [
        { label: '打开数据', fn: () => actions.openTable(t.name) },
        { label: '查看结构', fn: () => actions.openStruct(t.name) },
        { label: '新建查询', fn: () => actions.newQueryFor(t.name) },
        { label: '生成 SELECT', fn: () => actions.genSelect(t.name) },
        { label: '复制表名', fn: () => navigator.clipboard.writeText(t.name) },
        { label: '删除表', danger: true, fn: () => actions.dropTable(t.name) },
      ]);
    });
    return el;
  }

  function render(s) {
    const kw = (s.treeFilter || '').toLowerCase();
    container.innerHTML = '';

    const dbs = (s.databases && s.databases.length) ? s.databases : [{ name: s.currentDb, current: true }];

    for (const d of dbs) {
      const cur = !!d.current || d.name === s.currentDb;
      const node = document.createElement('div');
      node.className = 'treenode db';
      if (cur) {
        // 过滤时自动展开
        node.innerHTML = `▾ ${esc(d.name)}<span class="badge">当前</span>` +
          `<span class="cnt">${(s.tables || []).length}</span>`;
        node.addEventListener('contextmenu', (e) => {
          e.preventDefault();
          e.stopPropagation();
          showMenu(e.clientX, e.clientY, [
            { label: '新建查询', fn: () => actions.newQuery() },
            { label: '刷新目录', fn: () => actions.refresh() },
          ]);
        });
        container.appendChild(node);

        const tables = (s.tables || []).filter((t) => !kw || t.name.toLowerCase().includes(kw));
        for (const t of tables) container.appendChild(tableNode(s, t));

        if (!tables.length) {
          const d2 = document.createElement('div');
          d2.className = 'treeempty';
          if (kw) {
            d2.textContent = '没有匹配的表';
          } else {
            d2.innerHTML = '这个库还没有表。<br>' +
              '<span class="hint">一键体验：</span>' +
              '<a href="#" class="treelink" data-act="sample">插入建表示例</a>' +
              '<span class="hint"> · </span>' +
              '<a href="#" class="treelink" data-act="query">新建查询</a>';
            d2.querySelector('[data-act="sample"]').addEventListener('click', (e) => {
              e.preventDefault();
              actions.newQuerySample();
            });
            d2.querySelector('[data-act="query"]').addEventListener('click', (e) => {
              e.preventDefault();
              actions.newQuery();
            });
          }
          container.appendChild(d2);
        }
      } else {
        node.innerHTML = `▸ ${esc(d.name)}`;
        node.title = `点击切换到 ${d.name}`;
        node.addEventListener('click', () => actions.useDb(d.name));
        container.appendChild(node);
      }
    }
  }

  return { render };
}
