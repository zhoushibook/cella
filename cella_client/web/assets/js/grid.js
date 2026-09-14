// grid.js —— 结果网格：虚拟滚动 + 列宽拖拽/自适应 + 选区复制 + 行勾选 + 三态排序（PLAN §5.2）。
//
// 虚拟滚动：只渲染视口附近的行，用上下垫片行撑出总高度（行高固定 26px）。
// 列宽：table-layout:fixed + <colgroup>；宽度由内容测量得来，可拖拽、双击自适应。
// 选择：单元格拖拽区块 / 点「#」选整行 / 拖「#」连选多行 / 点「#」表头或 Ctrl+A 全选；
//       Ctrl+C 把选区复制为 TSV（可直接粘进 Excel）。
// 勾选：checkable=true 时多出复选框列，供「批量删除」使用，索引对应**视图行号**。

const ROW_H = 26;
const MIN_W = 52;
const MAX_W = 460;
const ROWNUM_W = 52;
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
  let resizeState = null;   // {ci, x, w}

  // 有 onSort（服务端/受控排序）时不自排；否则本地排序（查询结果用）
  const localSort = !onSort;

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
    h += `<col style="width:${ROWNUM_W}px">`;
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
    const end = Math.min(total, start + Math.ceil(viewport / ROW_H) + 12);

    let html = colgroupHtml();
    html += '<thead><tr>';
    if (checkable) {
      const all = total > 0 && checked.size === total;
      const part = !all && checked.size > 0;
      html += `<th class="ck" title="全选本页"><input type="checkbox" data-ck-all${all ? ' checked' : ''}${part ? ' data-part="1"' : ''}></th>`;
    }
    html += `<th class="rownum" title="点击选中整行；点表头选中全部">#</th>`;
    for (let c = 0; c < cols.length; c++) {
      const col = cols[c];
      const arrow = sortKey && sortKey.col === col.name ? (sortKey.dir === 'asc' ? ' ▲' : ' ▼') : '';
      html += `<th data-col="${esc(col.name)}" data-ci="${c}" class="${isNumCol(col) ? 'num' : ''}"` +
        ` title="${esc(col.name)}${localSort ? '（点击排序，三次取消）' : '（点击排序，三次恢复默认序）'}">` +
        `${esc(col.name)}${arrow}<span class="resize"></span></th>`;
    }
    html += '</tr></thead><tbody>';

    if (start > 0) {
      html += `<tr class="pad"><td colspan="${cols.length + (checkable ? 2 : 1)}" style="height:${start * ROW_H}px"></td></tr>`;
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
      html += `<tr class="pad"><td colspan="${cols.length + (checkable ? 2 : 1)}" style="height:${(total - end) * ROW_H}px"></td></tr>`;
    }
    html += '</tbody>';
    table.innerHTML = html;
    applyColWidths();
  }

  // 拖拽列宽时只改 colgroup，不重排 DOM
  function applyColWidths() {
    let sum = ROWNUM_W + (checkable ? CK_W : 0);
    for (const w of widths) sum += w;
    table.style.tableLayout = 'fixed';
    table.style.width = sum + 'px';
    const els = table.querySelectorAll('colgroup col');
    const base = checkable ? 2 : 1;
    for (let c = 0; c < widths.length; c++) {
      if (els[base + c]) els[base + c].style.width = widths[c] + 'px';
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

  // ── 列宽拖拽 / 双击自适应 ──────────────────────────────
  table.addEventListener('mousedown', (e) => {
    const handle = e.target.closest('.resize');
    if (handle) {
      const ci = +handle.closest('th[data-col]').getAttribute('data-ci');
      resizeState = { ci, x: e.clientX, w: widths[ci] };
      document.body.style.cursor = 'col-resize';
      e.preventDefault();
      return;
    }
    if (e.target.closest('input')) return;   // 复选框自己处理
    if (e.target.closest('thead')) return;   // 表头交给 click

    const td = e.target.closest('td');
    const tr = td && td.closest('tr[data-r]');
    if (!td || !tr) return;
    const r = +tr.getAttribute('data-r');
    if (td.classList.contains('rownum') || td.classList.contains('ck')) {
      sel = { r1: r, c1: 0, r2: r, c2: cols.length - 1 };
      dragMode = 'row';
      render();
      return;
    }
    sel = { r1: r, c1: +td.getAttribute('data-c'), r2: r, c2: +td.getAttribute('data-c') };
    dragMode = 'cell';
    render();
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
    render();
  });

  // 双击列边界 → 该列自适应内容宽度
  table.addEventListener('dblclick', (e) => {
    const handle = e.target.closest('.resize');
    if (!handle) return;
    const ci = +handle.closest('th[data-col]').getAttribute('data-ci');
    e.preventDefault();
    e.stopPropagation();
    widths[ci] = fitWidth(ci);
    applyColWidths();
  });

  document.addEventListener('mousemove', (e) => {
    if (resizeState) {
      const w = Math.max(MIN_W, Math.min(MAX_W, resizeState.w + (e.clientX - resizeState.x)));
      widths[resizeState.ci] = Math.round(w);
      applyColWidths();
      return;
    }
    if (dragMode && (e.buttons & 1) === 0) dragMode = null;
  });
  document.addEventListener('mouseup', () => {
    resizeState = null;
    dragMode = null;
    document.body.style.cursor = '';
  });

  // ── 排序（表头点击，三态） / 全选（# 表头） ──────────────
  table.addEventListener('click', (e) => {
    if (e.target.closest('.resize')) return;
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
    const fv = Math.floor(container.scrollTop / ROW_H);
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
      cols = columns || [];
      rows = dataRows || [];
      checked = new Set();
      sel = null;
      firstVisible = 0;
      container.scrollTop = 0;
      view = rows;
      applySort();
      widths = cols.map((_, c) => fitWidth(c));
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
    clear() { cols = []; rows = []; view = []; widths = []; sel = null; checked = new Set(); render(); },
    rowAt(i) { return view[i]; },
    rowCount: () => view.length,
    getChecked: () => [...checked].sort((a, b) => a - b),
    checkedCount: () => checked.size,
    clearChecked() { checked = new Set(); render(); afterCheck(); },
    clearSelection() { sel = null; render(); },
  };
}
