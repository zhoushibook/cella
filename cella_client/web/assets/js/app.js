// app.js —— 装配：顶栏 / 标签 / 数据浏览 / 行编辑 / 事务 / 导出。
// 约定（PLAN §5.4.1）：组件间不互相调用，一律通过 store 交互。

import { Api, ApiError, setToken, Auth, Conn } from './api.js';
import { state$, set, subscribe, tableByName, pkOf } from './store.js';
import { createEditor, formatSql } from './editor.js';
import { createGrid } from './grid.js';
import { createTree } from './tree.js';
import { initBottomPanel, renderStruct } from './panels.js';
import { loadNum, saveNum, makeSplitter, setVar } from './ui.js';

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

// 空库引导用的示例脚本（cella 方言：get=SELECT, in=FROM, limit=WHERE, ordered=ORDER BY）
const SAMPLE_SQL = [
  '-- 建表 → 插数据 → 查询（cella 方言）',
  'CREATE TABLE student(id INT PRIMARY KEY, name VARCHAR(16), score DOUBLE);',
  "INSERT INTO student VALUES (1, 'Alice', 88.5), (2, 'Bob', 76.0), (3, 'Cara', 95.25);",
  'get id, name, score',
  'in student',
  'ordered score desc;',
  '',
].join('\n');

// ── SQL 历史（localStorage，最近 200 条，去重后置顶）──────
const HIST_KEY = 'cella.history';
const HIST_MAX = 200;
let history = [];
try {
  const raw = JSON.parse(localStorage.getItem(HIST_KEY) || '[]');
  if (Array.isArray(raw)) history = raw.filter((x) => typeof x === 'string');
} catch (e) { history = []; }
let histIdx = -1;

function pushHistory(sql) {
  const s = String(sql || '').trim();
  if (!s) return;
  const i = history.indexOf(s);
  if (i >= 0) history.splice(i, 1);
  history.unshift(s);
  if (history.length > HIST_MAX) history.length = HIST_MAX;
  histIdx = -1;
  try { localStorage.setItem(HIST_KEY, JSON.stringify(history)); } catch (e) { /* 超限就只留内存 */ }
  renderHistory();
}

function renderHistory() {
  const sel = $('sqlHistory');
  if (!sel) return;
  sel.innerHTML = '<option value="">历史…</option>';
  for (let i = 0; i < history.length; i++) {
    const o = document.createElement('option');
    o.value = String(i);
    const one = history[i].replace(/\s+/g, ' ').trim();
    o.textContent = `${i + 1}. ${one.length > 64 ? one.slice(0, 64) + '…' : one}`;
    sel.appendChild(o);
  }
  sel.disabled = history.length === 0;
  sel.value = '';
}

function activeQueryTab() {
  const t = state$().tabs.find((x) => x.id === state$().activeTab);
  return t && t.type === 'query' ? t : null;
}

// Ctrl+↑ / Ctrl+↓：像 shell 一样回翻历史（dir: -1 更早，+1 更近）
function historyPick(dir) {
  const tab = activeQueryTab();
  if (!tab || !tab.ui || !history.length) return;
  histIdx = histIdx < 0 ? 0 : Math.min(history.length - 1, Math.max(0, histIdx - dir));
  tab.sql = history[histIdx];
  tab.ui.editor.setValue(tab.sql);
  tab.ui.editor.focus();
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
  // 切走前记下光标/滚动位置（面板 DOM 复用，回来时恢复）
  const cur = s.tabs.find((t) => t.id === s.activeTab);
  if (cur && cur.ui && cur.ui.editor) cur.caret = cur.ui.editor.getState();
  s.activeTab = id;
  renderTabs();
  renderTabBody();
}
function disposeTab(tab) {
  if (tab && tab.el) {
    tab.el.remove();
    tab.el = null;
  }
}
function closeTab(id) {
  const s = state$();
  const i = s.tabs.findIndex((t) => t.id === id);
  if (i < 0) return;
  disposeTab(s.tabs[i]);
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
    d.innerHTML = `<span>${esc(t.title)}</span><span class="close" title="关闭 (Ctrl+W)">✕</span>`;
    d.addEventListener('click', (e) => {
      if (e.target.classList.contains('close')) closeTab(t.id);
      else activateTab(t.id);
    });
    bar.appendChild(d);
  }
  const plus = document.createElement('div');
  plus.className = 'tabadd';
  plus.title = '新建查询 (Ctrl+T)';
  plus.textContent = '+';
  plus.addEventListener('click', () => newQueryTab());
  bar.appendChild(plus);
}

let emptyEl = null;
function renderTabBody() {
  const body = $('tabbody');
  const tab = state$().tabs.find((t) => t.id === state$().activeTab);
  // 非活动标签的面板**保留在 DOM 里**（只隐藏）：切回来时光标/滚动/列宽原样
  for (const el of Array.from(body.children)) el.style.display = 'none';
  if (!tab) {
    if (!emptyEl) {
      emptyEl = document.createElement('div');
      emptyEl.className = 'empty';
      emptyEl.innerHTML = '新建一个查询标签开始使用<br>' +
        '<span class="hint">Ctrl+T 新建查询 · 或点左侧表名浏览数据</span>' +
        '<div style="margin-top:12px"><button class="btn primary" id="btnSample">插入建表示例</button></div>';
      emptyEl.querySelector('#btnSample').addEventListener('click', () => newQueryTab(SAMPLE_SQL, '示例'));
    }
    emptyEl.style.display = '';
    body.appendChild(emptyEl);
    return;
  }
  if (!tab.el) {
    tab.el = document.createElement('div');
    tab.el.className = 'tabpane';
    body.appendChild(tab.el);
    if (tab.type === 'query') mountQueryTab(tab.el, tab);
    else if (tab.type === 'data') mountDataTab(tab.el, tab);
    else if (tab.type === 'struct') mountStructTab(tab.el, tab);
  }
  tab.el.style.display = '';
  if (tab.type === 'query' && tab.ui && tab.ui.editor) {
    const ed = tab.ui.editor;
    const caret = tab.caret;
    // 元素从 display:none 恢复显示后，浏览器会把插入点挪到末尾 → 显式还原，并在下一任务再兜一次
    ed.focus();
    ed.restoreState(caret);
    setTimeout(() => { if (tab.el.style.display !== 'none') ed.restoreState(caret); }, 0);
  }
}

// 新建空查询标签
function newQueryTab(sql = '', titlePrefix = '') {
  const n = state$().tabs.filter((t) => t.type === 'query').length + 1;
  return addTab({ type: 'query', title: titlePrefix ? `${titlePrefix} ${n}` : '查询 ' + n, sql });
}

// ── 查询标签 ────────────────────────────────────────────────
function mountQueryTab(pane, tab) {
  const edwrap = document.createElement('div');
  const bar = document.createElement('div');
  bar.className = 'resultbar';
  const gridwrap = document.createElement('div');
  pane.append(edwrap, bar, gridwrap);

  const editor = createEditor(edwrap, {
    onRun: (sql) => runSql(tab, sql),
    onChange: (v) => { tab.sql = v; tab.caret = null; },   // 内容变了，旧光标位置作废
    onHistory: (dir) => historyPick(dir),
  });
  editor.setValue(tab.sql || '');
  const grid = createGrid(gridwrap);   // 查询结果在本地排序（引擎顺序为基准，点列头三态切换）
  tab.ui = { editor, grid, bar };

  bar.innerHTML = '<span>Ctrl+Enter 执行（选中片段只跑选区）· 点列头排序 · 点单元格拖选区 · Ctrl+C 复制</span>';
}

async function runSql(tab, sql) {
  if (!sql || !sql.trim()) return;
  pushHistory(sql);
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
      if (st.plan && (st.plan.before || st.plan.after)) {
        showPanel('plan', { before: st.plan.before, after: st.plan.after, elapsedMs: st.elapsedMs });
      }
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
function mountStructTab(pane, tab) {
  pane.style.overflow = 'auto';
  renderStruct(pane, tab.table);
}

// ── 数据标签（浏览 + 编辑）──────────────────────────────────
const FULL_FETCH = 20000; // §11.7：小表全量拉取阈值

function mountDataTab(pane, tab) {
  const bar = document.createElement('div');
  bar.className = 'databar';
  const gridwrap = document.createElement('div');
  // 底部编辑栏（Navicat 式）：改动先暂存，这里统一 提交 / 回退
  const editbar = document.createElement('div');
  editbar.className = 'editbar';
  pane.append(bar, gridwrap, editbar);

  const grid = createGrid(gridwrap, {
    checkable: true,
    onSort: (k) => {
      // 三态：升 → 降 → 恢复默认序（有主键 = 主键升序，无主键 = rowid）
      if (k) { tab.sort = k.col; tab.order = k.dir; } else { tab.sort = null; tab.order = 'asc'; }
      loadRows(tab);
    },
    onCheckChange: () => updateEditBar(tab),
  });
  tab.pending = { ins: [], upd: new Map(), del: new Map() };

  bar.innerHTML = `
    <button class="btn" data-act="reload">刷新</button>
    <span class="spacer"></span>
    <span class="pager">
      <button class="btn" data-act="first">«</button>
      <button class="btn" data-act="prev">‹</button>
      第 <input data-act="page" value="1"> 页
      <button class="btn" data-act="next">›</button>
      <button class="btn" data-act="last">»</button>
      每页 <select data-act="pagesize">
        <option value="100">100</option>
        <option value="200" selected>200</option>
        <option value="500">500</option>
        <option value="1000">1000</option>
      </select>
      <span data-act="total">共 … 行</span>
    </span>`;
  bar.querySelector('[data-act="reload"]').addEventListener('click', () => loadRows(tab));

  editbar.innerHTML = `
    <button class="btn" data-act="ins" title="新增一行（先暂存，提交时写入库）">+ 新增行</button>
    <button class="btn danger" data-act="delSel" disabled title="把勾选的行标记删除（提交时写入库）">− 删除选中</button>
    <button class="btn primary" data-act="commit" disabled title="把全部暂存修改写入库（同一事务，任一行失败整体回滚）">✓ 提交</button>
    <button class="btn" data-act="revert" disabled title="丢弃全部未提交修改">✕ 回退</button>
    <span data-act="pstat">无未提交修改</span>
    <span class="spacer"></span>
    <span class="hint">双击单元格编辑（先暂存）</span>`;
  editbar.querySelector('[data-act="ins"]').addEventListener('click', () => openInsertModal(tab));
  editbar.querySelector('[data-act="delSel"]').addEventListener('click', () => {
    const rows = tab.ui.grid.getChecked().map((i) => tab.ui.grid.rowAt(i)).filter(Boolean);
    if (!rows.length) return;
    stageDelete(tab, rows);
    tab.ui.grid.clearChecked();
  });
  editbar.querySelector('[data-act="commit"]').addEventListener('click', () => commitEdits(tab));
  editbar.querySelector('[data-act="revert"]').addEventListener('click', () => revertAll(tab));
  tab.uiEditbar = editbar;
  const pageInput = bar.querySelector('[data-act="page"]');
  pageInput.addEventListener('change', () => {
    const p = Math.max(1, parseInt(pageInput.value, 10) || 1);
    goPage(tab, p);
  });
  const sizeSel = bar.querySelector('[data-act="pagesize"]');
  sizeSel.addEventListener('change', () => {
    tab.pageSize = parseInt(sizeSel.value, 10) || 200;
    tab.page = 1;
    if (tab.local) renderLocal(tab);
    else loadRows(tab);
  });
  bar.querySelector('[data-act="first"]').addEventListener('click', () => goPage(tab, 1));
  bar.querySelector('[data-act="prev"]').addEventListener('click', () => goPage(tab, (tab.page || 1) - 1));
  bar.querySelector('[data-act="next"]').addEventListener('click', () => goPage(tab, (tab.page || 1) + 1));
  bar.querySelector('[data-act="last"]').addEventListener('click', () => goPage(tab, tab.totalPages || 1));

  // 行右键：暂存删除 / 撤销暂存（提交时统一写库）
  gridwrap.addEventListener('contextmenu', (e) => {
    const tr = e.target.closest('tr[data-r]');
    if (!tr) return;
    e.preventDefault();
    const r = +tr.getAttribute('data-r');
    const row = grid.rowAt(r);
    if (!row) return;
    showRowMenu(e.clientX, e.clientY, tab, row);
  });

  // 双击单元格 → 内联编辑（先暂存，提交时写库；rowid 列除外）
  gridwrap.addEventListener('dblclick', (e) => {
    const td = e.target.closest('td[data-c]');
    const tr = td && td.closest('tr[data-r]');
    if (!td || !tr) return;
    const colIdx = +td.getAttribute('data-c');
    const col = tab.columns[colIdx];
    if (!col || col.name === 'rowid') return;
    const r = +tr.getAttribute('data-r');
    const shown0 = grid.rowAt(r)[colIdx];
    if (td.querySelector('input')) return;
    const shown = shown0 === null ? 'NULL' : String(shown0);
    td.innerHTML = `<input class="cellinput" value="${esc(shown)}">`;
    const input = td.querySelector('input');
    input.focus();
    input.select();
    const commit = () => {
      const text = input.value;
      if (text === shown) { refreshDataGrid(tab); return; }
      let nv;
      try { nv = cellFromInput(text, col.type); }
      catch (err) { toast(err.message, 'err'); refreshDataGrid(tab); return; }
      stageCellEdit(tab, r, colIdx, nv, td);
    };
    const restore = () => refreshDataGrid(tab);
    input.addEventListener('keydown', (ev) => {
      if (ev.key === 'Enter') { ev.preventDefault(); commit(); }
      if (ev.key === 'Escape') { ev.preventDefault(); restore(); }
    });
    input.addEventListener('blur', () => { if (td.isConnected) commit(); });
  });

  tab.ui = { grid, bar, pageInput };
  loadRows(tab);
}

let rowMenuEl = null;  // 连续右键多行时，先关掉上一个菜单
function showRowMenu(x, y, tab, row) {
  if (rowMenuEl) { rowMenuEl.remove(); rowMenuEl = null; }
  const menu = document.createElement('div');
  menu.className = 'ctxmenu';
  rowMenuEl = menu;
  const p = pendingOf(tab);
  const rid = rowidOf(tab, row);
  const insIdx = (tab.insRows || []).indexOf(row);
  const isDel = rid != null && p.del.has(rid);
  const items = [];
  items.push({ label: '复制该行 (TSV)', fn: () => {
    // 复制用户可见的数据：跳过内部定位键 rowid
    const vis = row.filter((_, i) => tab.columns[i] && tab.columns[i].name !== 'rowid');
    navigator.clipboard.writeText(vis.map((v) => (v === null ? 'NULL' : String(v))).join('\t'));
  } });
  if (insIdx >= 0) {
    items.push({ label: '撤销新增该行', danger: true, fn: () => { p.ins.splice(insIdx, 1); refreshDataGrid(tab); } });
  } else if (isDel) {
    items.push({ label: '撤销删除该行', fn: () => unstageRow(tab, row) });
  } else {
    items.push({ label: '删除该行（暂存）', danger: true, fn: () => { stageDelete(tab, [row]); } });
  }
  if (p.upd.has(rid)) {
    items.push({ label: '撤销该行修改', fn: () => { p.upd.delete(rid); refreshDataGrid(tab); } });
  }
  for (const it of items) {
    const d = document.createElement('div');
    d.textContent = it.label;
    if (it.danger) d.className = 'danger';
    d.addEventListener('click', () => it.fn());
    menu.appendChild(d);
  }
  document.body.appendChild(menu);
  // 定位到鼠标处（position:fixed 不给 left/top 会停在文档流原位置=左上角），并收敛到视口内
  const mr = menu.getBoundingClientRect();
  menu.style.left = Math.max(4, Math.min(x, window.innerWidth - mr.width - 8)) + 'px';
  menu.style.top = Math.max(4, Math.min(y, window.innerHeight - mr.height - 8)) + 'px';
  const close = () => { menu.remove(); rowMenuEl = null; document.removeEventListener('click', close); };
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

// ── Navicat 式暂存编辑：改动先暂存，左下角统一 提交 / 回退 ──────────
function pendingOf(tab) {
  if (!tab.pending) tab.pending = { ins: [], upd: new Map(), del: new Map() };
  return tab.pending;
}
function rowidOf(tab, row) {
  const i = tab.columns.findIndex((c) => c.name === 'rowid');
  return i >= 0 ? row[i] : null;
}
function pendingCount(tab) {
  const p = pendingOf(tab);
  return p.ins.length + p.upd.size + p.del.size;
}

// 把暂存修改叠加到要显示的行上；末尾接上「新增行」的虚拟行（rowid 列留空）
function buildDisplayRows(tab) {
  const p = pendingOf(tab);
  const ridIdx = tab.columns.findIndex((c) => c.name === 'rowid');
  const out = [];
  for (const row of (tab.pageRows || [])) {
    const rid = ridIdx >= 0 ? row[ridIdx] : null;
    const e = p.upd.get(rid);
    if (!e || p.del.has(rid)) { out.push(row); continue; }
    const copy = row.slice();
    for (const [k, v] of Object.entries(e.values)) {
      const i = tab.columns.findIndex((c) => c.name === k);
      if (i >= 0) copy[i] = v;
    }
    out.push(copy);
  }
  tab.insRows = p.ins.map((entry) => tab.columns.map((c) => {
    if (c.name === 'rowid') return '';
    return Object.prototype.hasOwnProperty.call(entry.values, c.name) ? entry.values[c.name] : null;
  }));
  return out.concat(tab.insRows);
}

function makeDirtyFn(tab) {
  const p = pendingOf(tab);
  const insRows = tab.insRows || [];
  const ridIdx = tab.columns.findIndex((c) => c.name === 'rowid');
  return (r, c) => {
    const row = tab.ui.grid.rowAt(r);
    if (!row) return null;
    if (insRows.includes(row)) return 'inserted';
    const rid = ridIdx >= 0 ? row[ridIdx] : null;
    if (rid != null && p.del.has(rid)) return 'deleted';
    const e = p.upd.get(rid);
    if (!e) return null;
    if (c === -1) return 'dirty';
    const name = tab.columns[c] && tab.columns[c].name;
    return name && Object.prototype.hasOwnProperty.call(e.values, name) ? 'dirty' : null;
  };
}

// 用当前 tab.pageRows + 暂存集重画网格（不请求服务端）
function refreshDataGrid(tab) {
  const display = buildDisplayRows(tab);
  // rowid 是引擎定位键（暂存编辑/乐观校验靠它），数据照拉但**不展示**——列从网格消失，
  // 其余列的 data-c 仍是数据索引，编辑/选区/复制不受影响
  tab.ui.grid.setData(tab.columns, display, makeDirtyFn(tab), { hidden: ['rowid'] });
  updateEditBar(tab);
}

function updateEditBar(tab) {
  const bar = tab.uiEditbar;
  if (!bar || !tab.ui.grid) return;
  const p = pendingOf(tab);
  const n = pendingCount(tab);
  const commitBtn = bar.querySelector('[data-act="commit"]');
  const revertBtn = bar.querySelector('[data-act="revert"]');
  const delBtn = bar.querySelector('[data-act="delSel"]');
  const stat = bar.querySelector('[data-act="pstat"]');
  commitBtn.disabled = revertBtn.disabled = n === 0;
  commitBtn.textContent = n ? `✓ 提交 (${n})` : '✓ 提交';
  delBtn.disabled = tab.ui.grid.getChecked().length === 0;
  stat.textContent = n
    ? `未提交：${p.ins.length} 增 / ${p.upd.size} 改 / ${p.del.size} 删`
    : '无未提交修改';
  stat.classList.toggle('has', n > 0);
}

// 暂存一次单元格编辑（就地更新该格显示，不整表重建、不请求服务端）
function stageCellEdit(tab, r, colIdx, nv, td) {
  const row = tab.ui.grid.rowAt(r);
  if (!row) return;
  const col = tab.columns[colIdx];
  const p = pendingOf(tab);
  const rid = rowidOf(tab, row);
  const insIdx = (tab.insRows || []).indexOf(row);
  if (insIdx >= 0) {                       // 编辑「新增行」的虚拟格
    p.ins[insIdx].values[col.name] = nv;
    refreshDataGrid(tab);
    return;
  }
  const e = p.upd.get(rid);
  const original = e ? e.original : row.slice();
  if (p.del.has(rid)) { toast('该行已标记删除；先在右键菜单撤销删除再改', 'err'); refreshDataGrid(tab); return; }
  const values = Object.assign({}, e ? e.values : {}, { [col.name]: nv });
  // 改回原值的列剔除；全部改回 = 撤销该行暂存
  const changed = {};
  let any = false;
  for (const [k, v] of Object.entries(values)) {
    const i = tab.columns.findIndex((c) => c.name === k);
    if (i >= 0 && v !== original[i]) { changed[k] = v; any = true; }
  }
  if (any) p.upd.set(rid, { original, values: changed });
  else p.upd.delete(rid);
  // 就地更新显示
  if (td && td.isConnected) {
    td.textContent = nv === null ? 'NULL' : String(nv);
    td.classList.toggle('nullv', nv === null);
    td.classList.toggle('dirty', any);
    const rn = td.closest('tr') && td.closest('tr').querySelector('td.rownum');
    if (rn) rn.classList.toggle('dirty', any);
  }
  updateEditBar(tab);
}

// 暂存删除（对「新增行」= 撤销新增）；对库里已有行 = 标记删除（提交时生效）
function stageDelete(tab, rows) {
  const p = pendingOf(tab);
  for (const row of rows) {
    const insIdx = (tab.insRows || []).indexOf(row);
    if (insIdx >= 0) { p.ins.splice(insIdx, 1); continue; }
    const rid = rowidOf(tab, row);
    const e = p.upd.get(rid);
    const original = e ? e.original : row.slice();   // 删除校验要用库里的旧值
    if (e) p.upd.delete(rid);
    p.del.set(rid, { original });
  }
  refreshDataGrid(tab);
}

function unstageRow(tab, row) {
  const p = pendingOf(tab);
  const rid = rowidOf(tab, row);
  p.del.delete(rid);
  p.upd.delete(rid);
  refreshDataGrid(tab);
}

// 提交全部暂存修改：单事务内 增 → 改 → 删，任一行失败整体回滚（乐观校验在服务端）
async function commitEdits(tab) {
  const p = pendingOf(tab);
  const n = pendingCount(tab);
  if (!n) return;
  const stat = `${p.ins.length} 行新增 / ${p.upd.size} 行修改 / ${p.del.size} 行删除`;
  if (!confirm(`确认提交 ${stat}？\n（同一事务内执行；某行校验失败则整体回滚）`)) return;
  const autoTxn = !state$().inTxn;
  set({ busy: true });
  try {
    if (autoTxn) await Api.txn('begin');
    for (const entry of p.ins) await Api.insertRow(tab.table, entry.values);
    for (const [, e] of p.upd) {
      await Api.updateRow(tab.table, {
        key: keyForRow(tab, e.original),
        expect: expectForRow(tab, e.original),
        values: e.values,
      });
    }
    for (const [, e] of p.del) {
      await Api.deleteRow(tab.table, {
        key: keyForRow(tab, e.original),
        expect: expectForRow(tab, e.original),
      });
    }
    if (autoTxn) await Api.txn('commit');
    tab.pending = { ins: [], upd: new Map(), del: new Map() };
    toast(`已提交：${stat}` + (autoTxn ? '' : '（外层事务未提交，记得 COMMIT）'), 'ok');
    await fullRefresh();
    await loadRows(tab);
  } catch (err) {
    if (autoTxn) { try { await Api.txn('rollback'); } catch (e2) { /* 继续提示 */ } }
    toast(err.message, 'err', err.detail);
  } finally {
    set({ busy: false });
    updateEditBar(tab);
  }
}

// 回退全部暂存修改（数据没动过，本地重画即可）
function revertAll(tab) {
  const n = pendingCount(tab);
  if (!n) return;
  if (!confirm(`确认丢弃全部 ${n} 条未提交修改？`)) return;
  tab.pending = { ins: [], upd: new Map(), del: new Map() };
  refreshDataGrid(tab);
  toast('已回退全部未提交修改', 'ok');
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
  ok.textContent = '暂存新行';
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
    pendingOf(tab).ins.push({ values });
    back.remove();
    toast('已暂存新行（左下角「✓ 提交」时写入库）', 'ok');
    refreshDataGrid(tab);
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
      if (!tab.pageSize) tab.pageSize = 200;
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
  tab.pageRows = sorted.slice(start, start + tab.pageSize);
  refreshDataGrid(tab);
  updateDataBar(tab, {
    page: tab.page,
    shown: tab.pageRows.length,
    totalKnown: true,
    total: sorted.length,
  });
  set({
    statusText: tab.pageRows.length
      ? `第 ${start + 1}–${start + tab.pageRows.length} 行 · 共 ${sorted.length} 行`
      : `共 ${sorted.length} 行`,
  });
}

function renderServer(tab, d) {
  tab.pageRows = d.rows;
  tab.columns = d.columns;
  refreshDataGrid(tab);
  updateDataBar(tab, { page: d.page, shown: d.rows.length, totalKnown: d.totalKnown, total: d.total });
  set({ statusText: `第 ${d.page} 页 · ${d.rows.length} 行` + (d.totalKnown ? ` · 共 ${d.total} 行` : ' · 共 ≈? 行') });
}

function updateDataBar(tab, info) {
  const bar = tab.ui.bar;
  const pageInput = bar.querySelector('[data-act="page"]');
  // 大表懒统计时算不出总页数，页码框退化为自由输入
  if (pageInput) {
    pageInput.value = info.page;
    if (info.totalKnown) pageInput.max = tab.totalPages || 1;
  }
  const sizeSel = bar.querySelector('[data-act="pagesize"]');
  if (sizeSel && tab.pageSize) sizeSel.value = String(tab.pageSize);
  const totalEl = bar.querySelector('[data-act="total"]');
  if (totalEl) {
    totalEl.textContent = info.totalKnown
      ? `共 ${info.total} 行`
      : (info.shown ? `本页 ${info.shown} 行 · 共 ≈? 行（大表懒统计）` : '共 ≈? 行（大表懒统计）');
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
  sel.addEventListener('change', () => { actions.useDb(sel.value); });
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
  newQuery() { newQueryTab(); },
  newQuerySample() { newQueryTab(SAMPLE_SQL, '示例'); },
  refresh() { fullRefresh(); },
  async useDb(name) {
    if (!name || name === state$().currentDb) return;
    try {
      await Api.useDb(name);
      toast('已切换到 ' + name, 'ok');
      // 切库后目录全变：关掉所有数据/结构标签，避免展示旧库内容
      const s = state$();
      const doomed = s.tabs.filter((t) => t.type !== 'query');
      doomed.forEach(disposeTab);
      set({ tabs: s.tabs.filter((t) => t.type === 'query'), currentDb: name });
      if (!state$().tabs.find((t) => t.id === state$().activeTab)) {
        const left = state$().tabs;
        state$().activeTab = left.length ? left[0].id : null;
      }
      renderTabs();
      renderTabBody();
      await fullRefresh();
    } catch (e) {
      toast(e.message, 'err', e.detail);
      await refreshDatabases();
    }
  },
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
    // 注意：`get rowid, * in t` 是语法错误（`*` 必须独占整条 select 列表）→ 显式列全
    const t = tableByName(name);
    const cols = ['rowid'].concat(((t && t.columns) || []).map((c) => c.name)).join(', ');
    newQueryTab(`get ${cols} in ${name};\n`);
  },
  genSelect(name) {
    const t = tableByName(name);
    const cols = ((t && t.columns) || []).map((c) => c.name);
    const list = cols.length ? cols.join(', ') : '*';
    newQueryTab(`get ${list}\nin ${name}\nordered ${cols[0] || 'rowid'} asc;\n`);
  },
  async dropTable(name) {
    if (!confirm(`确认删除表 ${name}？（DROP TABLE，不可撤销）`)) return;
    try {
      await Api.query(`drop table ${name};`);
      toast('已删除表 ' + name, 'ok');
      const s = state$();
      const doomed = s.tabs.filter((t) => t.table && t.table.toLowerCase() === name.toLowerCase());
      doomed.forEach(disposeTab);
      set({ tabs: s.tabs.filter((t) => !doomed.includes(t)) });
      if (!state$().tabs.find((t) => t.id === state$().activeTab)) {
        const left = state$().tabs;
        state$().activeTab = left.length ? left[left.length - 1].id : null;
      }
      renderTabs();
      renderTabBody();
      await refreshCatalog();
    } catch (e) {
      toast(e.message, 'err', e.detail);
    }
  },
};

// ── 访问控制：登录 / 登出 / 用户标识 ───────────────────────
function renderUserChip(s) {
  const chip = $('userChip');
  const btn = $('btnLogout');
  if (!s.authEnabled) {
    chip.style.display = 'none';
    btn.style.display = 'none';
    return;
  }
  chip.style.display = '';
  chip.textContent = s.user ? (s.user + (s.isAdmin ? '（管理员）' : '')) : '未登录';
  btn.style.display = s.user ? '' : 'none';
}

function showLogin(message) {
  $('loginErr').textContent = message || '';
  $('loginOverlay').classList.add('show');
  if (!$('loginUser').value) $('loginUser').focus(); else $('loginPass').focus();
}

function hideLogin() {
  $('loginOverlay').classList.remove('show');
  $('loginErr').textContent = '';
  $('loginPass').value = '';
}

async function doLogin() {
  const user = $('loginUser').value.trim();
  const password = $('loginPass').value;
  if (!user) { $('loginErr').textContent = '请输入用户名'; return; }
  $('btnLogin').disabled = true;
  try {
    const d = await Api.login(user, password);
    setToken(d.token);
    set({ user: d.user, isAdmin: !!d.admin });
    hideLogin();
    toast('已登录：' + d.user + (d.admin ? '（管理员）' : ''), 'ok');
    await fullRefresh();
  } catch (e) {
    $('loginErr').textContent = e.message;
  } finally {
    $('btnLogin').disabled = false;
  }
}

async function doLogout() {
  try { await Api.logout(); } catch (e) { /* 令牌可能已过期，忽略 */ }
  setToken('');
  set({ user: '', isAdmin: false });
  set({ tabs: [], activeTab: null });
  renderTabs();
  renderTabBody();
  showLogin('已登出');
}

// 布局分隔条：侧边栏宽度、底部面板高度（都持久化；双击复位）
const SIDEBAR_W = { def: 200, min: 140, max: 560 };
const PANEL_H = { def: 200, min: 90, max: 640 };

function applySidebarW(px) {
  setVar('--sidebar-w', px);
  saveNum('sidebarW', px);
}
function applyPanelH(px) {
  setVar('--panel-h', px);
  saveNum('panelH', px);
}

function initSplitters() {
  const sw0 = loadNum('sidebarW', SIDEBAR_W.def, SIDEBAR_W.min, SIDEBAR_W.max);
  const ph0 = loadNum('panelH', PANEL_H.def, PANEL_H.min, PANEL_H.max);
  applySidebarW(sw0);
  applyPanelH(ph0);

  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

  // 拖拽基准必须在**按下瞬间**取当前实际尺寸：否则第二次拖动会从启动时的旧值算起，
  // 表现为「先跳回默认宽度再跟随鼠标」。（onMove 的 dx 是相对按下点的累计位移，基准不能累加）
  const sidebarBox = $('sidebar');
  const sideEl = $('sidebarSplit');
  let sideBase = sw0;
  let sideW = sw0;
  makeSplitter(sideEl, {
    axis: 'x',
    onStart: () => { sideBase = sidebarBox.getBoundingClientRect().width; },
    onMove: (dx) => {
      sideW = clamp(sideBase + dx, SIDEBAR_W.min, SIDEBAR_W.max);
      setVar('--sidebar-w', sideW);
    },
    onEnd: () => saveNum('sidebarW', sideW),
  });
  sideEl.addEventListener('dblclick', () => { sideW = SIDEBAR_W.def; applySidebarW(sideW); });

  const panelBox = $('bottomPanel');
  const panelEl = $('panelSplit');
  let panelBase = ph0;
  let panelH = ph0;
  makeSplitter(panelEl, {
    axis: 'y',
    onStart: () => { panelBase = panelBox.getBoundingClientRect().height; },
    onMove: (dx, dy) => {
      panelH = clamp(panelBase - dy, PANEL_H.min, PANEL_H.max);   // 向上拖 = 面板变高
      setVar('--panel-h', panelH);
    },
    onEnd: () => saveNum('panelH', panelH),
  });
  panelEl.addEventListener('dblclick', () => { panelH = PANEL_H.def; applyPanelH(panelH); });

  // 面板隐藏时把手柄一起藏起来
  const panel = $('bottomPanel');
  const syncSplit = () => panelEl.classList.toggle('hidden', panel.classList.contains('hidden'));
  syncSplit();
  new MutationObserver(syncSplit).observe(panel, { attributes: true, attributeFilter: ['class'] });
}

// ── 启动 ────────────────────────────────────────────────────
function applyTheme(t) {
  document.documentElement.setAttribute('data-theme', t);
  localStorage.setItem('cella.theme', t);
}

// 断线横幅 + 自动重连探测（服务进程被关掉时给出明确提示，而不是无限转圈）
function setupConnection() {
  const banner = $('connBanner');
  let timer = null;
  let wasDown = false;
  const probe = async () => {
    try {
      await Api.health();
      Conn.set(true);
    } catch (e) { /* 还没恢复，继续等 */ }
  };
  Conn.onLine((online) => {
    banner.style.display = online ? 'none' : 'flex';
    if (!online) {
      wasDown = true;
      if (!timer) timer = setInterval(probe, 3000);
    } else if (timer) {
      clearInterval(timer);
      timer = null;
      if (wasDown) {
        wasDown = false;
        toast('已重新连接到服务', 'ok');
        fullRefresh();
      }
    }
  });
  $('connRetry').addEventListener('click', probe);
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
  initSplitters();
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

  // SQL 历史下拉：选中即回填到当前查询标签
  const histSel = $('sqlHistory');
  renderHistory();
  histSel.addEventListener('change', () => {
    const i = parseInt(histSel.value, 10);
    if (Number.isNaN(i)) return;
    const tab = activeQueryTab();
    const sql = history[i];
    histSel.value = '';
    if (sql === undefined) return;
    if (!tab) {
      const t = newQueryTab(sql);
      toast(`已在新标签打开历史 #${i + 1}`, 'ok');
      void t;
      return;
    }
    tab.sql = sql;
    tab.ui.editor.setValue(sql);
    tab.ui.editor.focus();
  });

  // 全局快捷键
  document.addEventListener('keydown', (e) => {
    const mod = e.ctrlKey || e.metaKey;
    const tag = (document.activeElement || {}).tagName || '';
    const inField = tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT';
    if (mod && !e.shiftKey && !e.altKey && e.key.toLowerCase() === 't') {
      e.preventDefault();
      newQueryTab();
      return;
    }
    if (mod && !e.shiftKey && !e.altKey && e.key.toLowerCase() === 'w') {
      const id = state$().activeTab;
      if (id) { e.preventDefault(); closeTab(id); }
      return;
    }
    if (e.key === 'F5') {                    // F5 刷新目录（别按到浏览器刷新）
      e.preventDefault();
      fullRefresh();
      return;
    }
    if (mod && e.key.toLowerCase() === 'k' && !inField) {   // Ctrl+K 快速筛选表
      e.preventDefault();
      $('treeFilter').focus();
      $('treeFilter').select();
    }
  });

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

  // 全局忙碌态：禁用执行按钮 + 顶部进度条 + 状态栏提示
  subscribe((s) => {
    $('btnRun').disabled = s.busy;
    $('busyBar').classList.toggle('hidden', !s.busy);
    $('stBusy').classList.toggle('hidden', !s.busy);
  });

  setupConnection();

  // 访问控制：登录层与用户标识
  subscribe(renderUserChip);
  renderUserChip(state$());
  $('btnLogin').addEventListener('click', doLogin);
  $('btnLogout').addEventListener('click', doLogout);
  $('loginUser').addEventListener('keydown', (e) => { if (e.key === 'Enter') doLogin(); });
  $('loginPass').addEventListener('keydown', (e) => { if (e.key === 'Enter') doLogin(); });
  Auth.onUnauthorized = () => showLogin('登录已过期，请重新登录');

  // 首标签
  addTab({ type: 'query', title: '查询 1',
    sql: '-- cella 方言：get=SELECT, in=FROM, limit=WHERE, ordered=ORDER BY, page 页码, 每页行数\n' +
         '-- 快捷键：Ctrl+Enter 执行 · Ctrl+/ 注释 · Ctrl+↑/↓ 历史 · Ctrl+T 新标签 · F5 刷新目录\n' +
         '-- 试试：CREATE TABLE student(id INT PRIMARY KEY, name VARCHAR(16), score DOUBLE);\n' });

  try {
    const h = await Api.health();
    set({ health: h, currentDb: h.currentDb, authEnabled: !!h.authEnabled,
          user: h.user || '', isAdmin: !!h.admin });
    $('stLeft').textContent = `数据目录 ${h.dataDir} · 页大小 ${h.pageSize} · 缓冲池 ${h.poolSize} 帧 · ${h.replacer}`;
    if (h.authEnabled && !h.user) {
      showLogin('');          // 服务端启用了访问控制且当前未登录
      return;
    }
    await fullRefresh();
  } catch (e) {
    if (e.status === 401) showLogin(e.message);
    else toast(e.message, 'err', e.detail);
  }
}

// 调试/自测句柄（web/_selftest/*.html 依赖；正常使用无副作用）
window.__cella = {
  state$, actions, newQueryTab, closeTab, fullRefresh,
  layout: {
    SIDEBAR_W, PANEL_H,
    applySidebarW, applyPanelH,
    reset() { applySidebarW(SIDEBAR_W.def); applyPanelH(PANEL_H.def); },
  },
};

boot();
