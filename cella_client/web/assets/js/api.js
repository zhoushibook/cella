// api.js —— fetch 封装 + 统一错误 + 访问控制令牌（不依赖任何视图，见 PLAN §5.4.1）。

const TOKEN_KEY = 'cella.token';

let token = '';
try { token = localStorage.getItem(TOKEN_KEY) || ''; } catch (e) { token = ''; }

export function setToken(t) {
  token = t || '';
  try {
    if (token) localStorage.setItem(TOKEN_KEY, token);
    else localStorage.removeItem(TOKEN_KEY);
  } catch (e) { /* 无痕模式等：退化为仅内存保存 */ }
}

export function getToken() { return token; }

// 401 回调（由 app.js 注入：弹出登录框）
export const Auth = { onUnauthorized: null };

// 连接状态：服务进程挂掉时页面要能直说「已断开」，而不是每条请求各弹一个 toast
export const Conn = {
  online: true,
  listeners: new Set(),
  onLine(fn) { Conn.listeners.add(fn); return () => Conn.listeners.delete(fn); },
  set(online) {
    if (online === Conn.online) return;
    Conn.online = online;
    for (const fn of Conn.listeners) fn(online);
  },
};

export class ApiError extends Error {
  constructor(payload) {
    super(payload.message || '请求失败');
    this.code = payload.code || '';
    this.line = payload.line || 0;
    this.col = payload.col || 0;
    this.detail = payload.detail || '';
    this.status = payload.status || 0;
  }
}

async function call(method, path, body) {
  const opt = { method, headers: {} };
  if (token) opt.headers['Authorization'] = 'Bearer ' + token;
  if (body !== undefined) {
    opt.headers['Content-Type'] = 'application/json; charset=utf-8';
    opt.body = JSON.stringify(body);
  }
  let resp;
  try {
    resp = await fetch(path, opt);
  } catch (e) {
    Conn.set(false);
    throw new ApiError({ code: 'NET', message: '无法连接服务（可能已停止运行）' });
  }
  Conn.set(true);
  if (resp.status === 401) {
    setToken('');
    if (Auth.onUnauthorized) Auth.onUnauthorized();
    throw new ApiError({ code: 'HTTP-401', message: '未登录或登录已过期，请重新登录', status: 401 });
  }
  let json = null;
  try {
    json = await resp.json();
  } catch (e) {
    throw new ApiError({ code: 'HTTP-' + resp.status, message: '服务返回非 JSON（HTTP ' + resp.status + '）' });
  }
  if (!json || json.ok !== true) {
    const err = (json && json.error) || {};
    err.status = resp.status;
    throw new ApiError(err);
  }
  return json.data;
}

export const Api = {
  health: () => call('GET', '/api/health'),
  login: (user, password) => call('POST', '/api/login', { user, password }),
  logout: () => call('POST', '/api/logout'),
  databases: () => call('GET', '/api/databases'),
  useDb: (name) => call('POST', '/api/databases/use', { name }),
  createDb: (name) => call('POST', '/api/databases/create', { name }),
  dropDb: (name) => call('POST', '/api/databases/drop', { name }),
  query: (sql, maxRows) => call('POST', '/api/query', { sql, maxRows }),
  plan: (sql) => call('POST', '/api/plan', { sql }),
  session: () => call('GET', '/api/session'),
  txn: (op) => call('POST', '/api/txn/' + op),
  checkpoint: () => call('POST', '/api/checkpoint'),
  diagnostics: (kind) => call('GET', '/api/diagnostics/' + kind),
  catalog: () => call('GET', '/api/catalog'),
  tableInfo: (t) => call('GET', '/api/catalog/' + encodeURIComponent(t)),
  rows: (t, q) => {
    const qs = new URLSearchParams();
    if (q.page) qs.set('page', q.page);
    if (q.pageSize) qs.set('pageSize', q.pageSize);
    if (q.sort) qs.set('sort', q.sort);
    if (q.order) qs.set('order', q.order);
    return call('GET', '/api/tables/' + encodeURIComponent(t) + '/rows?' + qs.toString());
  },
  count: (t) => call('GET', '/api/tables/' + encodeURIComponent(t) + '/count'),
  insertRow: (t, values) => call('POST', '/api/tables/' + encodeURIComponent(t) + '/rows', { values }),
  updateRow: (t, payload) => call('PATCH', '/api/tables/' + encodeURIComponent(t) + '/rows', payload),
  deleteRow: (t, payload) => call('DELETE', '/api/tables/' + encodeURIComponent(t) + '/rows', payload),
};
