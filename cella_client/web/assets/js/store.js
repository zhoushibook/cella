// store.js —— 应用状态唯一持有者（PLAN §5.4.1：组件只订阅，不互相引用）。

const state = {
  theme: localStorage.getItem('cella.theme') || 'light',
  health: null,
  authEnabled: false,  // 服务端是否启用访问控制
  user: '',            // 当前登录用户（空 = 未登录）
  isAdmin: false,
  databases: [],
  currentDb: 'main',
  tables: [],          // [{name, columns, primaryKey, ...}]
  treeFilter: '',
  tabs: [],            // [{id, type:'query'|'data'|'struct'|'diag', title, ...}]
  activeTab: null,
  inTxn: false,
  txnId: -1,
  busy: false,
  statusText: '',
  lastResult: null,    // 最近一次查询（导出用）
};

const subs = new Set();

export function state$() { return state; }

export function set(patch) {
  Object.assign(state, patch);
  for (const fn of subs) fn(state);
}

export function subscribe(fn) { subs.add(fn); return () => subs.delete(fn); }

export function tableByName(name) {
  const n = (name || '').toLowerCase();
  return state.tables.find((t) => t.name.toLowerCase() === n) || null;
}

export function pkOf(table) {
  if (!table) return [];
  return (table.primaryKey && table.primaryKey.columns) || [];
}
