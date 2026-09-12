// editor.js —— SQL 编辑器：<textarea> + 底层 <pre> 高亮层（PLAN §5.2）。
// 高亮用单遍扫描状态机（普通/字符串/注释），不用正则替换（PLAN §6.6）。
// 关键字表与引擎保留字一致（PLAN §11.5）。

const KEYWORDS = new Set(('CREATE TABLE PRIMARY KEY ALTER DROP TRUNCATE RENAME INSERT INTO VALUES ' +
  'UPDATE SET DELETE FROM WHERE GET IN LIMIT GROUPED HAVING ORDERED AMONG PAGE JOIN ON LEFT RIGHT ' +
  'MIDDLE UNION DISTINCT AS AND OR NOT NULL TRUE FALSE ASC DESC IS INT INTEGER FLOAT DOUBLE CHAR ' +
  'VARCHAR TEXT DATE TIME DATETIME').split(' '));

function esc(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

// 单遍扫描：状态 = 普通 | 行注释 | 块注释 | 字符串
function highlight(sql) {
  let out = '';
  let i = 0;
  const n = sql.length;
  let plain = '';
  const flush = () => { if (plain) { out += esc(plain); plain = ''; } };
  while (i < n) {
    const c = sql[i];
    if (c === '-' && sql[i + 1] === '-') {
      flush();
      let j = i;
      while (j < n && sql[j] !== '\n') j++;
      out += '<span class="tok-com">' + esc(sql.slice(i, j)) + '</span>';
      i = j;
      continue;
    }
    if (c === '/' && sql[i + 1] === '*') {
      flush();
      let j = i + 2;
      while (j < n && !(sql[j] === '*' && sql[j + 1] === '/')) j++;
      j = j < n ? j + 2 : n;
      out += '<span class="tok-com">' + esc(sql.slice(i, j)) + '</span>';
      i = j;
      continue;
    }
    if (c === "'") {
      flush();
      let j = i + 1;
      while (j < n) {
        if (sql[j] === "'" && sql[j + 1] === "'") { j += 2; continue; }
        if (sql[j] === "'") { j++; break; }
        j++;
      }
      out += '<span class="tok-str">' + esc(sql.slice(i, j)) + '</span>';
      i = j;
      continue;
    }
    if (/[A-Za-z_]/.test(c)) {
      let j = i;
      while (j < n && /[A-Za-z0-9_]/.test(sql[j])) j++;
      const word = sql.slice(i, j);
      if (KEYWORDS.has(word.toUpperCase())) {
        flush();
        out += '<span class="tok-kw">' + word + '</span>';
      } else {
        plain += word;
      }
      i = j;
      continue;
    }
    if (/[0-9]/.test(c)) {
      flush();
      let j = i;
      while (j < n && /[0-9.]/.test(sql[j])) j++;
      out += '<span class="tok-num">' + esc(sql.slice(i, j)) + '</span>';
      i = j;
      continue;
    }
    plain += c;
    i++;
  }
  flush();
  return out;
}

// 简单格式化：关键字前换行、统一缩进
export function formatSql(sql) {
  const K = ['get ', 'in ', 'limit ', 'ordered ', 'grouped ', 'having ', 'among ', 'page ',
    'union ', 'insert into ', 'values ', 'update ', 'set ', 'delete in ', 'create table '];
  let s = sql.replace(/\s+/g, ' ').trim();
  for (const k of K) {
    const re = new RegExp('\\s+' + k.replace(/ /g, '\\s+'), 'gi');
    s = s.replace(re, '\n' + k.trim().toUpperCase() + ' ');
  }
  const lines = s.split('\n').map((l) => l.trim()).filter(Boolean);
  return lines.join('\n') + (/[;]$/.test(lines[lines.length - 1] || '') ? '' : ';');
}

export function createEditor(container, { onRun, onChange } = {}) {
  container.classList.add('editorwrap');
  const pre = document.createElement('pre');
  const ta = document.createElement('textarea');
  ta.spellcheck = false;
  ta.setAttribute('aria-label', 'SQL 编辑器');
  container.append(pre, ta);

  const render = () => {
    const v = ta.value;
    pre.innerHTML = highlight(v) + '\n';
  };
  const sync = () => { pre.scrollTop = ta.scrollTop; };

  ta.addEventListener('input', () => { render(); if (onChange) onChange(ta.value); });
  ta.addEventListener('scroll', sync);

  ta.addEventListener('keydown', (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
      e.preventDefault();
      if (onRun) onRun(ta.selectionStart !== ta.selectionEnd ? ta.value.substring(ta.selectionStart, ta.selectionEnd) : ta.value);
      return;
    }
    if (e.key === 'Tab') {
      e.preventDefault();
      const { selectionStart: a, selectionEnd: b } = ta;
      ta.value = ta.value.slice(0, a) + '  ' + ta.value.slice(b);
      ta.selectionStart = ta.selectionEnd = a + 2;
      render();
      return;
    }
    if (e.key === 'Enter' && !e.ctrlKey && !e.shiftKey) {
      // 自动缩进：继承上一行前导空白
      const a = ta.selectionStart;
      const lineStart = ta.value.lastIndexOf('\n', a - 1) + 1;
      const indent = /^[ \t]*/.exec(ta.value.slice(lineStart, a))[0];
      if (indent) {
        e.preventDefault();
        ta.value = ta.value.slice(0, a) + '\n' + indent + ta.value.slice(ta.selectionEnd);
        ta.selectionStart = ta.selectionEnd = a + 1 + indent.length;
        render();
      }
    }
  });

  return {
    el: container,
    getValue: () => ta.value,
    setValue: (v) => { ta.value = v; render(); },
    focus: () => ta.focus(),
    // 定位到脚本绝对行列（错误跳转）
    gotoLine: (line, col) => {
      const lines = ta.value.split('\n');
      const idx = (() => {
        let acc = 0;
        for (let k = 0; k < Math.min(line - 1, lines.length); k++) acc += lines[k].length + 1;
        return acc;
      })();
      ta.focus();
      ta.selectionStart = ta.selectionEnd = Math.min(idx + Math.max(0, col - 1), ta.value.length);
      const lh = 20;
      ta.scrollTop = Math.max(0, (line - 3) * lh);
      sync();
    },
  };
}
