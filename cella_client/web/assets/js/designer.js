// designer.js —— 表设计器（P5.6）：把「结构编辑」翻译成一串 ALTER TABLE。
//
// 为什么编排放在客户端：
//   cella 的 ALTER TABLE **一次只做一个动作**（ADD/DROP COLUMN、RENAME、ADD/DROP PRIMARY
//   KEY，见 cella_db/README §3）。引擎侧没有「一次改多列」的落点 —— 加了它就要处理
//   「部分成功」的语义。于是客户端把用户的一次编辑**差分成一串单动作语句**依次执行：
//   引擎保持最小接口，编排、校验、预览都在前端完成。
//
// 模型：列行**从不真正从数组里消失**，删除只是打上 `deleted` 标记（墓碑行）。
//   这是差分的硬前提 —— 要产出 `DROP COLUMN v`，就必须还记得 v 存在过；
//   若把行直接从数组里 splice 掉，diff 就只能看到「现状」，永远推不出「删了什么」。
//   同理，改名的行靠 `orig.name` 回忆旧名字。墓碑行的另一个好处是能一键撤销删除。
//
// 本模块分两层：
//   * 纯函数层 diffSchema()：给「列模型」产出语句序列与诊断。无 DOM、无网络 ——
//     因此可以在 _selftest 里逐条断言（designer.html）。
//   * 视图层 createDesigner()：可编辑列清单 + 实时预览 + 执行。
//
// 引擎不支持的能力（改列类型/长度、把已有列改成 NOT NULL）会**明确报错**而不是
// 静默丢弃 —— 用户看到的是「引擎不支持」，而不是「点了没反应」。

import { Api } from './api.js';

export const TYPES = ['INT', 'FLOAT', 'DOUBLE', 'CHAR', 'VARCHAR', 'TEXT', 'DATE', 'TIME', 'DATETIME'];

const CHARLIKE = new Set(['CHAR', 'VARCHAR']);
const IDENT_RE = /^[A-Za-z_][A-Za-z0-9_]*$/;

const eqName = (a, b) => String(a).toLowerCase() === String(b).toLowerCase();

// 列的类型声明文本：字符族带长度（未填默认 255，与引擎建表一致）
export function typeSql(c) {
  const t = String(c.type || 'INT').toUpperCase();
  if (!CHARLIKE.has(t)) return t;
  const n = Number.isFinite(+c.len) && +c.len > 0 ? Math.floor(+c.len) : 255;
  return `${t}(${n})`;
}

// 目录（/api/catalog/{table}）→ 设计器列模型。
// id 是「行的稳定身份」：改名时名字变了但 id 不变 → 差分据此识别 RENAME 而不是 DROP+ADD。
let seq = 0;
export function columnsFromCatalog(table) {
  const out = [];
  for (const c of (table && table.columns) || []) {
    out.push({
      id: 'c' + (++seq),
      name: c.name,
      type: String(c.type || 'INT').toUpperCase(),
      len: c.len || 0,
      notNull: !!c.notNull,
      primaryKey: !!c.primaryKey,
      deleted: false,
      orig: {
        name: c.name,
        type: String(c.type || 'INT').toUpperCase(),
        len: c.len || 0,
        notNull: !!c.notNull,
        primaryKey: !!c.primaryKey,
      },
    });
  }
  return out;
}

// 新建列（还没有对应的物理列）
export function newColumn(name = '') {
  return {
    id: 'c' + (++seq),
    name,
    type: 'INT',
    len: 0,
    notNull: false,
    primaryKey: false,
    deleted: false,
    orig: null,
  };
}

// 打删除标记（墓碑）。已有列标 deleted；还没落库的新增列直接丢弃 —— 它本来就没存在过，
// 留着只会让预览里出现无意义的墓碑。
export function deleteColumn(cols, i) {
  const c = cols[i];
  if (!c) return;
  if (c.orig) c.deleted = true;
  else cols.splice(i, 1);
}

export function restoreColumn(c) {
  if (c) c.deleted = false;
}

// 界面上要显示的行（墓碑行也显示，划掉以示「将删除」）
export const visibleColumns = (cols) => cols;

// ── 纯函数：差分 ──────────────────────────────────────────────
//
// 返回 { statements: [{sql, kind}], errors: [string], warnings: [string] }。
// errors 非空时 statements 仍会给出（便于预览），但执行按钮应当禁用。
//
// 语句顺序是**有约束的**，不是随便排的：
//   ① RENAME 先做 —— 后面的 DROP/ADD/ADD PRIMARY KEY 才能直接用新列名；
//   ② DROP PRIMARY KEY 必须在 DROP COLUMN 之前 —— 主键列不允许直接删；
//   ③ ADD COLUMN 必须在 ADD PRIMARY KEY 之前 —— 新列要先存在。
export function diffSchema(tableName, cols) {
  const errors = [];
  const warnings = [];
  const statements = [];

  const origAll = cols.filter((c) => c.orig);                 // 原有列（含墓碑）
  const origCols = origAll.filter((c) => !c.deleted);         // 保留的原有列
  const droppedCols = origAll.filter((c) => c.deleted);       // 待删除的原有列
  const addedCols = cols.filter((c) => !c.orig && !c.deleted);
  const liveCols = origCols.concat(addedCols);

  // 「一列都不剩」有两种来源：把原有列全删了、或者新建表时一列都没加。都拦掉 ——
  // 引擎侧拒绝零列表，放行只会让用户在执行时撞一个更难懂的错。
  if (!liveCols.length) {
    errors.push('表至少要有一列');
  }

  // 列名合法性 + 重名（不区分大小写，与引擎一致）。墓碑行不参与。
  const seen = new Map();
  for (const c of liveCols) {
    const n = String(c.name || '').trim();
    if (!n) {
      errors.push('存在未命名的列');
      continue;
    }
    if (n.toLowerCase() === 'rowid') {
      errors.push(`列名 ${n} 非法：rowid 是每张表都有的只读伪列`);
    } else if (!IDENT_RE.test(n)) {
      errors.push(`列名 ${n} 非法：只允许字母/数字/下划线，且不能以数字开头`);
    }
    const k = n.toLowerCase();
    if (seen.has(k)) {
      errors.push(`列名重复：${n}`);
    }
    seen.set(k, c);
  }

  // 已有列的能力边界：引擎只支持改名，不支持改类型/长度/放宽非空
  for (const c of origCols) {
    if (String(c.type).toUpperCase() !== c.orig.type || (+c.len || 0) !== (+c.orig.len || 0)) {
      errors.push(
        `列 ${c.orig.name}：引擎不支持修改列的类型/长度（${c.orig.type} → ${typeSql(c)}）。` +
          '改为「删掉这一列 + 新增一列」，或另建新表迁移数据。'
      );
    }
    if (!c.orig.notNull && c.notNull && !c.primaryKey) {
      errors.push(
        `列 ${c.orig.name}：引擎不支持把已有列改成 NOT NULL（没有 SET NOT NULL 动作）。` +
          '可删掉后按 NOT NULL 重新新增，或在 INSERT 侧约束。'
      );
    }
  }

  // ── ① 列改名（拓扑序：目标名空闲的先做，支持 a→b, b→c 这类链；成环则报错）──
  // current[nameLower] = 现在占用这个名字的列的 id
  const current = new Map();
  for (const c of liveCols) {
    const k = c.orig ? String(c.orig.name).toLowerCase() : String(c.name).trim().toLowerCase();
    current.set(k, c.id);
  }
  const renames = [];
  for (const c of origCols) {
    const from = c.orig.name;
    const to = String(c.name).trim();
    if (eqName(from, to)) continue;
    renames.push({ id: c.id, from, to });
  }
  const pending = renames.slice();
  let guard = pending.length + 1;
  while (pending.length && guard-- > 0) {
    let progressed = false;
    for (let i = 0; i < pending.length; ++i) {
      const r = pending[i];
      const holder = current.get(r.to.toLowerCase());
      if (holder === undefined || holder === r.id) {
        statements.push({
          sql: `ALTER TABLE ${tableName} RENAME COLUMN ${r.from} TO ${r.to};`,
          kind: 'RENAME COLUMN',
        });
        // 释放旧名，占用新名
        for (const [k, v] of Array.from(current)) {
          if (v === r.id) current.delete(k);
        }
        current.set(r.to.toLowerCase(), r.id);
        pending.splice(i, 1);
        progressed = true;
        break;
      }
    }
    if (!progressed) break;
  }
  if (pending.length) {
    errors.push(
      '列名互换需要分两次执行（引擎要求目标名先空闲）：' +
        pending.map((r) => `${r.from}→${r.to}`).join('、')
    );
  }

  // ── ② 主键差异（按列身份比对，改名不算改主键）──────────────
  const origPkIds = new Set(origAll.filter((c) => c.orig.primaryKey).map((c) => c.id));
  const newPkIds = new Set(liveCols.filter((c) => c.primaryKey).map((c) => c.id));
  const pkSame = origPkIds.size === newPkIds.size && [...origPkIds].every((id) => newPkIds.has(id));
  if (!pkSame && origPkIds.size) {
    statements.push({ sql: `ALTER TABLE ${tableName} DROP PRIMARY KEY;`, kind: 'DROP PRIMARY KEY' });
  }

  // ── ③ 删列（墓碑行）────────────────────────────────────────
  for (const c of droppedCols) {
    statements.push({
      sql: `ALTER TABLE ${tableName} DROP COLUMN ${c.orig.name};`,
      kind: 'DROP COLUMN',
    });
  }

  // ── ④ 加列 ────────────────────────────────────────────────
  for (const c of addedCols) {
    let s = `ALTER TABLE ${tableName} ADD COLUMN ${String(c.name).trim()} ${typeSql(c)}`;
    // 主键由独立的 ADD PRIMARY KEY 语句统一处理（引擎的 ADD COLUMN 不接 PRIMARY KEY）
    if (c.notNull || c.primaryKey) s += ' NOT NULL';
    statements.push({ sql: s + ';', kind: 'ADD COLUMN' });
    if (c.notNull || c.primaryKey) {
      warnings.push(
        `新增列 ${String(c.name).trim()} 声明了 NOT NULL：表里已有数据时会失败（无法为老行补值）。`
      );
    }
  }

  // ── ⑤ 加主键 ──────────────────────────────────────────────
  if (!pkSame && newPkIds.size) {
    const names = liveCols.filter((c) => c.primaryKey).map((c) => String(c.name).trim());
    statements.push({
      sql: `ALTER TABLE ${tableName} ADD PRIMARY KEY (${names.join(', ')});`,
      kind: 'ADD PRIMARY KEY',
    });
    warnings.push('新建主键要求该列在已有数据上「无 NULL 且无重复」，否则会被拒绝。');
  }

  // 「新建表」形态：没有任何已有列 → 直接给 CREATE TABLE 更自然
  if (!origAll.length) {
    statements.length = 0;
    const defs = liveCols.map((c) => {
      let s = `  ${String(c.name).trim()} ${typeSql(c)}`;
      if (c.primaryKey) s += ' PRIMARY KEY';
      else if (c.notNull) s += ' NOT NULL';
      return s;
    });
    statements.push({ sql: `CREATE TABLE ${tableName}(\n${defs.join(',\n')}\n);`, kind: 'CREATE TABLE' });
  }

  return { statements, errors, warnings };
}

// ─────────────────────────────────────────────────────────────
// 视图层
// ─────────────────────────────────────────────────────────────

const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

// 挂载设计器到标签面板。deps: { onMessage, onApplied }
export function createDesigner(pane, tab, deps = {}) {
  let cols = columnsFromCatalog(tab.table);
  let tableName = tab.table.name;
  let lastDiff = null;

  pane.classList.add('designer');
  pane.innerHTML = `
    <div class="dsgbar">
      <span class="dsgtable">表 <b>${esc(tableName)}</b></span>
      <span class="hint">改写列名 = 改名；类型/长度与「已有列改非空」引擎不支持，会在这里提示</span>
      <span class="spacer"></span>
      <button class="btn" data-act="add">+ 新增列</button>
      <button class="btn" data-act="reset">重置</button>
      <button class="btn primary" data-act="apply" disabled>执行改动</button>
    </div>
    <div class="dsgwrap"><table class="grid dsggrid">
      <thead><tr><th>#</th><th>列名</th><th>类型</th><th>长度</th><th>非空</th><th>主键</th><th>原结构</th><th></th></tr></thead>
      <tbody></tbody>
    </table></div>
    <div class="dsgpreview">
      <div class="dsgtitle">将执行的 SQL（按顺序）</div>
      <pre class="dsgsql"></pre>
      <div class="dsgfixed"></div>
    </div>`;

  const tbody = pane.querySelector('.dsggrid tbody');
  const sqlPre = pane.querySelector('.dsgsql');
  const fixedEl = pane.querySelector('.dsgfixed');
  const applyBtn = pane.querySelector('[data-act="apply"]');

  function origText(c) {
    if (!c.orig) return '<span class="newtag">新增</span>';
    const bits = [`${c.orig.type}${CHARLIKE.has(c.orig.type) ? '(' + (c.orig.len || 255) + ')' : ''}`];
    if (c.orig.primaryKey) bits.push('PK');
    else if (c.orig.notNull) bits.push('NOT NULL');
    const renamed = !eqName(c.orig.name, c.name) ? ` ← ${esc(c.orig.name)}` : '';
    return esc(bits.join(' ')) + renamed;
  }

  function renderRows() {
    tbody.innerHTML = '';
    cols.forEach((c, i) => {
      const tr = document.createElement('tr');
      tr.dataset.i = String(i);
      if (!c.orig) tr.classList.add('new');
      if (c.deleted) tr.classList.add('del');
      const lenOn = CHARLIKE.has(String(c.type).toUpperCase());
      const off = c.deleted ? ' disabled' : '';
      tr.innerHTML = `
        <td class="num">${i + 1}</td>
        <td><input class="dsgin" data-f="name" value="${esc(c.name)}" spellcheck="false"${off}></td>
        <td>
          <select class="dsgin dsgsel" data-f="type"${off}>
            ${TYPES.map((t) => `<option value="${t}"${t === c.type ? ' selected' : ''}>${t}</option>`).join('')}
          </select>
        </td>
        <td><input class="dsgin dsgnum" data-f="len" value="${lenOn ? (c.len || 255) : ''}"
                   ${lenOn && !c.deleted ? '' : 'disabled'} title="${lenOn ? '字符列长度' : '仅 CHAR/VARCHAR 需要'}"></td>
        <td class="ctr"><input type="checkbox" data-f="notNull" ${c.notNull ? 'checked' : ''}${off}></td>
        <td class="ctr"><input type="radio" name="dsgpk" data-f="primaryKey" ${c.primaryKey ? 'checked' : ''}${off}></td>
        <td class="orig">${origText(c)}</td>
        <td class="ctr"><span class="dsgdel" title="${c.deleted ? '撤销删除' : '删除这一列'}">${c.deleted ? '↺' : '✕'}</span></td>
      `;
      tbody.appendChild(tr);
    });
    refresh();
  }

  function refresh() {
    lastDiff = diffSchema(tableName, cols);
    const { statements, errors, warnings } = lastDiff;
    sqlPre.textContent = statements.length
      ? statements.map((s, i) => `${i + 1}. ${s.sql}`).join('\n')
      : '（没有结构变化）';
    const lines = [];
    for (const e of errors) lines.push(`<div class="dsgerr">✕ ${esc(e)}</div>`);
    for (const w of warnings) lines.push(`<div class="dsgwarn">⚠ ${esc(w)}</div>`);
    fixedEl.innerHTML = lines.join('');
    applyBtn.disabled = errors.length > 0 || statements.length === 0;
    applyBtn.textContent = statements.length ? `执行改动 (${statements.length})` : '执行改动';
  }

  // 输入 → 模型（输入事件是委托的：行会被整块重画，不能绑在行上）
  pane.addEventListener('input', (e) => {
    const tr = e.target.closest('tr[data-i]');
    if (!tr) return;
    const c = cols[+tr.dataset.i];
    if (!c) return;
    const f = e.target.dataset.f;
    if (f === 'name') c.name = e.target.value;
    else if (f === 'len') c.len = parseInt(e.target.value, 10) || 0;
    // 必须重算并重绘预览：改名的行**不能**整块重画（会把光标踢出输入框），
    // 但「将要执行的 SQL」必须跟着变，否则用户看不到自己的改动会生效。
    refresh();
  });
  pane.addEventListener('change', (e) => {
    const tr = e.target.closest('tr[data-i]');
    if (!tr) return;
    const c = cols[+tr.dataset.i];
    if (!c) return;
    const f = e.target.dataset.f;
    if (f === 'type') {
      c.type = e.target.value;
      renderRows();  // 长度输入框的可用性跟着类型变
      return;
    }
    if (f === 'notNull') c.notNull = e.target.checked;
    if (f === 'primaryKey' && e.target.checked) {
      // 主键是单选的：其余行取消（引擎的 ADD PRIMARY KEY 允许复合，但 UI 先只暴露单列）
      for (const o of cols) o.primaryKey = false;
      c.primaryKey = true;
      c.notNull = true;
      renderRows();
      return;
    }
    refresh();
  });

  pane.addEventListener('click', (e) => {
    const act = e.target.dataset && e.target.dataset.act;
    if (act === 'add') {
      cols.push(newColumn(`new_col${cols.filter((c) => !c.orig).length + 1}`));
      renderRows();
      return;
    }
    if (act === 'reset') {
      cols = columnsFromCatalog(tab.table);
      renderRows();
      return;
    }
    if (act === 'apply') {
      apply();
      return;
    }
    if (e.target.classList.contains('dsgdel')) {
      const tr = e.target.closest('tr[data-i]');
      if (!tr) return;
      const i = +tr.dataset.i;
      if (cols[i] && cols[i].deleted) restoreColumn(cols[i]);
      else deleteColumn(cols, i);
      renderRows();
    }
  });

  async function apply() {
    if (!lastDiff || lastDiff.errors.length || !lastDiff.statements.length) return;
    applyBtn.disabled = true;
    const sql = lastDiff.statements.map((s) => s.sql).join('\n');
    try {
      const d = await Api.query(sql);
      // 语句级结果：ok=false 时错误在 error.{code,message}（见 api_service Query 端点）
      const bad = (d.statements || []).filter((s) => !s.ok);
      if (deps.onMessage) deps.onMessage({ sql, data: d, bad });
      if (bad.length) {
        // 前面的语句已经生效：这是引擎「一次一个动作」的必然结果，必须如实说明
        const e0 = bad[0].error || {};
        alert(
          `共 ${lastDiff.statements.length} 条改动，第 ${bad[0].index} 条失败：\n` +
            `${e0.message || e0.code || '未知错误'}\n\n` +
            '（前面已成功的语句不会回滚 —— cella 的 DDL 不做事务回滚）'
        );
      }
      if (deps.onApplied) await deps.onApplied();
      return d;
    } catch (err) {
      alert('执行失败：' + (err && err.message ? err.message : err));
    } finally {
      // 不能在这里无条件 disabled=false：onApplied 里的 reload→renderRows→refresh 已经
      // 按**新结构**算过一遍「还有没有改动」。无条件置回可用会让「刚执行完、当前无改动」
      // 的按钮显示成可点，用户再点一次就是空跑一轮。
      refresh();
    }
  }

  renderRows();

  return {
    // 目录刷新后重新载入（执行成功 / 外部改动）
    reload(table) {
      tab.table = table;
      tableName = table.name;
      pane.querySelector('.dsgtable').innerHTML = `表 <b>${esc(tableName)}</b>`;
      cols = columnsFromCatalog(table);
      renderRows();
    },
    get columns() { return cols; },
    get diff() { return lastDiff; },
  };
}
