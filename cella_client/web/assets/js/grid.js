// grid.js —— 结果网格：虚拟滚动 + 列宽/行高拖拽 + 选区复制 + 行勾选 + 三态排序（PLAN §5.2）。
//
// 虚拟滚动：只渲染视口附近的行，用上下垫片行撑出总高度（行高可拖拽，默认 26px）。
// 列宽：table-layout:fixed + <colgroup>；宽度由内容测量得来，可拖拽、双击自适应（行号列同理）。
// 行高：拖表头「#」格下边缘的分隔条；双击复位。
// 选择：单元格拖拽区块 / 点「#」选整行 / 拖「#」连选多行 / 点「#」表头或 Ctrl+A 全选；
//       Ctrl+C 把选区复制为 TSV（可直接粘进 Excel）。
// 勾选：checkable=true 时多出复选框列，供「批量删除」使用，索引对应**视图行号**。

import { loadNum, saveNum } from './ui.js';

const ROW_H_DEF = 26;
const ROW_H_MIN = 20;
const ROW_H_MAX = 64;
const MIN_W = 52;
const MAX_W = 460;
const ROWNUM_W_DEF = 52;
const CK_W = 30;
const MONO = '12px "Cascadia Mono", Consolas, "Courier New", monospace';

const mctx = document.createElement('canvas').getContext('2d');
function textW(s) {
  mctx.font = MONO;
  return mctx.measureText(String(s)).width;
}

const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
const cellText = (v) => (v === null || v === undefined ? 'NULL' : String(v));
const isNumCol = (c) => ['INT32', 'INT64', 'FLOAT', 'DOUBLE', 'INT'].includes(c.type);

export function createGrid(container, { onSort, checkable = false, onCheckChange } = {}) {
  container.classList.add('gridwrap');
  container.tabIndex = 0;
  const table = document.createElement('table');
  table.className = 'grid' + (checkable ? ' with-ck' : '');
  container.appendChild(table);

  let cols = [];            // [{name, type}]
  let rows = [];            // 原始数据（未排序）
  let view = [];            // 视图数据（可能的内部排序结果）—— rowAt/勾选都用视图坐标
  let widths = [];          // 每列宽度（px）
  let sortKey = null;       // {col, dir}；null = 未排序
  let sel = null;           // {r1,c1,r2,c2} 视图坐标
  let checked = new Set();  // 勾选的行（视图坐标）
  let firstVisible = 0;
  let dragMode = null;      // 'cell' | 'row'
  let resizeState = null;   // {kind:'col'|'rownum', x, w} | {kind:'row', y, h}
  let suppressClick = false; // 拖拽列宽/行高后浏览器会补一个 click，不能当成排序/全选
  let rowH = loadNum('rowH', ROW_H_DEF, ROW_H_MIN, ROW_H_MAX);          // 行高（可拖）
  let rownumW = loadNum('rownumW', ROWNUM_W_DEF, 34, 160);              // 「#」列宽（可拖）

  // colgroup 索引：checkable 时 [ck][rownum][数据列...]
  const CK_COL = 0;
  const ROWNUM_COL = checkable ? 1 : 0;
  const dataColIndex = (c) => ROWNUM_COL + 1 + c;

  // 有 onSort（服务端/受控排序）时不自排；否则本地排序（查询结果用）
  const localSort = !onSort;

  // 列宽备忘：key = 列名|类型。手动拖过的宽度跨 setData 记住（翻页/排序/换查询都不丢）
  const widthMemo = new Map();
  const colKey = (c) => c.name + '|' + c.type;

  function fitWidth(ci) {
    const c = cols[ci];
    let w = textW(c.name) + 30;
    const n = Math.min(view.length, 150);
    for (let i = 0; i < n; i++) {
      const t = cellText(view[i][ci]);
      const tw = textW(t.length > 48 ? t.slice(0, 48) : t) + 22;
      if (tw > w) w = tw;
    }
    return Math.max(MIN_W, Math.min(MAX_W, Math.ceil(w)));
  }

  function applySort() {
    if (!localSort || !sortKey) { view = rows; return; }
    let ci = -1;
    for (let i = 0; i < cols.length; i++) if (cols[i].name === sortKey.col) ci = i;
    if (ci < 0) { view = rows; return; }
    const dir = sortKey.dir === 'desc' ? -1 : 1;
    view = rows.slice().sort((a, b) => {
      const x = a[ci], y = b[ci];
      if (x === null || x === undefined) return y === null || y === undefined ? 0 : 1;  // NULL 排最后
      if (y === null || y === undefined) return -1;
      if (typeof x === 'number' && typeof y === 'number') return (x - y) * dir;
      return String(x).localeCompare(String(y)) * dir;
    });
  }

  const inR = (r) => sel && r >= Math.min(sel.r1, sel.r2) && r <= Math.max(sel.r1, sel.r2);
  const inSel = (r, c) => inR(r) && c >= Math.min(sel.c1, sel.c2) && c <= Math.max(sel.c1, sel.c2);
  const isRowSel = (r) => inR(r) && sel && sel.c1 === 0 && sel.c2 === cols.length - 1;

  function colgroupHtml() {
    let h = '<colgroup>';
    if (checkable) h += `<col style="width:${CK_W}px">`;
    h += `<col style="width:${rownumW}px">`;
    for (let c = 0; c < cols.length; c++) h += `<col style="width:${widths[c]}px">`;
    return h + '</colgroup>';
  }

  function render() {
    if (!cols.length) {
      table.innerHTML = '<tbody><tr><td class="empty" style="font-family:var(--sans)">查询成功，0 行</td></tr></tbody>';
      return;
    }
    const viewport = container.clientHeight || 400;
    const total = view.length;
    const start = Math.max(0, firstVisible - 6);
    const end = Math.min(total, start + Math.ceil(viewport / rowH) + 12);

    let html = colgroupHtml();
    html += '<thead><tr>';
    if (checkable) {
      const all = total > 0 && checked.size === total;
      const part = !all && checked.size > 0;
      html += `<th class="ck" title="全选本页"><input type="checkbox" data-ck-all${all ? ' checked' : ''}${part ? ' data-part="1"' : ''}></th>`;
    }
    html += `<th class="rownum" title="点选整行；点表头选全部；拖右边缘调列宽，拖下边缘调行高">#` +
      `<span class="resize size-rownum" data-ci="-1"></span>` +
      `<span class="rowresize" title="拖动调整行高（双击复位）"></span></th>`;
    for (let c = 0; c < cols.length; c++) {
      const col = cols[c];
      const arrow = sortKey && sortKey.col === col.name ? (sortKey.dir === 'asc' ? ' ▲' : ' ▼') : '';
      html += `<th data-col="${esc(col.name)}" data-ci="${c}" class="${isNumCol(col) ? 'num' : ''}"` +
        ` title="${esc(col.name)}${localSort ? '（点击排序，三次取消）' : '（点击排序，三次恢复默认序）'}">` +
        `${esc(col.name)}${arrow}<span class="resize" data-ci="${c}"></span></th>`;
    }
    html += '</tr></thead><tbody>';

    const span = cols.length + (checkable ? 2 : 1);
    if (start > 0) {
      html += `<tr class="pad"><td colspan="${span}" style="height:${start * rowH}px"></td></tr>`;
    }
    for (let r = start; r < end; r++) {
      const row = view[r];
      const rs = isRowSel(r) ? ' sel' : '';
      html += `<tr data-r="${r}">`;
      if (checkable) html += `<td class="ck${rs}"><input type="checkbox" data-ck="${r}"${checked.has(r) ? ' checked' : ''}></td>`;
      html += `<td class="rownum${rs}">${r + 1}</td>`;
      for (let c = 0; c < cols.length; c++) {
        const v = row[c];
        const cls = (isNumCol(cols[c]) ? 'num ' : '') + (inSel(r, c) ? 'sel ' : '') + (v === null || v === undefined ? 'nullv' : '');
        html += `<td data-c="${c}" class="${cls.trim()}" title="${esc(cellText(v))}">${esc(cellText(v))}</td>`;
      }
      html += '</tr>';
    }
    if (end < total) {
      html += `<tr class="pad"><td colspan="${span}" style="height:${(total - end) * rowH}px"></td></tr>`;
    }
    html += '</tbody>';
    table.innerHTML = html;
    applyColWidths();
  }

  // 拖拽列宽/行高时只改 colgroup 与 CSS 变量，不重排整表
  function applyColWidths() {
    let sum = rownumW + (checkable ? CK_W : 0);
    for (const w of widths) sum += w;
    table.style.tableLayout = 'fixed';
    table.style.width = sum + 'px';
    table.style.setProperty('--row-h', rowH + 'px');
    const els = table.querySelectorAll('colgroup col');
    if (checkable && els[CK_COL]) els[CK_COL].style.width = CK_W + 'px';
    if (els[ROWNUM_COL]) els[ROWNUM_COL].style.width = rownumW + 'px';
    for (let c = 0; c < widths.length; c++) {
      if (els[dataColIndex(c)]) els[dataColIndex(c)].style.width = widths[c] + 'px';
    }
  }

  function syncCkAll() {
    const el = table.querySelector('input[data-ck-all]');
    if (!el) return;
    el.checked = view.length > 0 && checked.size === view.length;
    el.indeterminate = checked.size > 0 && checked.size < view.length;
  }

  function afterCheck() {
    syncCkAll();
    if (onCheckChange) onCheckChange(checked.size);
  }

  // 只改选区高亮的类名，**不重建 DOM**。mousedown 里整表重建会打断浏览器的双击计数
  // （第二次点击落在新建的 td 实例上，click detail 永远到不了 2 → dblclick 不触发 → 单元格编辑失灵）
  function paintSelection() {
    table.querySelectorAll('.sel').forEach((el) => el.classList.remove('sel'));
    if (!sel || !cols.length) return;
    const r1 = Math.min(sel.r1, sel.r2), r2 = Math.max(sel.r1, sel.r2);
    const c1 = Math.min(sel.c1, sel.c2), c2 = Math.max(sel.c1, sel.c2);
    const rowSel = sel.c1 === 0 && sel.c2 === cols.length - 1;
    table.querySelectorAll('tbody tr[data-r]').forEach((tr) => {
      const r = +tr.getAttribute('data-r');
      if (r < r1 || r > r2) return;
      if (rowSel) {
        const rn = tr.querySelector('td.rownum');
        if (rn) rn.classList.add('sel');
        const ck = tr.querySelector('td.ck');
        if (ck) ck.classList.add('sel');
      }
      for (let c = c1; c <= c2; c++) {
        const td = tr.querySelector('td[data-c="' + c + '"]');
        if (td) td.classList.add('sel');
      }
    });
  }

  // ── 列宽 / 行高拖拽（双击自适应、复位）──────────────────
  table.addEventListener('mousedown', (e) => {
    const rowHandle = e.target.closest('.rowresize');
    if (rowHandle) {
      resizeState = { kind: 'row', y: e.clientY, h: rowH };
      suppressClick = true;
      document.body.style.cursor = 'row-resize';
      e.preventDefault();
      return;
    }
    const handle = e.target.closest('.resize');
    if (handle) {
      const ci = +handle.getAttribute('data-ci');
      if (ci === -1) resizeState = { kind: 'rownum', x: e.clientX, w: rownumW };
      else resizeState = { kind: 'col', ci, x: e.clientX, w: widths[ci] };
      suppressClick = true;
      document.body.style.cursor = 'col-resize';
      e.preventDefault();
      return;
    }
    suppressClick = false;   // 普通按下：清掉可能残留的标记（上次拖拽没跟来 click 时）
    if (e.target.closest('input')) return;   // 复选框自己处理
    if (e.target.closest('thead')) return;   // 表头交给 click

    const td = e.target.closest('td');
    const tr = td && td.closest('tr[data-r]');
    if (!td || !tr) return;
    const r = +tr.getAttribute('data-r');
    if (td.classList.contains('rownum') || td.classList.contains('ck')) {
      sel = { r1: r, c1: 0, r2: r, c2: cols.length - 1 };
      dragMode = 'row';
      paintSelection();
      return;
    }
    sel = { r1: r, c1: +td.getAttribute('data-c'), r2: r, c2: +td.getAttribute('data-c') };
    dragMode = 'cell';
    paintSelection();
  });

  table.addEventListener('mousemove', (e) => {
    if (!dragMode || (e.buttons & 1) === 0) return;
    const tr = e.target.closest('tr[data-r]');
    if (!tr) return;
    const r = +tr.getAttribute('data-r');
    const changed = sel.r2 !== r;
    sel.r2 = r;
    if (dragMode === 'row') {
      sel.c2 = cols.length - 1;
    } else {
      const td = e.target.closest('td[data-c]');
      if (!td) return;
      if (sel.c2 === +td.getAttribute('data-c') && !changed) return;
      sel.c2 = +td.getAttribute('data-c');
    }
    paintSelection();
  });

  // 双击边界 → 列自适应内容宽度 / 行高复位
  table.addEventListener('dblclick', (e) => {
    const rowHandle = e.target.closest('.rowresize');
    if (rowHandle) {
      e.preventDefault();
      e.stopPropagation();
      rowH = ROW_H_DEF;
      saveNum('rowH', rowH);
      render();
      return;
    }
    const handle = e.target.closest('.resize');
    if (!handle) return;
    e.preventDefault();
    e.stopPropagation();
    const ci = +handle.getAttribute('data-ci');
    if (ci === -1) {
      rownumW = ROWNUM_W_DEF;
      saveNum('rownumW', rownumW);
    } else {
      widths[ci] = fitWidth(ci);
    }
    applyColWidths();
  });

  document.addEventListener('mousemove', (e) => {
    if (resizeState) {
      if (resizeState.kind === 'row') {
        rowH = Math.max(ROW_H_MIN, Math.min(ROW_H_MAX, Math.round(resizeState.h + (e.clientY - resizeState.y))));
        render();                       // 行高变了：垫片高度与可视行数都要重算
      } else if (resizeState.kind === 'rownum') {
        rownumW = Math.max(34, Math.min(160, Math.round(resizeState.w + (e.clientX - resizeState.x))));
        applyColWidths();
      } else {
        widths[resizeState.ci] = Math.round(Math.max(MIN_W, Math.min(MAX_W, resizeState.w + (e.clientX - resizeState.x))));
        applyColWidths();
      }
      return;
    }
    if (dragMode && (e.buttons & 1) === 0) dragMode = null;
  });
  document.addEventListener('mouseup', () => {
    if (resizeState) {
      if (resizeState.kind === 'row') saveNum('rowH', rowH);
      else if (resizeState.kind === 'rownum') saveNum('rownumW', rownumW);
    }
    resizeState = null;
    dragMode = null;
    document.body.style.cursor = '';
  });

  // ── 排序（表头点击，三态） / 全选（# 表头） ──────────────
  table.addEventListener('click', (e) => {
    // 拖完列宽/行高后浏览器补发的 click：既不能排序，也不能触发「点 # 表头全选」
    if (suppressClick) {
      suppressClick = false;
      return;
    }
    if (e.target.closest('.resize') || e.target.closest('.rowresize')) return;
    const thAll = e.target.closest('thead th.rownum');
    if (thAll) {
      sel = { r1: 0, c1: 0, r2: Math.max(0, view.length - 1), c2: Math.max(0, cols.length - 1) };
      render();
      container.focus();
      return;
    }
    const th = e.target.closest('th[data-col]');
    if (!th) return;
    const col = th.getAttribute('data-col');
    let next;
    if (!sortKey || sortKey.col !== col) next = { col, dir: 'asc' };
    else if (sortKey.dir === 'asc') next = { col, dir: 'desc' };
    else next = null;
    sortKey = next;
    if (onSort) {
      onSort(next);
    } else {
      applySort();
      render();
    }
  });

  // ── 勾选 ────────────────────────────────────────────────
  table.addEventListener('change', (e) => {
    const one = e.target.closest('input[data-ck]');
    if (one) {
      const r = +one.getAttribute('data-ck');
      if (one.checked) checked.add(r); else checked.delete(r);
      afterCheck();
      return;
    }
    const all = e.target.closest('input[data-ck-all]');
    if (all) {
      checked.clear();
      if (all.checked) for (let i = 0; i < view.length; i++) checked.add(i);
      render();
      afterCheck();
    }
  });

  // ── 键盘：Ctrl+A 全选 / Esc 取消选择 ─────────────────────
  container.addEventListener('keydown', (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'a') {
      e.preventDefault();
      sel = { r1: 0, c1: 0, r2: Math.max(0, view.length - 1), c2: Math.max(0, cols.length - 1) };
      render();
    } else if (e.key === 'Escape') {
      sel = null;
      render();
    }
  });

  container.addEventListener('scroll', () => {
    const fv = Math.floor(container.scrollTop / rowH);
    if (fv !== firstVisible) {
      firstVisible = fv;
      render();
    }
  });

  container.addEventListener('mousedown', () => {
    if (document.activeElement !== container) container.focus({ preventScroll: true });
  });

  // Ctrl+C → 选区 TSV（编辑器/输入框聚焦时让位给原生复制）
  document.addEventListener('copy', (e) => {
    if (!sel || !container.isConnected) return;
    const active = document.activeElement;
    if (active && (active.tagName === 'TEXTAREA' || active.tagName === 'INPUT')) return;
    const r1 = Math.min(sel.r1, sel.r2), r2 = Math.max(sel.r1, sel.r2);
    const c1 = Math.min(sel.c1, sel.c2), c2 = Math.max(sel.c1, sel.c2);
    const lines = [];
    for (let r = r1; r <= r2 && r < view.length; r++) {
      const cells = [];
      for (let c = c1; c <= c2 && c < cols.length; c++) cells.push(cellText(view[r][c]));
      lines.push(cells.join('\t'));
    }
    e.clipboardData.setData('text/plain', lines.join('\n'));
    e.preventDefault();
  });

  return {
    setData(columns, dataRows) {
      // 保留「同名同类型」列的宽度：翻页 / 排序 / 保存 / 重跑查询 / 换列集再换回来，都不冲掉用户调过的列宽
      const prevCols = cols;
      const prevWidths = widths;
      prevCols.forEach((c, i) => widthMemo.set(colKey(c), prevWidths[i]));
      cols = columns || [];
      rows = dataRows || [];
      checked = new Set();
      sel = null;
      firstVisible = 0;
      container.scrollTop = 0;
      view = rows;
      applySort();
      widths = cols.map((c, i) => {
        const remembered = widthMemo.get(colKey(c));
        return remembered != null ? remembered : fitWidth(i);
      });
      render();
      afterCheck();
    },
    // 受控重排（数据变化但列不变）时保持列宽
    setRows(dataRows) {
      rows = dataRows || [];
      checked = new Set();
      view = rows;
      applySort();
      firstVisible = 0;
      container.scrollTop = 0;
      render();
      afterCheck();
    },
    setSort(k) { sortKey = k; },
    // 行高 / 行号列宽复位（供自测与「恢复默认」用）
    resetLayout() {
      rowH = ROW_H_DEF;
      rownumW = ROWNUM_W_DEF;
      saveNum('rowH', rowH);
      saveNum('rownumW', rownumW);
      render();
    },
    rowHeight: () => rowH,
    clear() { cols = []; rows = []; view = []; widths = []; sel = null; checked = new Set(); render(); },
    rowAt(i) { return view[i]; },
    rowCount: () => view.length,
    getChecked: () => [...checked].sort((a, b) => a - b),
    checkedCount: () => checked.size,
    clearChecked() { checked = new Set(); render(); afterCheck(); },
    clearSelection() { sel = null; render(); },
  };
}
