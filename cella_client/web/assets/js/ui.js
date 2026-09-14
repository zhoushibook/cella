// ui.js —— 布局分隔条拖拽 + 界面偏好持久化（PLAN §5.4.1：与视图无关的通用工具）。
//
// 所有「可拖拽的边」都走同一个 startDrag：mousedown 后把 mousemove/mouseup 挂到 document，
// 避免鼠标移出元素后丢事件；拖拽期间统一 body 光标与禁用文本选择。

const PREF_PREFIX = 'cella.ui.';

export function loadNum(key, def, min, max) {
  let v = def;
  try {
    const raw = localStorage.getItem(PREF_PREFIX + key);
    if (raw !== null) {
      const n = parseFloat(raw);
      if (!Number.isNaN(n)) v = n;
    }
  } catch (e) { /* 无痕模式：用默认值 */ }
  return Math.max(min, Math.min(max, v));
}

export function saveNum(key, v) {
  try { localStorage.setItem(PREF_PREFIX + key, String(Math.round(v))); } catch (e) { /* 忽略 */ }
}

// 把一个元素变成拖拽手柄；onMove 收到 (dx, dy) 相对按下点的位移
// noClass=true 时不自动加 .splitter/.v/.h（元素已有自己的定位样式时用）
export function makeSplitter(el, { axis = 'x', cursor, onMove, onEnd, onStart, noClass = false } = {}) {
  const cur = cursor || (axis === 'x' ? 'col-resize' : 'row-resize');
  if (!noClass) el.classList.add('splitter', axis === 'x' ? 'v' : 'h');
  el.addEventListener('mousedown', (e) => {
    if (e.button !== 0) return;
    e.preventDefault();
    e.stopPropagation();
    const x0 = e.clientX;
    const y0 = e.clientY;
    const prevCursor = document.body.style.cursor;
    const prevSelect = document.body.style.userSelect;
    document.body.style.cursor = cur;
    document.body.style.userSelect = 'none';
    el.classList.add('dragging');
    if (onStart) onStart();
    const move = (ev) => onMove(ev.clientX - x0, ev.clientY - y0, ev);
    const up = () => {
      document.removeEventListener('mousemove', move);
      document.removeEventListener('mouseup', up);
      document.body.style.cursor = prevCursor;
      document.body.style.userSelect = prevSelect;
      el.classList.remove('dragging');
      if (onEnd) onEnd();
    };
    document.addEventListener('mousemove', move);
    document.addEventListener('mouseup', up);
  });
  return el;
}

// 拖动后把值写进 CSS 变量（供布局使用）
export function setVar(name, px) {
  document.documentElement.style.setProperty(name, px + 'px');
}
