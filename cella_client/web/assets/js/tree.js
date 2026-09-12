// tree.js —— 库/表树 + 右键菜单（PLAN §5.2）。

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

  function render(s) {
    const kw = (s.treeFilter || '').toLowerCase();
    container.innerHTML = '';
    if (!s.tables.length) {
      const d = document.createElement('div');
      d.className = 'treeempty';
      d.innerHTML = '还没有表。<br><span class="hint">在查询标签里执行 CREATE TABLE，<br>或点右下角「诊断」查看系统状态。</span>';
      container.appendChild(d);
      return;
    }
    // 当前库节点
    const db = document.createElement('div');
    db.className = 'treenode db';
    db.innerHTML = `▾ ${esc(s.currentDb)}<span class="badge">当前</span>`;
    container.appendChild(db);

    const tables = s.tables.filter((t) => !kw || t.name.toLowerCase().includes(kw));
    for (const t of tables) {
      const sys = t.name.toLowerCase() === 'cella_catalog';
      const el = document.createElement('div');
      el.className = 'treenode tbl' + (sys ? ' sys' : '');
      el.textContent = t.name;
      el.title = `${t.name}（${(t.columns || []).length} 列）`;
      el.addEventListener('click', () => actions.openTable(t.name));
      el.addEventListener('contextmenu', (e) => {
        e.preventDefault();
        showMenu(e.clientX, e.clientY, [
          { label: '打开数据', fn: () => actions.openTable(t.name) },
          { label: '查看结构', fn: () => actions.openStruct(t.name) },
          { label: '新建查询', fn: () => actions.newQueryFor(t.name) },
          { label: '复制表名', fn: () => navigator.clipboard.writeText(t.name) },
          { label: '删除表', danger: true, fn: () => actions.dropTable(t.name) },
        ]);
      });
      container.appendChild(el);
    }
    if (!tables.length) {
      const d = document.createElement('div');
      d.className = 'treeempty';
      d.textContent = '没有匹配的表';
      container.appendChild(d);
    }
  }

  const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  return { render };
}
