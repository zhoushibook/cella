// editor.js —— SQL 编辑器：<textarea> + 底层 <pre> 高亮层（PLAN §5.2）。
// 高亮用单遍扫描状态机（普通/字符串/注释），不用正则替换（PLAN §6.6）。
// 关键字表与引擎保留字一致（PLAN §11.5），另补上会话层拦截的管理语句关键字。
// 高度可拖：下边缘的分隔条（双击复位），偏好持久化在 localStorage。

import { loadNum, saveNum, makeSplitter } from './ui.js';

const KEYWORDS = new Set(('CREATE TABLE PRIMARY KEY ALTER DROP TRUNCATE RENAME INSERT INTO VALUES ' +
  'UPDATE SET DELETE FROM WHERE GET IN LIMIT GROUPED HAVING ORDERED AMONG PAGE JOIN ON LEFT RIGHT ' +
  'MIDDLE UNION DISTINCT AS AND OR NOT NULL TRUE FALSE ASC DESC IS INT INTEGER FLOAT DOUBLE CHAR ' +
  'VARCHAR TEXT DATE TIME DATETIME ' +
  // 会话层管理语句（引擎拦在编译器之前，高亮跟上）
  'USER USERS GRANT GRANTS REVOKE SHOW IDENTIFIED BY TO FROM ADMIN ALL READ PASSWORD ' +
  'DATABASE DATABASES USE BEGIN COMMIT ROLLBACK TRANSACTION CHECKPOINT EXPLAIN').split(' '));

const PAIRS = { '(': ')', '[': ']', '{': '}', "'": "'", '"': '"' };

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

// 简单格式化：关键字前换行 + 关键字统一大写、统一缩进
export function formatSql(sql) {
  const K = ['get ', 'in ', 'limit ', 'ordered ', 'grouped ', 'having ', 'among ', 'page ',
    'union ', 'insert into ', 'values ', 'update ', 'set ', 'delete in ', 'create table '];
  let s = sql.replace(/\s+/g, ' ').trim();
  for (const k of K) {
    // 开头也要能命中（原来只匹配「空白 + 关键字」，行首关键字大小写不会被规范）
    const re = new RegExp('(^|\\s+)' + k.replace(/ /g, '\\s+'), 'gi');
    s = s.replace(re, '\n' + k.trim().toUpperCase() + ' ');
  }
  s = s.replace(/^\n+/, '');
  const lines = s.split('\n').map((l) => l.trim()).filter(Boolean);
  return lines.join('\n') + (/[;]$/.test(lines[lines.length - 1] || '') ? '' : ';');
}

export function createEditor(container, { onRun, onChange, onHistory } = {}) {
  container.classList.add('editorwrap');
  const pre = document.createElement('pre');
  const ta = document.createElement('textarea');
  ta.spellcheck = false;
  ta.setAttribute('aria-label', 'SQL 编辑器');
  container.append(pre, ta);

  // 编辑器高度可拖（拖下边缘；双击复位），偏好写到 localStorage
  let height = loadNum('editorH', 168, 80, 900);
  let baseH = height;      // 按下瞬间的实际高度：onMove 的 dy 是累计位移，基准不能累加
  container.style.height = height + 'px';
  const grip = document.createElement('div');
  grip.className = 'edresize';
  grip.title = '拖动调整编辑器高度（双击复位）';
  container.appendChild(grip);
  makeSplitter(grip, {
    axis: 'y',
    noClass: true,
    onStart: () => { baseH = container.getBoundingClientRect().height; },
    onMove: (dx, dy) => {
      height = Math.max(80, Math.min(900, Math.round(baseH + dy)));
      container.style.height = height + 'px';
      sync();
    },
    onEnd: () => saveNum('editorH', height),
  });
  grip.addEventListener('dblclick', () => {
    height = 168;
    container.style.height = height + 'px';
    saveNum('editorH', height);
    sync();
  });

  const render = () => {
    pre.innerHTML = highlight(ta.value) + '\n';
  };
  const sync = () => { pre.scrollTop = ta.scrollTop; };

  // 选区涉及的行区间（含首尾整行）
  function lineRange() {
    const v = ta.value;
    const a = ta.selectionStart;
    const b = ta.selectionEnd;
    const ls = v.lastIndexOf('\n', a - 1) + 1;
    let le = v.indexOf('\n', b);
    if (le < 0) le = v.length;
    return { ls, le, a, b };
  }

  function replaceRange(ls, le, text, selStart, selEnd) {
    const v = ta.value;
    ta.value = v.slice(0, ls) + text + v.slice(le);
    ta.selectionStart = selStart;
    ta.selectionEnd = selEnd === undefined ? selStart : selEnd;
    render();
    if (onChange) onChange(ta.value);
  }

  // Ctrl+/ —— 行注释开关
  function toggleComment() {
    const { ls, le, a, b } = lineRange();
    const lines = ta.value.slice(ls, le).split('\n');
    const list = lines.filter((l) => l.trim());
    const allCommented = list.length > 0 && list.every((l) => /^\s*--/.test(l));
    const out = lines.map((l) => {
      if (!l.trim()) return l;
      return allCommented ? l.replace(/^(\s*)--\s?/, '$1') : l.replace(/^(\s*)/, '$1-- ');
    }).join('\n');
    const delta = out.length - (le - ls);
    const ns = Math.max(ls, a + (allCommented ? -3 : 3));
    replaceRange(ls, le, out, ns, Math.max(ns, b + delta));
  }

  function indentBlock(dedent) {
    const { ls, le, a, b } = lineRange();
    const lines = ta.value.slice(ls, le).split('\n');
    const out = dedent ? lines.map((l) => l.replace(/^ {1,2}/, '')).join('\n')
      : lines.map((l) => '  ' + l).join('\n');
    const delta = out.length - (le - ls);
    replaceRange(ls, le, out, Math.max(ls, a + (dedent ? -2 : 2)), Math.max(ls, b + delta));
  }

  ta.addEventListener('input', () => { render(); if (onChange) onChange(ta.value); });
  ta.addEventListener('scroll', sync);

  ta.addEventListener('keydown', (e) => {
    const mod = e.ctrlKey || e.metaKey;

    if (mod && e.key === 'Enter') {
      e.preventDefault();
      if (onRun) onRun(ta.selectionStart !== ta.selectionEnd ? ta.value.substring(ta.selectionStart, ta.selectionEnd) : ta.value);
      return;
    }
    if (mod && e.key === '/') {          // Ctrl+/ 注释
      e.preventDefault();
      toggleComment();
      return;
    }
    if (mod && (e.key === 'ArrowUp' || e.key === 'ArrowDown') && onHistory) {
      e.preventDefault();
      onHistory(e.key === 'ArrowUp' ? -1 : 1);
      return;
    }
    if (e.key === 'Tab') {
      e.preventDefault();
      indentBlock(e.shiftKey);
      return;
    }
    if (e.key === 'Enter' && !e.shiftKey) {
      // 自动缩进：继承上一行前导空白；若上一行以 ( 结尾再缩进一级
      const a = ta.selectionStart;
      const v = ta.value;
      const lineStart = v.lastIndexOf('\n', a - 1) + 1;
      const lineText = v.slice(lineStart, a);
      const indent = /^[ \t]*/.exec(lineText)[0];
      const extra = /[([]\s*$/.test(lineText) ? '  ' : '';
      if (indent || extra) {
        e.preventDefault();
        replaceRange(a, ta.selectionEnd, '\n' + indent + extra, a + 1 + indent.length + extra.length);
      }
      return;
    }
    // 右符号：下一个字符已是该符号则直接跳过（不打双份）
    if ([')', ']', '}', '"', "'"].includes(e.key) && !mod &&
        ta.selectionStart === ta.selectionEnd && ta.value[ta.selectionStart] === e.key) {
      e.preventDefault();
      ta.selectionStart = ta.selectionEnd = ta.selectionStart + 1;
      return;
    }
    // 括号/引号自动配对（有选区时包裹选区）
    if (PAIRS[e.key] && !mod) {
      const close = PAIRS[e.key];
      const a = ta.selectionStart;
      const b = ta.selectionEnd;
      e.preventDefault();
      if (a !== b) {
        replaceRange(a, b, e.key + ta.value.slice(a, b) + close, a + 1, b + 1);
      } else {
        replaceRange(a, a, e.key + close, a + 1);
      }
      return;
    }
    if (e.key === 'Backspace' && ta.selectionStart === ta.selectionEnd) {
      const p = ta.selectionStart;
      const prev = ta.value[p - 1];
      const next = ta.value[p];
      if (prev && next && PAIRS[prev] === next) {   // 删左半边时连带删右半边
        e.preventDefault();
        replaceRange(p - 1, p + 1, '', p - 1);
      }
    }
  });

  return {
    el: container,
    ta,
    getValue: () => ta.value,
    setValue: (v, opts = {}) => {
      const st = opts.keepState ? { start: ta.selectionStart, end: ta.selectionEnd, scroll: ta.scrollTop } : null;
      ta.value = v;
      render();
      if (onChange) onChange(ta.value);
      if (st) {
        ta.selectionStart = Math.min(st.start, v.length);
        ta.selectionEnd = Math.min(st.end, v.length);
        ta.scrollTop = st.scroll;
        sync();
      } else {
        ta.selectionStart = ta.selectionEnd = v.length;
        ta.scrollTop = 0;
        sync();
      }
    },
    focus: () => ta.focus(),
    // 光标/滚动位置（标签切换时保留）
    getState: () => ({ start: ta.selectionStart, end: ta.selectionEnd, scroll: ta.scrollTop }),
    restoreState: (s) => {
      if (!s) return;
      ta.selectionStart = Math.min(s.start, ta.value.length);
      ta.selectionEnd = Math.min(s.end, ta.value.length);
      ta.scrollTop = s.scroll || 0;
      sync();
    },
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
