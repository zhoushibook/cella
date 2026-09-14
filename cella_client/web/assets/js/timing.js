// timing.js —— 查询耗时的格式化 / 记录 / 呈现。
//
// 为什么一次执行报三个口径，而不是一个「耗时」：
//   engineMs  引擎内「编译 + 执行」合计（StatementOutcome::elapsed_ms）—— 查询本身有多快；
//   serverMs  服务端处理器墙钟（含排队等 gate_ 引擎锁 + 结果 JSON 编码）；
//   wallMs    浏览器发请求到收到响应（再含 HTTP、网络、前端 JSON 解析）。
// 只报一个数会骗人：同一条 SQL 的 wallMs 抖动往往与查询无关（并发排队、结果集大）。
// 横向对比查询本身看 engineMs，评估用户实际等待看 wallMs。
//
// 记录只放内存（一次会话），不落 localStorage：跨会话的耗时没有可比性
// （缓存冷热、数据量、进程状态都变了），存下来只会误导。

import { state$, set } from './store.js';

const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

export const TIMING_LIMIT = 60;   // 最多留最近 60 次

// 速度分档：只用于配色（绿/黄/红），阈值按「本地库」的体感定
const FAST_MS = 10;
const SLOW_MS = 100;

export function speedClass(ms) {
  const v = Number(ms);
  if (!Number.isFinite(v)) return '';
  if (v >= SLOW_MS) return 'slow';
  if (v >= FAST_MS) return 'mid';
  return 'fast';
}

// 统一毫秒口径（需求要求以毫秒为单位）：只在数值很大时收掉小数，单位不变
export function fmtMs(x) {
  const v = Number(x);
  if (!Number.isFinite(v) || v < 0) return '—';
  if (v >= 1000) return v.toFixed(0) + ' ms';
  if (v >= 100) return v.toFixed(1) + ' ms';
  return v.toFixed(2) + ' ms';
}

const num = (x) => (Number.isFinite(Number(x)) ? Number(x) : 0);

// 空白折叠：同一条 SQL 换行/缩进不同也应视作同一条，否则「相对上一次」永远对不上
export function normalizeSql(sql) {
  return String(sql || '').replace(/\s+/g, ' ').trim();
}

function shortSql(sql, n = 72) {
  const s = String(sql || '');
  return s.length > n ? s.slice(0, n) + '…' : s;
}

// ── 记录 ────────────────────────────────────────────────────
export function recordRun(r) {
  const prevSeq = state$().timings.length ? state$().timings[0].seq : 0;
  const rec = {
    seq: prevSeq + 1,
    sql: normalizeSql(r.sql),
    kind: r.kind || '',
    ok: r.ok !== false,
    rows: Number.isFinite(Number(r.rows)) ? Number(r.rows) : null,
    engineMs: num(r.engineMs),
    serverMs: num(r.serverMs),
    wallMs: num(r.wallMs),
    at: Date.now(),
  };
  const list = [rec, ...state$().timings];
  if (list.length > TIMING_LIMIT) list.length = TIMING_LIMIT;
  set({ timings: list });
  return rec;
}

export function clearRuns() {
  set({ timings: [] });
}

// 同一条 SQL 的上一次记录（更早的才算），用于算变化率
function previousSame(list, index) {
  const sql = list[index].sql;
  for (let i = index + 1; i < list.length; i++) {
    if (list[i].sql === sql) return list[i];
  }
  return null;
}

// ── 呈现：结果栏里的耗时徽标 ────────────────────────────────
export function chipHtml(rec) {
  if (!rec) return '';
  const title = [
    `引擎 ${fmtMs(rec.engineMs)}（编译 + 执行）`,
    `服务端 ${fmtMs(rec.serverMs)}（含排队与结果编码）`,
    `端到端 ${fmtMs(rec.wallMs)}（含 HTTP 与前端解析）`,
    rec.rows === null ? '' : `结果 ${rec.rows} 行`,
    '点这里看历史耗时对比',
  ].filter(Boolean).join('\n');
  return `<span class="mschip ${speedClass(rec.engineMs)}" data-ms="${rec.engineMs.toFixed(2)}" ` +
    `title="${esc(title)}">⏱ ${fmtMs(rec.engineMs)}</span>`;
}

// 执行中的实时计时徽标（长查询期间让用户看到已经跑了多久）
export function liveChipHtml(elapsedMs) {
  return `<span class="mschip running" title="执行中">⏱ 执行中 ${fmtMs(elapsedMs)}</span>`;
}

// ── 呈现：底部「耗时」面板（历史对比）──────────────────────
export function renderTiming(host) {
  const list = state$().timings;
  if (!list.length) {
    host.innerHTML = '<div class="empty" style="padding:12px">' +
      '还没有耗时记录。每次执行查询后这里会留下一条，方便横向对比不同查询。</div>';
    return;
  }
  const engines = list.map((r) => r.engineMs);
  const min = Math.min(...engines);
  const max = Math.max(...engines);
  const avg = engines.reduce((a, b) => a + b, 0) / engines.length;
  const scale = Math.max(max, 0.001);

  const rows = list.map((r, i) => {
    const prev = previousSame(list, i);
    let delta = '<span class="delta first">首次</span>';
    if (prev) {
      const base = prev.engineMs;
      // 基数过小（<0.05ms）时百分比没有意义，直接给差值
      if (base < 0.05) {
        delta = `<span class="delta">${fmtMs(r.engineMs)}</span>`;
      } else {
        const pct = ((r.engineMs - base) / base) * 100;
        const cls = Math.abs(pct) < 5 ? 'same' : (pct > 0 ? 'up' : 'down');
        const txt = Math.abs(pct) < 5 ? '≈持平' : (pct > 0 ? '+' : '') + pct.toFixed(0) + '%';
        delta = `<span class="delta ${cls}" title="上一次同语句 ${fmtMs(base)}">${txt}</span>`;
      }
    }
    const w = Math.max(2, Math.round((r.engineMs / scale) * 100));
    return `<tr class="${r.ok ? '' : 'bad'}">` +
      `<td class="num">${r.seq}</td>` +
      `<td class="sql" title="${esc(r.sql)}">${esc(shortSql(r.sql))}</td>` +
      `<td class="num">${r.rows === null ? '—' : r.rows}</td>` +
      `<td class="num nowrap"><span class="msbar"><i class="${speedClass(r.engineMs)}" ` +
      `style="width:${w}%"></i></span>${fmtMs(r.engineMs)}</td>` +
      `<td class="num">${fmtMs(r.serverMs)}</td>` +
      `<td class="num">${fmtMs(r.wallMs)}</td>` +
      `<td class="num">${delta}</td></tr>`;
  }).join('');

  host.innerHTML =
    '<div class="timingwrap">' +
    `<div class="diagline timinghead">最近 <b>${list.length}</b> 次 · ` +
    `引擎最快 <b>${fmtMs(min)}</b> · 最慢 <b>${fmtMs(max)}</b> · 平均 <b>${fmtMs(avg)}</b>` +
    '<span class="spacer"></span>' +
    '<button class="btn" data-act="timing-clear">清空记录</button></div>' +
    '<table class="grid timing"><thead><tr>' +
    '<th>#</th><th>语句</th><th>行数</th><th>引擎</th><th>服务端</th><th>端到端</th><th>相对上次同语句</th>' +
    '</tr></thead><tbody>' + rows + '</tbody></table>' +
    '<div class="diagline">引擎 = 编译 + 执行（对比查询本身快慢用这个）· ' +
    '服务端 = 含排队等引擎锁与结果编码 · 端到端 = 再含 HTTP 与前端解析。</div>' +
    '</div>';

  const btn = host.querySelector('[data-act="timing-clear"]');
  if (btn) btn.addEventListener('click', clearRuns);
}
