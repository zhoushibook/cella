// grid.js —— 结果网格：虚拟滚动 + 列排序回调 + 选区复制（PLAN §5.2）。
// 万行不卡的关键：只渲染视口附近的行，用上下垫片行撑出总高度（行高固定 26px）。

const ROW_H = 26;

export function createGrid(container, { onSort } = {}) {
  container.classList.add('gridwrap');
  const table = document.createElement('table');
  table.className = 'grid';
  container.appendChild(table);

  let cols = [];       // [{name, type}]
  let rows = [];       // [[value,...]]
  let sortKey = null;  // {col, dir}
  let sel = null;      // {r1,c1,r2,c2} 数据坐标
  let firstVisible = 0;

  const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  const isNum = (c) => ['INT32', 'INT64', 'FLOAT', 'DOUBLE', 'INT'].includes(c.type);

  function headerHtml() {
    let h = '<thead><tr><th class="rownum" style="min-width:44px">#</th>';
    for (const c of cols) {
      const arrow = sortKey && sortKey.col === c.name ? (sortKey.dir === 'asc' ? ' ▲' : ' ▼') : '';
      h += `<th data-col="${esc(c.name)}" class="${isNum(c) ? 'num' : ''}">${esc(c.name)}${arrow}<span class="resize"></span></th>`;
    }
    return h + '</tr></thead>';
  }

  function cellHtml(v) {
    if (v === null || v === undefined) return '<td class="nullv">NULL</td>';
    if (typeof v === 'number') return '<td class="num">' + esc(String(v)) + '</td>';
    if (typeof v === 'boolean') return '<td>' + (v ? 'true' : 'false') + '</td>';
    return '<td>' + esc(String(v)) + '</td>';
  }

  const inSel = (r, c) => sel && r >= Math.min(sel.r1, sel.r2) && r <= Math.max(sel.r1, sel.r2) &&
    c >= Math.min(sel.c1, sel.c2) && c <= Math.max(sel.c1, sel.c2);

  function render() {
    if (!cols.length) {
      table.innerHTML = '<tbody><tr><td class="empty" style="font-family:var(--sans)">查询成功，0 行</td></tr></tbody>';
      return;
    }
    const viewport = container.clientHeight || 400;
    const total = rows.length;
    const start = Math.max(0, firstVisible - 6);
    const end = Math.min(total, start + Math.ceil(viewport / ROW_H) + 12);

    let html = headerHtml() + '<tbody>';
    if (start > 0) {
      html += `<tr><td colspan="${cols.length + 1}" style="height:${start * ROW_H}px;padding:0;border:none"></td></tr>`;
    }
    for (let r = start; r < end; r++) {
      const row = rows[r];
      html += `<tr data-r="${r}"><td class="rownum">${r + 1}</td>`;
      for (let c = 0; c < cols.length; c++) {
        const selCls = inSel(r, c) ? ' sel' : '';
        html += cellHtml(row[c]).replace('<td',
          `<td data-c="${c}" class="${(isNum(cols[c]) ? 'num ' : '') + selCls.trim()}"`);
      }
      html += '</tr>';
    }
    if (end < total) {
      html += `<tr><td colspan="${cols.length + 1}" style="height:${(total - end) * ROW_H}px;padding:0;border:none"></td></tr>`;
    }
    html += '</tbody>';
    table.innerHTML = html;
  }

  container.addEventListener('scroll', () => {
    const fv = Math.floor(container.scrollTop / ROW_H);
    if (fv !== firstVisible) {
      firstVisible = fv;
      render();
    }
  });

  table.addEventListener('click', (e) => {
    const th = e.target.closest('th[data-col]');
    if (!th || e.target.classList.contains('resize')) return;
    const col = th.getAttribute('data-col');
    const dir = sortKey && sortKey.col === col && sortKey.dir === 'asc' ? 'desc' : 'asc';
    sortKey = { col, dir };
    if (onSort) onSort(sortKey);
    else render();
  });

  table.addEventListener('mousedown', (e) => {
    const td = e.target.closest('td[data-c]');
    const tr = td && td.closest('tr[data-r]');
    if (!td || !tr) return;
    const r = +tr.getAttribute('data-r');
    const c = +td.getAttribute('data-c');
    sel = { r1: r, c1: c, r2: r, c2: c };
    render();
  });
  table.addEventListener('mousemove', (e) => {
    if (!sel || (e.buttons & 1) === 0) return;
    const td = e.target.closest('td[data-c]');
    const tr = td && td.closest('tr[data-r]');
    if (td && tr) {
      sel.r2 = +tr.getAttribute('data-r');
      sel.c2 = +td.getAttribute('data-c');
      render();
    }
  });

  // Ctrl+C → TSV（文本框聚焦时让位给原生复制）
  document.addEventListener('copy', (e) => {
    if (!sel || !container.isConnected) return;
    const active = document.activeElement;
    if (active && (active.tagName === 'TEXTAREA' || active.tagName === 'INPUT')) return;
    const r1 = Math.min(sel.r1, sel.r2), r2 = Math.max(sel.r1, sel.r2);
    const c1 = Math.min(sel.c1, sel.c2), c2 = Math.max(sel.c1, sel.c2);
    const lines = [];
    for (let r = r1; r <= r2; r++) {
      const cells = [];
      for (let c = c1; c <= c2; c++) {
        const v = rows[r] ? rows[r][c] : '';
        cells.push(v === null || v === undefined ? 'NULL' : String(v));
      }
      lines.push(cells.join('\t'));
    }
    e.clipboardData.setData('text/plain', lines.join('\n'));
    e.preventDefault();
  });

  return {
    setData(columns, dataRows) {
      cols = columns || [];
      rows = dataRows || [];
      sel = null;
      firstVisible = 0;
      container.scrollTop = 0;
      render();
    },
    setSort(k) { sortKey = k; },
    clear() { cols = []; rows = []; render(); },
    rowAt(i) { return rows[i]; },
    rowCount: () => rows.length,
  };
}
