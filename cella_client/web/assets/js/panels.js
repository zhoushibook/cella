// panels.js —— 结构面板 / 诊断面板 / 执行计划视图 / 底部面板（PLAN §5.2）。
//
// 诊断与计划都做**结构化**呈现：引擎给的是文本（`StatsText/LockText/WaitForGraphText/TxnText`
// 与计划树的缩进文本），这里解析成表格/进度条/边列表；解析失败或用户想看原文时切「原始文本」。
// 这样 cella_db 一行不用改（客户端模块的零改动约定，PLAN §9）。

import { Api } from './api.js';
import { state$, tableByName, pkOf } from './store.js';

const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

// ── 解析：四类诊断文本 ──────────────────────────────────────
export function parseStats(text) {
  const out = { counters: [], config: {}, checkpoints: 0, evictions: [] };
  const lines = String(text || '').split('\n');
  let inEvict = false;
  for (const line of lines) {
    const m = /^\s*([a-z_]+)\s*:\s*(\S+)\s*$/.exec(line);
    if (m) { out.counters.push({ name: m[1], value: m[2] }); inEvict = false; continue; }
    const cfg = /^替换策略:\s*(\S+)\s*\/\s*缓冲池帧数:\s*(\d+)\s*\/\s*页大小:\s*(\d+)(?:\s*\/\s*存盘点\s*(\d+)\s*次)?/.exec(line);
    if (cfg) {
      out.config = { replacer: cfg[1], poolSize: +cfg[2], pageSize: +cfg[3] };
      out.checkpoints = cfg[4] ? +cfg[4] : 0;
      inEvict = false;
      continue;
    }
    if (/^最近淘汰\((\d+) 条\):/.test(line)) { inEvict = true; continue; }
    if (inEvict && /^\s+\S/.test(line)) out.evictions.push(line.trim());
  }
  const rate = out.counters.find((c) => c.name === 'hit_rate');
  out.hitRate = rate ? parseFloat(rate.value) : null;
  return out;
}

export function parseLocks(text) {
  const rows = [];
  for (const line of String(text || '').split('\n')) {
    if (/^\s*\(空\)/.test(line) || !line.trim() || line.startsWith('锁表')) continue;
    const m = /^\s*(\S.*?)\s*:\s*(.*)$/.exec(line);
    if (!m) continue;
    const rest = m[2];
    const x = /\[X txn=(\d+)\]/.exec(rest);
    const shared = [...rest.matchAll(/\[S txn=(\d+)\]/g)].map((a) => +a[1]);
    const wait = /等待:\s*([\d,]+)/.exec(rest);
    rows.push({
      resource: m[1],
      exclusive: x ? +x[1] : null,
      shared,
      waiters: wait ? wait[1].split(',').map((s) => +s) : [],
    });
  }
  return rows;
}

export function parseWaitFor(text) {
  const edges = [];
  for (const line of String(text || '').split('\n')) {
    if (line.startsWith('等待图') || /^\s*\(无等待\)/.test(line)) continue;
    const m = /^\s*(\d+)\s*->\s*([\d,]+|\(无\))\s*(?:\[(\S+)\s+(.+)\])?/.exec(line);
    if (!m) continue;
    edges.push({
      txn: +m[1],
      blockers: m[2] === '(无)' ? [] : m[2].split(',').map((s) => +s),
      mode: m[3] || '',
      resource: (m[4] || '').trim(),
    });
  }
  // 环检测：找出落在环上的事务（死锁高亮）
  const adj = new Map();
  for (const e of edges) adj.set(e.txn, e.blockers);
  const state = new Map();   // 0 未访问 1 在栈上 2 已定
  const onCycle = new Set();
  const stack = [];
  const dfs = (n) => {
    state.set(n, 1);
    stack.push(n);
    for (const b of (adj.get(n) || [])) {
      if (!adj.has(b)) continue;
      if (state.get(b) === 1) {
        // 回边：栈上从 b 到 n 的这一段构成环
        const i = stack.indexOf(b);
        for (let k = i; k < stack.length; k++) onCycle.add(stack[k]);
      } else if (!state.get(b)) {
        dfs(b);
      }
    }
    stack.pop();
    state.set(n, 2);
  };
  for (const e of edges) if (!state.get(e.txn)) dfs(e.txn);
  return { edges, onCycle: [...onCycle] };
}

export function parseTxns(text) {
  const out = { total: 0, active: 0, txns: [], committed: 0, aborted: 0 };
  const head = /^事务表（共\s*(\d+)\s*个，活动\s*(\d+)\s*）/.exec(text || '');
  if (head) { out.total = +head[1]; out.active = +head[2]; }
  for (const line of String(text || '').split('\n')) {
    const m = /^\s*txn=(\d+)\s+状态=(\S+)\s+语句数=(\d+)\s+undo=(\d+)\s+涉及表=\[([^\]]*)\]/.exec(line);
    if (m) {
      out.txns.push({
        id: +m[1], state: m[2], stmts: +m[3], undo: +m[4],
        tables: m[5] ? m[5].split(',').map((s) => s.trim()).filter(Boolean) : [],
      });
    }
    const tail = /^已提交\s*(\d+)\s*\/\s*已回滚\s*(\d+)/.exec(line);
    if (tail) { out.committed = +tail[1]; out.aborted = +tail[2]; }
  }
  return out;
}

// 计划文本 → 缩进树
export function parsePlanTree(text) {
  return String(text || '').split('\n').filter((l) => l.trim()).map((l) => ({
    depth: Math.floor((l.match(/^ */)[0].length) / 2),
    text: l.trim(),
  }));
}

// ── 渲染：四类诊断 + 计划树 ─────────────────────────────────
function table(headers, rows) {
  return `<table class="grid diag"><thead><tr>${headers.map((h) => `<th>${esc(h)}</th>`).join('')}</tr></thead>` +
    `<tbody>${rows.map((r) => `<tr>${r.map((c) => `<td>${c}</td>`).join('')}</tr>`).join('')}</tbody></table>`;
}

function renderStats(text) {
  const s = parseStats(text);
  const pct = s.hitRate === null ? null : Math.max(0, Math.min(100, s.hitRate));
  const num = (n) => {
    const c = s.counters.find((x) => x.name === n);
    return c ? c.value : '—';
  };
  const cls = pct === null ? '' : (pct >= 90 ? 'ok' : (pct >= 60 ? 'warn' : 'err'));
  let html = '<div class="diagwrap">';
  if (pct !== null) {
    html += `<div class="hitrate"><span class="hrlabel">命中率</span>` +
      `<span class="hbar"><i class="${cls}" style="width:${pct}%"></i></span>` +
      `<span class="hrvalue">${pct.toFixed(2)}%</span></div>`;
  }
  html += `<div class="diagcards">
    <div class="card"><b>${num('accesses')}</b><span>总访问</span></div>
    <div class="card"><b>${num('hits')}</b><span>命中</span></div>
    <div class="card"><b>${num('misses')}</b><span>未命中</span></div>
    <div class="card"><b>${num('evictions')}</b><span>淘汰</span></div>
    <div class="card"><b>${num('dirty_flush')}</b><span>脏页写回</span></div>
    <div class="card"><b>${num('disk_reads')}</b><span>磁盘读</span></div>
    <div class="card"><b>${num('disk_writes')}</b><span>磁盘写</span></div>
    <div class="card"><b>${num('page_allocs')}/${num('page_frees')}</b><span>页分配/释放</span></div>
  </div>`;
  html += `<div class="diagline">替换策略 <b>${esc(s.config.replacer || '—')}</b> · 缓冲池 <b>${s.config.poolSize || '—'}</b> 帧 · ` +
    `页大小 <b>${s.config.pageSize || '—'}</b>` +
    (s.checkpoints ? ` · 存盘点 <b>${s.checkpoints}</b> 次（上方为累计值）` : '') + '</div>';
  if (s.evictions.length) {
    html += `<details class="diagfold"><summary>最近淘汰（${s.evictions.length} 条）</summary>` +
      `<pre>${esc(s.evictions.join('\n'))}</pre></details>`;
  }
  return html + '</div>';
}

function renderLocks(text) {
  const rows = parseLocks(text);
  if (!rows.length) return '<div class="empty" style="padding:14px">锁表为空（没有并发事务持锁）</div>';
  const body = rows.map((r) => {
    const holders = [];
    if (r.exclusive !== null) holders.push(`<span class="lockmode x">X</span> txn=${r.exclusive}`);
    for (const s of r.shared) holders.push(`<span class="lockmode s">S</span> txn=${s}`);
    return [
      `<code>${esc(r.resource)}</code>`,
      holders.join(' ') || '—',
      r.waiters.length ? `<span class="waiter">txn=${r.waiters.join(', ')}</span>` : '—',
    ];
  });
  return `<div class="diagwrap"><div class="diagline">资源 = 持有者 / 等待者</div>` +
    table(['资源', '持有者', '等待者'], body) + '</div>';
}

function renderWaitFor(text) {
  const { edges, onCycle } = parseWaitFor(text);
  if (!edges.length) return '<div class="empty" style="padding:14px">无等待（没有事务在等锁）</div>';
  let html = '<div class="diagwrap">';
  if (onCycle.length) {
    html += `<div class="diagalert">⚠ 检测到等待环（可能死锁）：相关事务 ${onCycle.map((t) => 'txn=' + t).join('、')}</div>`;
  }
  html += '<div class="edges">';
  for (const e of edges) {
    const dead = onCycle.includes(e.txn);
    html += `<div class="edge${dead ? ' dead' : ''}">` +
      `<span class="node${dead ? ' dead' : ''}">txn=${e.txn}</span>` +
      `<span class="arrow">等待 ←</span>` +
      (e.blockers.length
        ? e.blockers.map((b) => `<span class="node${onCycle.includes(b) ? ' dead' : ''}">txn=${b}</span>`).join('')
        : '<span class="node none">无</span>') +
      (e.resource ? `<span class="res">[${esc(e.mode)} ${esc(e.resource)}]</span>` : '') +
      '</div>';
  }
  return html + '</div></div>';
}

function renderTxns(text) {
  const t = parseTxns(text);
  const body = t.txns.slice().reverse().map((x) => [
    `txn=${x.id}`,
    `<span class="tstate ${esc(x.state)}">${esc(x.state)}</span>`,
    String(x.stmts),
    String(x.undo),
    esc(x.tables.join(', ')) || '—',
  ]);
  return `<div class="diagwrap"><div class="diagline">共 <b>${t.total}</b> 个事务 · 活动 <b>${t.active}</b> · ` +
    `已提交 <b>${t.committed}</b> · 已回滚 <b>${t.aborted}</b></div>` +
    table(['事务', '状态', '语句数', 'undo 记录', '涉及表'], body) + '</div>';
}

function renderPlanTree(text, label) {
  const nodes = parsePlanTree(text);
  if (!nodes.length) return '<div class="empty" style="padding:12px">（无）</div>';
  return `<div class="plantree"><div class="plantreehead">${esc(label)}</div>` + nodes.map((n) => {
    const op = n.text.split(' ')[0];
    return `<div class="planrow" style="padding-left:${n.depth * 16}px">` +
      `<span class="planop">${esc(op)}</span>` +
      `<span class="planrest">${esc(n.text.slice(op.length))}</span></div>`;
  }).join('') + '</div>';
}

// ── 底部面板开关 ────────────────────────────────────────────
export function initBottomPanel() {
  const panel = document.getElementById('bottomPanel');
  const title = document.getElementById('bpTitle');
  const body = document.getElementById('bpBody');
  let lastPlan = null;   // 最近一次计划：点状态栏「计划」时不必重跑查询
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

  // 结构化 / 原始文本 双视图切换条
  function viewBar(onPick) {
    const bar = document.createElement('div');
    bar.className = 'viewbar';
    const b1 = document.createElement('button');
    b1.className = 'btn diagtab';
    b1.textContent = '结构化';
    b1.dataset.view = '0';
    const b2 = document.createElement('button');
    b2.className = 'btn diagtab';
    b2.textContent = '原始文本';
    b2.dataset.view = '1';
    for (const b of [b1, b2]) {
      b.addEventListener('click', () => {
        bar.querySelectorAll('.diagtab').forEach((o) => o.classList.toggle('primary', o === b));
        onPick(b.dataset.view === '1');
      });
    }
    bar.append(b1, b2);
    return bar;
  }

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
      if (arg) lastPlan = arg;
      const p = arg || lastPlan;
      if (!p || (!p.before && !p.after)) {
        body.innerHTML = '<div class="empty" style="padding:12px">没有计划（运行查询后生成，DDL 也有计划）</div>';
        return;
      }
      const wrap = document.createElement('div');
      wrap.className = 'plan2col';
      const mk = (label, text) => {
        const d = document.createElement('div');
        d.innerHTML = renderPlanTree(text, label);
        return d;
      };
      wrap.append(mk('优化前', p.before), mk('优化后', p.after));
      const head = document.createElement('div');
      head.className = 'planhead';
      head.innerHTML = '<span>计划文本已按缩进解析成算子树</span>' +
        (p.elapsedMs != null ? `<span class="chip">编译耗时 ${Number(p.elapsedMs).toFixed(2)} ms</span>` : '');
      body.append(head, wrap);
    } else if (name === 'diag') {
      title.textContent = '诊断';
      const views = {
        stats: { name: '缓冲池', render: renderStats },
        locks: { name: '锁表', render: renderLocks },
        waitfor: { name: '等待图', render: renderWaitFor },
        txn: { name: '事务表', render: renderTxns },
      };
      let cur = views[localStorage.getItem('cella.diag')] ? localStorage.getItem('cella.diag') : 'stats';
      let raw = false;
      let paused = false;
      let lastText = '';

      const bar = document.createElement('div');
      bar.className = 'diagbar';
      const tabBtns = [];
      for (const k of Object.keys(views)) {
        const b = document.createElement('button');
        b.className = 'btn diagtab';
        b.textContent = views[k].name;
        b.dataset.kind = k;
        b.addEventListener('click', () => {
          cur = k;
          localStorage.setItem('cella.diag', k);
          tabBtns.forEach((o) => o.classList.toggle('primary', o === b));
          load();
        });
        tabBtns.push(b);
        bar.appendChild(b);
      }
      const view = document.createElement('div');
      view.className = 'diagview';

      const draw = () => {
        view.innerHTML = raw
          ? `<pre>${esc(lastText || '（空）')}</pre>`
          : views[cur].render(lastText);
      };
      const vb = viewBar((isRaw) => { raw = isRaw; draw(); });
      const pause = document.createElement('label');
      pause.className = 'diagpause';
      pause.innerHTML = '<input type="checkbox" checked> 自动刷新 (2s)';
      pause.querySelector('input').addEventListener('change', (e) => { paused = !e.target.checked; });
      bar.append(vb, pause);

      const load = async () => {
        try {
          const d = await Api.diagnostics(cur);
          lastText = d.text || '';
          if (d.title) title.textContent = '诊断 · ' + d.title;
        } catch (e) {
          lastText = '加载失败：' + e.message;
        }
        draw();
      };

      tabBtns.forEach((o) => o.classList.toggle('primary', o.dataset.kind === cur));
      body.append(bar, view);
      setInterval(() => {
        if (!paused && !state$().busy && !panel.classList.contains('hidden')) load();
      }, 2000);
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
