// Shared helpers for every page. Plain ES modules, no build step.

const TOKEN_KEY = 'campusx:token';

// Note: localStorage is readable by any script on the page. That's why every
// piece of user content is rendered with h() below (text nodes only, never
// innerHTML), which blocks the XSS that could steal it.
export const token = {
  get() {
    try { return localStorage.getItem(TOKEN_KEY); } catch { return null; }
  },
  set(value) {
    try {
      if (value) localStorage.setItem(TOKEN_KEY, value);
      else localStorage.removeItem(TOKEN_KEY);
    } catch { /* storage disabled: session lasts until reload */ }
  },
};

export class ApiError extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}

/** Calls the C++ API. `form` is sent as application/x-www-form-urlencoded. */
export async function api(path, { method = 'GET', form, signal, auth = true } = {}) {
  const headers = {};
  const t = auth ? token.get() : null;
  if (t) headers.Authorization = `Bearer ${t}`;
  const body = form ? new URLSearchParams(form) : undefined;

  let res;
  try {
    res = await fetch(path, { method, headers, body, signal });
  } catch (err) {
    if (err.name === 'AbortError') throw err;
    throw new ApiError(0, 'Cannot reach the server. Check your connection.');
  }
  if (res.status === 204) return null;
  const data = await res.json().catch(() => null);
  if (!res.ok) {
    if (res.status === 401 && t) token.set(null);  // session expired
    throw new ApiError(res.status, data?.error || `Request failed (${res.status}).`);
  }
  return data;
}

/**
 * Builds DOM elements safely: strings become text nodes, never HTML.
 *   h('p', { class: 'muted' }, 'Hello ', h('strong', {}, name))
 */
export function h(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  setAttrs(el, attrs);
  appendAll(el, children);
  return el;
}

/** Same as h() but for SVG elements. */
export function svg(tag, attrs = {}, ...children) {
  const el = document.createElementNS('http://www.w3.org/2000/svg', tag);
  setAttrs(el, attrs);
  appendAll(el, children);
  return el;
}

function setAttrs(el, attrs) {
  for (const [key, value] of Object.entries(attrs || {})) {
    if (value == null || value === false) continue;
    if (key.startsWith('on') && typeof value === 'function') el.addEventListener(key.slice(2), value);
    else if (key === 'class') el.setAttribute('class', value);
    else el.setAttribute(key, value === true ? '' : String(value));
  }
}

function appendAll(el, children) {
  for (const child of children.flat(Infinity)) {
    if (child == null || child === false) continue;
    el.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
}

export async function currentUser() {
  if (!token.get()) return null;
  try {
    return (await api('/api/auth/me')).user;
  } catch {
    return null;
  }
}

/** An icon from /img/icons.svg. Decorative unless you pass a label. */
export function icon(name, { cls = '', label = null } = {}) {
  return svg('svg', { class: `icon ${cls}`.trim(), 'aria-hidden': label ? null : 'true', role: label ? 'img' : null, 'aria-label': label },
    svg('use', { href: `/img/icons.svg#${name}` }));
}

/** Round avatar with initials; the colour pair is picked from the name so it stays stable. */
export function avatar(name = '?', size = '') {
  const initials = name.replace(/\(.*?\)/g, '').trim().split(/\s+/)
    .filter((w) => /^[A-Za-z]/.test(w) && !w.endsWith('.'))  // skip titles like "Dr."
    .slice(0, 2).map((w) => w[0].toUpperCase()).join('') || '?';
  let hash = 0;
  for (const c of name) hash = (hash * 31 + c.charCodeAt(0)) >>> 0;
  return h('span', { class: `avatar avatar-${hash % 6} ${size}`.trim(), 'aria-hidden': 'true' }, initials);
}

/* ---------- Theme (dark by default, remembered per browser) ---------- */
const THEME_KEY = 'campusx:theme';
export const currentTheme = () => document.documentElement.dataset.theme || 'dark';

function applyTheme(theme) {
  document.documentElement.dataset.theme = theme;
  try { localStorage.setItem(THEME_KEY, theme); } catch { /* not saved, still applied */ }
  const btn = document.getElementById('theme-toggle');
  if (btn) {
    const next = theme === 'dark' ? 'light' : 'dark';
    btn.replaceChildren(icon(theme === 'dark' ? 'sun' : 'moon'));
    btn.setAttribute('aria-label', `Switch to ${next} theme`);
    btn.title = `Switch to ${next} theme`;
  }
  document.dispatchEvent(new CustomEvent('campusx:theme', { detail: theme }));
}

/** Highlights the current page, wires the theme button and shows login state. Returns the user (or null). */
export async function initHeader() {
  // The directions page belongs to Search (that's where you open it from).
  const page = document.body.dataset.page === 'navigate' ? 'search' : document.body.dataset.page;
  document.querySelectorAll('.topnav a').forEach((a) => {
    if (a.dataset.page === page) a.setAttribute('aria-current', 'page');
  });

  const toggle = document.getElementById('theme-toggle');
  if (toggle) {
    applyTheme(currentTheme());
    toggle.addEventListener('click', () => {
      toggle.classList.remove('spin');
      void toggle.offsetWidth;  // restart the spin animation
      toggle.classList.add('spin');
      applyTheme(currentTheme() === 'dark' ? 'light' : 'dark');
    });
  }

  const slot = document.getElementById('user-slot');
  const user = await currentUser();
  if (user) {
    const logout = async () => {
      try { await api('/api/auth/logout', { method: 'POST' }); } catch { /* already logged out */ }
      token.set(null);
      location.href = '/login.html';
    };
    slot.replaceChildren(
      h('span', { class: 'user-chip', title: `${user.name} (${user.role})` }, avatar(user.name, 'avatar-sm'),
        h('span', { class: 'user-chip-name' }, user.name)),
      h('button', { type: 'button', class: 'icon-btn', 'aria-label': 'Log out', title: 'Log out', onclick: logout }, icon('logout')));
  } else {
    slot.replaceChildren(h('a', { class: 'btn btn-glass btn-sm', href: `/login.html?next=${encodeURIComponent(location.pathname)}` }, icon('login'), 'Log in'));
  }
  return user;
}

/** Animates a number from 0 to `to` (skipped when the user prefers reduced motion). */
export function countUp(el, to, ms = 900) {
  const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;
  if (reduce || !Number.isFinite(to)) { el.textContent = String(to); return; }
  const start = performance.now();
  const step = (now) => {
    const t = Math.min(1, (now - start) / ms);
    el.textContent = String(Math.round(to * (1 - (1 - t) ** 3)));
    if (t < 1) requestAnimationFrame(step);
  };
  requestAnimationFrame(step);
}

export function timeAgo(ms) {
  const s = Math.round((Date.now() - ms) / 1000);
  if (s < 60) return 'just now';
  const m = Math.round(s / 60);
  if (m < 60) return `${m} min ago`;
  const hrs = Math.round(m / 60);
  if (hrs < 24) return `${hrs} h ago`;
  return new Date(ms).toLocaleDateString(undefined, { day: 'numeric', month: 'short' });
}

export function debounce(fn, ms) {
  let timer;
  return (...args) => {
    clearTimeout(timer);
    timer = setTimeout(() => fn(...args), ms);
  };
}
