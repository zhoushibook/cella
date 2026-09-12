// panels.js —— 结构面板 / 诊断面板 / 执行计划视图 / 底部面板（PLAN §5.2）。

import { Api } from './api.js';
import { state$, set, tableByName, pkOf } from './store.js';

const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

// ── 底部面板开关 ────────────────────────────────────────────
export function initBottomPanel() {
  const panel = document.getElementById('bottomPanel');
  const title = document.getElementById('bpTitle');
  const body = document.getElementById('bpBody');
  document.getElementById('bpClose').addEventListener('click', () => panel.classList.add('hidden'));
  const wire = (id, name) => {
    document.getElementById(id).addEventListener('click', () => showPanel(name));
  };
  wire('stMsgs', 'messages');
  wire('stPlan', 'plan');
  wire('stDiag', 'diag');
  return {
    show: showPanel,
    isOpen: () => !panel.classList.contains('hidden'),
  };

  function showPanel(name, arg) {
    panel.classList.remove('hidden');
    body.innerHTML = '';
    if (name === 'messages') {
      title.textContent = '消息';
      const msgs = arg || [];
      if (!msgs.length) {
        body.innerHTML = '<div class="empty" style="padding:12px">暂无消息</div>';
        return;
      }
      for (const m of msgs) {
        const d = document.createElement('div');
        d.className = 'msgitem' + (m.ok ? ' ok' : '');
        d.innerHTML = `<span class="code">${m.ok ? '✓' : '✗'} ${esc(m.code || '')}</span>` +
          `<span>${esc(m.message)}</span>` +
          (m.loc ? `<span class="loc">${esc(m.loc)}</span>` : '') +
          (m.detail ? `<div class="detail">${esc(m.detail)}</div>` : '');
        if (m.onJump) d.addEventListener('click', m.onJump);
        body.appendChild(d);
      }
    } else if (name === 'plan') {
      title.textContent = '执行计划';
      const p = arg;
      if (!p || (!p.before && !p.after)) {
        body.innerHTML = '<div class="empty" style="padding:12px">没有计划（运行查询后生成，DDL 也有计划）</div>';
        return;
      }
      body.innerHTML = `<div class="plan2col">
        <div><h4>优化前</h4><pre>${esc(p.before || '（无）')}</pre></div>
        <div><h4>优化后</h4><pre>${esc(p.after || '（无）')}</pre></div>
      </div>`;
    } else if (name === 'diag') {
      title.textContent = '诊断';
      const kinds = ['stats', 'locks', 'waitfor', 'txn'];
      const names = { stats: '缓冲池', locks: '锁表', waitfor: '等待图', txn: '事务表' };
      const bar = document.createElement('div');
      bar.style.cssText = 'display:flex;gap:8px;margin-bottom:8px;align-items:center';
      let cur = localStorage.getItem('cella.diag') || 'stats';
      let timer = null;
      const pre = document.createElement('pre');
      const load = async () => {
        try {
          const d = await Api.diagnostics(cur);
          pre.textContent = d.text || '（空）';
        } catch (e) {
          pre.textContent = '加载失败：' + e.message;
        }
      };
      for (const k of kinds) {
        const b = document.createElement('button');
        b.className = 'btn';
        b.textContent = names[k];
        b.addEventListener('click', () => { cur = k; localStorage.setItem('cella.diag', k); load(); });
        bar.appendChild(b);
      }
      const pause = document.createElement('label');
      pause.style.cssText = 'margin-left:8px;color:var(--text-dim)';
      pause.innerHTML = '<input type="checkbox" checked> 自动刷新(2s)';
      bar.appendChild(pause);
      const box = pause.querySelector('input');
      timer = setInterval(() => { if (box.checked && !state$().busy) load(); }, 2000);
      body.append(bar, pre);
      load();
    }
  }
}

// ── 结构面板 ────────────────────────────────────────────────
export function renderStruct(container, tableName) {
  const t = tableByName(tableName) || { name: tableName, columns: [], primaryKey: { columns: [] } };
  const pk = pkOf(t);
  container.innerHTML = `
    <div class="structmeta">
      表 <b>${esc(t.name)}</b> · ${((t.columns) || []).length} 列 ·
      表号 ${t.tableId != null ? t.tableId : '—'}
      ${t.createdAt ? ' · 建表于 ' + new Date(t.createdAt * 1000).toLocaleString() : ''}
    </div>
    <div class="structgrid"><table class="grid">
      <thead><tr><th>#</th><th>列名</th><th>类型</th><th>非空</th><th>主键</th></tr></thead>
      <tbody>
        ${((t.columns) || []).map((c, i) => `
          <tr><td class="num">${c.ordinal || i + 1}</td>
          <td>${esc(c.name)}${pk.includes(c.name) ? '<span class="pkmark">PK</span>' : ''}</td>
          <td>${esc(c.typeFull || c.type)}</td>
          <td>${c.notNull ? '是' : ''}</td>
          <td>${c.primaryKey ? '✓' : ''}</td></tr>`).join('')}
      </tbody>
    </table></div>
    <div style="margin:0 14px 14px">
      <div style="font-size:12px;color:var(--text-dim);margin-bottom:6px">生成的 DDL（可复制到查询编辑器改）</div>
      <pre style="border:1px solid var(--border);border-radius:6px;padding:10px">${esc(ddlOf(t))}</pre>
    </div>`;
}

function ddlOf(t) {
  const cols = (t.columns || []).map((c) => {
    let s = `  ${c.name} ${c.typeFull || c.type}`;
    if (c.notNull) s += ' NOT NULL';
    if (c.primaryKey) s += ' PRIMARY KEY';
    return s;
  });
  return `CREATE TABLE ${t.name}(\n${cols.join(',\n')}\n);`;
}
