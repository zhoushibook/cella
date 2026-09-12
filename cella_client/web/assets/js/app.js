// app.js —— 装配：顶栏 / 标签 / 数据浏览 / 行编辑 / 事务 / 导出。
// 约定（PLAN §5.4.1）：组件间不互相调用，一律通过 store 交互。

import { Api, ApiError } from './api.js';
import { state$, set, subscribe, tableByName, pkOf } from './store.js';
import { createEditor, formatSql } from './editor.js';
import { createGrid } from './grid.js';
import { createTree } from './tree.js';
import { initBottomPanel, renderStruct } from './panels.js';

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

// 底部面板（boot 时注入实现；组件通过它打开消息/计划面板）
let showPanel = () => {};

// ── Toast ───────────────────────────────────────────────────
function toast(msg, kind = 'ok', detail = '') {
  const d = document.createElement('div');
  d.className = 'toast ' + kind;
  d.innerHTML = esc(msg) + (detail ? ' <span class="more">详情</span><pre></pre>' : '');
  if (detail) {
    d.querySelector('.more').addEventListener('click', () => d.classList.toggle('open'));
    d.querySelector('pre').textContent = detail;
  }
  $('toasts').appendChild(d);
  setTimeout(() => d.remove(), kind === 'err' ? 6000 : 3000);
}

// ── 类型换算：输入框文本 → JSON 值（服务端再按列类型二次校验）──
function cellFromInput(text, type) {
  if (text === '' || text === 'NULL') return null;
  const t = (type || '').toUpperCase();
  if (t === 'INT') {
    const n = Number(text);
    if (!Number.isInteger(n)) throw new Error('列类型 INT 需要整数');
    return n;
  }
  if (t === 'FLOAT' || t === 'DOUBLE') {
    const n = Number(text);
    if (Number.isNaN(n)) throw new Error('列类型 ' + t + ' 需要数值');
    return n;
  }
  return text; // CHAR/VARCHAR/TEXT/DATE/TIME/DATETIME
}

// ── 标签管理 ────────────────────────────────────────────────
let tabSeq = 0;
function addTab(tab) {
  tab.id = 'tab' + (++tabSeq);
  state$().tabs.push(tab);
  activateTab(tab.id);
  return tab;
}
function activateTab(id) {
  const s = state$();
  s.activeTab = id;
  renderTabs();
  renderTabBody();
}
function closeTab(id) {
  const s = state$();
  const i = s.tabs.findIndex((t) => t.id === id);
  if (i < 0) return;
  s.tabs.splice(i, 1);
  if (s.activeTab === id) {
    const next = s.tabs[Math.max(0, i - 1)];
    s.activeTab = next ? next.id : null;
  }
  renderTabs();
  renderTabBody();
}

function renderTabs() {
  const bar = $('tabbar');
  bar.innerHTML = '';
  for (const t of state$().tabs) {
    const d = document.createElement('div');
    d.className = 'tab' + (state$().activeTab === t.id ? ' active' : '');
    d.innerHTML = `<span>${esc(t.title)}</span><span class="close" title="关闭">✕</span>`;
    d.addEventListener('click', (e) => {
      if (e.target.classList.contains('close')) closeTab(t.id);
      else activateTab(t.id);
    });
    bar.appendChild(d);
  }
}

function renderTabBody() {
  const body = $('tabbody');
  body.innerHTML = '';
  const tab = state$().tabs.find((t) => t.id === state$().activeTab);
  if (!tab) {
    body.innerHTML = '<div class="empty">新建一个查询标签开始使用<br><span class="hint">或点左侧表名浏览数据</span></div>';
    return;
  }
  if (tab.type === 'query') mountQueryTab(body, tab);
  else if (tab.type === 'data') mountDataTab(body, tab);
  else if (tab.type === 'struct') mountStructTab(body, tab);
}

// ── 查询标签 ────────────────────────────────────────────────
function mountQueryTab(body, tab) {
  const pane = document.createElement('div');
  pane.className = 'tabpane';
  const edwrap = document.createElement('div');
  const bar = document.createElement('div');
  bar.className = 'resultbar';
  const gridwrap = document.createElement('div');
  pane.append(edwrap, bar, gridwrap);
  body.appendChild(pane);

  const editor = createEditor(edwrap, {
    onRun: (sql) => runSql(tab, sql),
    onChange: (v) => { tab.sql = v; },
  });
  editor.setValue(tab.sql || '');
  const grid = createGrid(gridwrap, {
    onSort: () => {},   // 查询结果本地无排序（结果即引擎顺序）；排序请在 SQL 里写 ordered
  });
  tab.ui = { editor, grid, bar };

  bar.innerHTML = '<span>按 Ctrl+Enter 执行（选中片段只跑选区）</span>';
}

async function runSql(tab, sql) {
  if (!sql || !sql.trim()) return;
  set({ busy: true });
  try {
    const d = await Api.query(sql);
    set({ inTxn: d.session.inTxn, txnId: d.session.txnId });
    refreshTxnButtons();

    const msgs = [];
    let shown = null;
    for (const st of d.statements) {
      const err = st.error;
      if (st.ok) {
        msgs.push({ ok: true, code: st.kind, message: st.tag || 'OK',
          loc: st.notice ? st.notice : '' });
      } else {
        msgs.push({
          ok: false, code: (err && err.code) || 'ERR', message: (err && err.message) || '失败',
          loc: err && err.absLine ? `行 ${err.absLine}:${err.col || 1}` : '',
          detail: (err && err.detail) || '',
          onJump: err && err.absLine ? () => tab.ui && tab.ui.editor.gotoLine(err.absLine, err.col || 1) : null,
        });
      }
      if (st.columns) shown = st;
      if (st.plan && (st.plan.before || st.plan.after)) showPanel('plan', st.plan);
    }

    if (shown) {
      tab.ui.grid.setData(shown.columns, shown.rows);
      const rowCount = shown.rows.length;
      const ms = d.statements.reduce((a, b) => a + (b.elapsedMs || 0), 0);
      set({ lastResult: { columns: shown.columns, rows: shown.rows },
        statusText: `${rowCount} 行 · ${ms.toFixed(2)} ms` });
      bar2(tab).innerHTML = `<span>结果</span><span>·</span><span>${rowCount} 行` +
        (shown.truncated ? '（已截断）' : '') + `</span><span>·</span><span>${ms.toFixed(2)} ms</span>`;
    } else {
      tab.ui.grid.clear();
      const okCount = d.statements.filter((x) => x.ok).length;
      set({ lastResult: null, statusText: `${okCount}/${d.statements.length} 条成功` });
      bar2(tab).innerHTML = `<span>${d.statements.length} 条语句，${okCount} 条成功</span>`;
    }
    showPanel('messages', msgs);
    refreshCatalog(); // 建表/删表/插删改都可能影响树
  } catch (e) {
    toast(e.message, 'err', e.detail);
  } finally {
    set({ busy: false });
  }
}
function bar2(tab) { return tab.ui.bar; }

// ── 结构标签 ────────────────────────────────────────────────
function mountStructTab(body, tab) {
  const pane = document.createElement('div');
  pane.className = 'tabpane';
  pane.style.overflow = 'auto';
  body.appendChild(pane);
  renderStruct(pane, tab.table);
}

// ── 数据标签（浏览 + 编辑）──────────────────────────────────
const FULL_FETCH = 20000; // §11.7：小表全量拉取阈值

function mountDataTab(body, tab) {
  const pane = document.createElement('div');
  pane.className = 'tabpane';
  const bar = document.createElement('div');
  bar.className = 'databar';
  const gridwrap = document.createElement('div');
  pane.append(bar, gridwrap);
  body.appendChild(pane);

  const grid = createGrid(gridwrap, {
    onSort: (k) => { tab.sort = k.col; tab.order = k.dir; loadRows(tab); },
  });

  bar.innerHTML = `
    <button class="btn" data-act="add">+ 新增行</button>
    <button class="btn" data-act="reload">刷新</button>
    <span class="spacer"></span>
    <span class="pager">
      <button class="btn" data-act="first">«</button>
      <button class="btn" data-act="prev">‹</button>
      第 <input data-act="page" value="1"> 页
      <button class="btn" data-act="next">›</button>
      <button class="btn" data-act="last">»</button>
      <span data-act="total">共 … 行</span>
    </span>`;
  bar.querySelector('[data-act="add"]').addEventListener('click', () => openInsertModal(tab));
  bar.querySelector('[data-act="reload"]').addEventListener('click', () => loadRows(tab));
  const pageInput = bar.querySelector('[data-act="page"]');
  pageInput.addEventListener('change', () => {
    const p = Math.max(1, parseInt(pageInput.value, 10) || 1);
    goPage(tab, p);
  });
  bar.querySelector('[data-act="first"]').addEventListener('click', () => goPage(tab, 1));
  bar.querySelector('[data-act="prev"]').addEventListener('click', () => goPage(tab, (tab.page || 1) - 1));
  bar.querySelector('[data-act="next"]').addEventListener('click', () => goPage(tab, (tab.page || 1) + 1));
  bar.querySelector('[data-act="last"]').addEventListener('click', () => goPage(tab, tab.totalPages || 1));

  // 行右键：删除该行（乐观校验在服务端）
  gridwrap.addEventListener('contextmenu', (e) => {
    const tr = e.target.closest('tr[data-r]');
    if (!tr) return;
    e.preventDefault();
    const r = +tr.getAttribute('data-r');
    const row = grid.rowAt(r);
    if (!row) return;
    showRowMenu(e.clientX, e.clientY, tab, row);
  });

  // 双击单元格 → 内联编辑（rowid 列除外）
  gridwrap.addEventListener('dblclick', (e) => {
    const td = e.target.closest('td[data-c]');
    const tr = td && td.closest('tr[data-r]');
    if (!td || !tr) return;
    const colIdx = +td.getAttribute('data-c');
    const col = tab.columns[colIdx];
    if (!col || col.name === 'rowid') return;
    const r = +tr.getAttribute('data-r');
    const original = grid.rowAt(r)[colIdx];
    if (td.querySelector('input')) return;
    const shown = original === null ? 'NULL' : String(original);
    td.innerHTML = `<input class="cellinput" value="${esc(shown)}">`;
    const input = td.querySelector('input');
    input.focus();
    input.select();
    const commit = async () => {
      const text = input.value;
      if (text === shown) { restore(); return; }
      let nv;
      try { nv = cellFromInput(text, col.type); }
      catch (err) { toast(err.message, 'err'); restore(); return; }
      await saveCell(tab, r, colIdx, original, nv);
    };
    const restore = () => loadRows(tab); // 重绘（简单可靠）
    input.addEventListener('keydown', (ev) => {
      if (ev.key === 'Enter') { ev.preventDefault(); commit(); }
      if (ev.key === 'Escape') { ev.preventDefault(); restore(); }
    });
    input.addEventListener('blur', () => { if (td.isConnected) commit(); });
  });

  tab.ui = { grid, bar, pageInput };
  loadRows(tab);
}

function showRowMenu(x, y, tab, row) {
  const menu = document.createElement('div');
  menu.className = 'ctxmenu';
  const del = document.createElement('div');
  del.textContent = '删除该行';
  del.className = 'danger';
  del.addEventListener('click', () => {
    menu.remove();
    if (!confirm('确认删除这一行？')) return;
    deleteRow(tab, row);
  });
  const copy = document.createElement('div');
  copy.textContent = '复制该行 (TSV)';
  copy.addEventListener('click', () => {
    menu.remove();
    navigator.clipboard.writeText(row.map((v) => (v === null ? 'NULL' : String(v))).join('\t'));
  });
  menu.append(copy, del);
  document.body.appendChild(menu);
  const close = () => { menu.remove(); document.removeEventListener('click', close); };
  setTimeout(() => document.addEventListener('click', close), 0);
}

function keyForRow(tab, row) {
  const pk = pkOf(tableByName(tab.table));
  const cols = (tab.columns || []).map((c) => c.name);
  const idxOf = (n) => cols.indexOf(n);
  if (pk.length >= 1) {
    const values = pk.map((n) => {
      const i = idxOf(n);
      return i >= 0 ? row[i] : null;
    });
    return { kind: 'primary', columns: pk, values };
  }
  const i = idxOf('rowid');
  return { kind: 'rowid', columns: ['rowid'], values: [i >= 0 ? row[i] : null] };
}

function expectForRow(tab, row) {
  const expect = {};
  for (const c of tab.columns) {
    if (c.name === 'rowid') continue;
    expect[c.name] = row[tab.columns.indexOf(c.name)];
  }
  return expect;
}

async function saveCell(tab, r, colIdx, original, newValue) {
  const row = tab.ui.grid.rowAt(r);
  if (!row) return;
  const col = tab.columns[colIdx];
  const payload = {
    key: keyForRow(tab, row),
    expect: expectForRow(tab, row),
    values: { [col.name]: newValue },
  };
  set({ busy: true });
  try {
    await Api.updateRow(tab.table, payload);
    toast('已保存', 'ok');
    await loadRows(tab); // §11.3：UPDATE 后行会物理移动，必须重取当前页
  } catch (e) {
    toast(e.message, 'err', e.detail);
    await loadRows(tab);
  } finally {
    set({ busy: false });
  }
}

async function deleteRow(tab, row) {
  const payload = { key: keyForRow(tab, row), expect: expectForRow(tab, row) };
  set({ busy: true });
  try {
    await Api.deleteRow(tab.table, payload);
    toast('已删除', 'ok');
    await loadRows(tab);
  } catch (e) {
    toast(e.message, 'err', e.detail);
    await loadRows(tab);
  } finally {
    set({ busy: false });
  }
}

function openInsertModal(tab) {
  const t = tableByName(tab.table);
  if (!t) return;
  const back = document.createElement('div');
  back.className = 'modal-back';
  const inputs = [];
  const rows = (t.columns || []).map((c) => {
    const row = document.createElement('div');
    row.className = 'row';
    const lab = document.createElement('span');
    lab.style.cssText = 'width:110px;flex:none';
    lab.innerHTML = esc(c.name) + `<span style="color:var(--text-dim);font-size:11px"> ${esc(c.typeFull || c.type)}</span>`;
    const input = document.createElement('input');
    input.placeholder = c.notNull ? '必填（NOT NULL）' : '留空 = NULL';
    row.append(lab, input);
    inputs.push({ c, input });
    return row;
  });
  const modal = document.createElement('div');
  modal.className = 'modal';
  const h = document.createElement('h3');
  h.textContent = '新增行 → ' + t.name;
  const btns = document.createElement('div');
  btns.className = 'btns';
  const cancel = document.createElement('button');
  cancel.className = 'btn';
  cancel.textContent = '取消';
  const ok = document.createElement('button');
  ok.className = 'btn primary';
  ok.textContent = '插入';
  btns.append(cancel, ok);
  modal.append(h, ...rows, btns);
  back.appendChild(modal);
  document.body.appendChild(back);
  back.addEventListener('click', (e) => { if (e.target === back) back.remove(); });
  cancel.addEventListener('click', () => back.remove());
  ok.addEventListener('click', async () => {
    const values = {};
    try {
      for (const { c, input } of inputs) {
        values[c.name] = cellFromInput(input.value.trim(), c.type);
      }
    } catch (e2) {
      toast(e2.message, 'err');
      return;
    }
    try {
      await Api.insertRow(tab.table, values);
      back.remove();
      toast('已插入', 'ok');
      await loadRows(tab);
    } catch (e2) {
      toast(e2.message, 'err', e2.detail);
    }
  });
}

async function goPage(tab, p) {
  p = Math.max(1, Math.min(p, tab.totalPages || p));
  if (tab.local) {
    tab.page = p;
    renderLocal(tab);
  } else {
    tab.page = p;
    await loadRows(tab);
  }
}

async function loadRows(tab) {
  set({ busy: true });
  try {
    const useFull = !tab.serverMode;
    const q = {
      page: useFull ? 1 : (tab.page || 1),
      pageSize: useFull ? FULL_FETCH : (tab.pageSize || 100),
      sort: tab.sort || '',
      order: tab.order || 'asc',
    };
    const d = await Api.rows(tab.table, q);
    tab.columns = d.columns;
    tab.hasPk = d.hasPrimaryKey;
    if (useFull && d.totalKnown) {
      // 小表：全量在内存，翻页/排序零 IO（§11.7）
      tab.local = true;
      tab.all = d.rows;
      tab.total = d.total;
      tab.pageSize = 200;
      tab.page = 1;
      tab.totalPages = Math.max(1, Math.ceil(d.total / tab.pageSize));
      renderLocal(tab);
    } else {
      tab.local = false;
      tab.serverMode = true;
      tab.rows = d.rows;
      tab.page = d.page;
      tab.pageSize = d.pageSize;
      tab.totalKnown = d.totalKnown;
      tab.total = d.total;
      tab.totalPages = d.totalKnown ? Math.max(1, Math.ceil(d.total / d.pageSize)) : (d.rows.length ? (tab.page || 1) + 1 : tab.page || 1);
      renderServer(tab, d);
    }
  } catch (e) {
    toast(e.message, 'err', e.detail);
  } finally {
    set({ busy: false });
  }
}

function localSortedRows(tab) {
  if (!tab.sortKey && tab.sort) tab.sortKey = { col: tab.sort, dir: tab.order || 'asc' };
  const cols = tab.columns.map((c) => c.name);
  const idx = tab.sort ? cols.indexOf(tab.sort) : -1;
  if (idx < 0) return tab.all;
  const dir = (tab.order || 'asc') === 'desc' ? -1 : 1;
  return tab.all.slice().sort((a, b) => {
    const x = a[idx], y = b[idx];
    if (x === null && y === null) return 0;
    if (x === null) return 1;   // NULL 排最后
    if (y === null) return -1;
    if (typeof x === 'number' && typeof y === 'number') return (x - y) * dir;
    return String(x).localeCompare(String(y)) * dir;
  });
}

function renderLocal(tab) {
  const sorted = localSortedRows(tab);
  tab.totalPages = Math.max(1, Math.ceil(sorted.length / tab.pageSize));
  tab.page = Math.min(Math.max(1, tab.page || 1), tab.totalPages);
  const start = (tab.page - 1) * tab.pageSize;
  const pageRows = sorted.slice(start, start + tab.pageSize);
  tab.ui.grid.setData(tab.columns, pageRows);
  updateDataBar(tab, sorted.length, tab.page, tab.totalPages);
  set({ statusText: `第 ${start + 1}–${start + pageRows.length} 行 · 共 ${sorted.length} 行` });
}

function renderServer(tab, d) {
  tab.ui.grid.setData(d.columns, d.rows);
  updateDataBar(tab, d.rows.length, d.page, tab.totalPages, d.totalKnown, d.total);
  set({ statusText: `第 ${d.page} 页 · ${d.rows.length} 行` + (d.totalKnown ? ` · 共 ${d.total} 行` : ' · 共 ≈? 行') });
}

function updateDataBar(tab, rowCount, page, totalPages, totalKnown, total) {
  const bar = tab.ui.bar;
  const pageInput = bar.querySelector('[data-act="page"]');
  if (pageInput) pageInput.value = page;
  const totalEl = bar.querySelector('[data-act="total"]');
  if (totalEl) {
    totalEl.textContent = totalKnown ? `共 ${total} 行` : `共 ≈? 行（大表懒统计）`;
  }
}

// ── 目录 / 树 ───────────────────────────────────────────────
async function refreshCatalog() {
  try {
    const d = await Api.catalog();
    set({ tables: d.tables, currentDb: d.currentDb });
  } catch (e) {
    toast(e.message, 'err');
  }
}

async function refreshDatabases() {
  try {
    const d = await Api.databases();
    set({ databases: d.databases, currentDb: d.current });
    renderDbSelect();
  } catch (e) {
    toast(e.message, 'err');
  }
}

async function fullRefresh() {
  await refreshCatalog();
  await refreshDatabases();
  const s = await Api.session();
  set({ inTxn: s.inTxn, txnId: s.txnId });
  refreshTxnButtons();
}

// ── 顶栏 ────────────────────────────────────────────────────
function renderDbSelect() {
  const holder = $('dbSelect');
  holder.innerHTML = '';
  const sel = document.createElement('select');
  for (const d of state$().databases) {
    const o = document.createElement('option');
    o.value = d.name;
    o.textContent = d.name + (d.current ? '（当前）' : '');
    if (d.current) o.selected = true;
    sel.appendChild(o);
  }
  sel.addEventListener('change', async () => {
    try {
      await Api.useDb(sel.value);
      toast('已切换到 ' + sel.value, 'ok');
      // 切库后目录全变：关掉所有数据/结构标签，避免展示旧库内容
      set({ tabs: state$().tabs.filter((t) => t.type === 'query') });
      state$().activeTab = state$().tabs.length ? state$().tabs[0].id : null;
      renderTabs();
      renderTabBody();
      await fullRefresh();
    } catch (e) {
      toast(e.message, 'err');
      await refreshDatabases();
    }
  });
  const plus = document.createElement('button');
  plus.className = 'btn icon';
  plus.title = '新建数据库';
  plus.textContent = '+';
  plus.addEventListener('click', async () => {
    const name = prompt('新数据库名（字母/数字/下划线）：');
    if (!name) return;
    try {
      await Api.createDb(name);
      toast('已创建 ' + name, 'ok');
      await fullRefresh();
    } catch (e) {
      toast(e.message, 'err');
    }
  });
  const del = document.createElement('button');
  del.className = 'btn icon danger';
  del.title = '删除数据库（软删除；当前库/启动库不可删）';
  del.textContent = '−';
  del.addEventListener('click', async () => {
    const name = sel.value;
    if (!confirm(`确认删除数据库 ${name}？（软删除：改名留档，当前库不可删）`)) return;
    try {
      await Api.dropDb(name);
      toast('已删除 ' + name, 'ok');
      await fullRefresh();
    } catch (e) {
      toast(e.message, 'err');
    }
  });
  holder.append(sel, plus, del);
}

function refreshTxnButtons() {
  const s = state$();
  const badge = $('txnBadge');
  badge.textContent = s.inTxn ? `事务进行中 (txn ${s.txnId})` : '事务空闲';
  badge.className = 'txnbadge' + (s.inTxn ? ' active' : '');
  $('btnBegin').disabled = s.inTxn;
  $('btnCommit').disabled = !s.inTxn;
  $('btnRollback').disabled = !s.inTxn;
}

// ── 导出 ────────────────────────────────────────────────────
function exportResult(kind) {
  const r = state$().lastResult;
  if (!r) { toast('没有可导出的结果', 'err'); return; }
  let blob;
  if (kind === 'json') {
    const arr = r.rows.map((row) => {
      const o = {};
      r.columns.forEach((c, i) => { o[c.name] = row[i]; });
      return o;
    });
    blob = new Blob([JSON.stringify(arr, null, 2)], { type: 'application/json' });
  } else {
    const lines = [r.columns.map((c) => c.name).join(',')];
    for (const row of r.rows) {
      lines.push(row.map((v) => {
        const s = v === null || v === undefined ? 'NULL' : String(v);
        return /[",\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
      }).join(','));
    }
    blob = new Blob(['\uFEFF' + lines.join('\n')], { type: 'text/csv' });
  }
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'cella-result.' + kind;
  a.click();
  URL.revokeObjectURL(a.href);
}

// ── 动作（树右键等）────────────────────────────────────────
const actions = {
  openTable(name) {
    const s = state$();
    const exist = s.tabs.find((t) => t.type === 'data' && t.table === name);
    if (exist) { activateTab(exist.id); return; }
    addTab({ type: 'data', title: name, table: name, page: 1 });
  },
  openStruct(name) {
    const s = state$();
    const exist = s.tabs.find((t) => t.type === 'struct' && t.table === name);
    if (exist) { activateTab(exist.id); return; }
    addTab({ type: 'struct', title: name + ' ⚙', table: name });
  },
  newQueryFor(name) {
    addTab({ type: 'query', title: '查询 ' + (state$().tabs.filter((t) => t.type === 'query').length + 1),
      sql: `get rowid, * in ${name};\n` });
  },
  async dropTable(name) {
    if (!confirm(`确认删除表 ${name}？（DROP TABLE，不可撤销）`)) return;
    try {
      await Api.query(`drop table ${name};`);
      toast('已删除表 ' + name, 'ok');
      const s = state$();
      set({ tabs: s.tabs.filter((t) => !(t.table && t.table.toLowerCase() === name.toLowerCase())) });
      if (!s.tabs.find((t) => t.id === s.activeTab)) {
        s.activeTab = s.tabs.length ? s.tabs[s.tabs.length - 1].id : null;
      }
      renderTabs();
      renderTabBody();
      await refreshCatalog();
    } catch (e) {
      toast(e.message, 'err', e.detail);
    }
  },
};

// ── 启动 ────────────────────────────────────────────────────
function applyTheme(t) {
  document.documentElement.setAttribute('data-theme', t);
  localStorage.setItem('cella.theme', t);
}

async function boot() {
  applyTheme(state$().theme);
  $('btnTheme').addEventListener('click', () => {
    const next = state$().theme === 'light' ? 'dark' : 'light';
    set({ theme: next });
    applyTheme(next);
  });

  const panel = initBottomPanel();
  showPanel = panel.show;
  const tree = createTree($('tree'), { store: { state$ }, actions });
  subscribe(() => tree.render(state$()));
  $('treeFilter').addEventListener('input', (e) => set({ treeFilter: e.target.value }));
  $('btnRefresh').addEventListener('click', () => fullRefresh());

  $('btnRun').addEventListener('click', () => {
    const tab = state$().tabs.find((t) => t.id === state$().activeTab);
    if (tab && tab.type === 'query' && tab.ui) {
      runSql(tab, tab.ui.editor.getValue());
    } else {
      toast('当前不是查询标签', 'err');
    }
  });
  $('btnFormat').addEventListener('click', () => {
    const tab = state$().tabs.find((t) => t.id === state$().activeTab);
    if (tab && tab.type === 'query' && tab.ui) tab.ui.editor.setValue(formatSql(tab.ui.editor.getValue()));
  });
  $('btnExport').addEventListener('click', () => exportResult('csv'));
  $('btnExport').addEventListener('contextmenu', (e) => { e.preventDefault(); exportResult('json'); });

  $('btnBegin').addEventListener('click', async () => {
    try { const d = await Api.txn('begin'); set({ inTxn: d.inTxn, txnId: d.txnId }); refreshTxnButtons(); toast(d.note || 'BEGIN', 'ok'); }
    catch (e) { toast(e.message, 'err'); }
  });
  $('btnCommit').addEventListener('click', async () => {
    try { const d = await Api.txn('commit'); set({ inTxn: d.inTxn, txnId: d.txnId }); refreshTxnButtons(); toast(d.note || 'COMMIT', 'ok'); }
    catch (e) { toast(e.message, 'err'); }
  });
  $('btnRollback').addEventListener('click', async () => {
    try { const d = await Api.txn('rollback'); set({ inTxn: d.inTxn, txnId: d.txnId }); refreshTxnButtons(); toast(d.note || 'ROLLBACK', 'ok'); }
    catch (e) { toast(e.message, 'err'); }
  });

  $('stDiag').addEventListener('click', () => panel.show('diag'));

  // 全局忙碌态：禁用执行按钮
  subscribe((s) => { $('btnRun').disabled = s.busy; });

  // 首标签
  addTab({ type: 'query', title: '查询 1',
    sql: '-- cella 方言：get=SELECT, in=FROM, limit=WHERE, ordered=ORDER BY, page 页码, 每页行数\n' +
         '-- 试试：CREATE TABLE student(id INT PRIMARY KEY, name VARCHAR(16), score DOUBLE);\n' });

  try {
    const h = await Api.health();
    set({ health: h, currentDb: h.currentDb });
    $('stLeft').textContent = `数据目录 ${h.dataDir} · 页大小 ${h.pageSize} · 缓冲池 ${h.poolSize} 帧 · ${h.replacer}`;
    await fullRefresh();
  } catch (e) {
    toast(e.message, 'err', e.detail);
  }
}

boot();
