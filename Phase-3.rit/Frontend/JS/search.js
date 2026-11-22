import { api, debounce, h, icon, initHeader } from './common.js';

const KINDS = [['all', 'All', 'layers'], ['place', 'Places', 'building'], ['faculty', 'People', 'user'], ['event', 'Events', 'calendar']];
const KIND_INFO = { place: ['Place', 'building'], faculty: ['Person', 'user'], event: ['Event', 'calendar'] };

const input = document.getElementById('q');
const kindsEl = document.getElementById('kinds');
const statusEl = document.getElementById('search-status');
const resultsEl = document.getElementById('results');

const params = new URLSearchParams(location.search);
let kind = params.get('kind') || 'all';
let controller = null;
input.value = params.get('q') || '';

function renderKinds() {
  kindsEl.replaceChildren(...KINDS.map(([value, label, ic]) =>
    h('button', {
      type: 'button', role: 'radio', 'aria-checked': String(kind === value),
      class: `chip ${kind === value ? 'is-on' : ''}`,
      onclick: () => { kind = value; renderKinds(); run(); },
    }, icon(ic), label)));
}

function setStatus(content, isError = false) {
  statusEl.replaceChildren(...[content].flat().filter(Boolean));
  statusEl.className = isError ? 'status-line error' : 'status-line';
}

function resultItem(r, i) {
  const [label, ic] = KIND_INFO[r.kind];
  const when = r.kind === 'event' && r.startsAt
    ? h('p', { class: 'result-when' }, icon('clock'), new Date(r.startsAt).toLocaleString(undefined, { weekday: 'short', day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' }))
    : null;
  const action = r.navNodeId
    ? h('a', { class: 'btn btn-glass btn-sm', href: `/navigate.html?to=${encodeURIComponent(r.navNodeId)}` }, icon('route'), 'Directions')
    : h('span', { class: 'small muted' }, 'Not on map yet');
  return h('li', { class: `result panel kind-${r.kind}`, style: `--i:${Math.min(i, 8)}` },
    h('span', { class: 'result-icon', title: label }, icon(ic, { label })),
    h('div', { class: 'result-main' },
      h('h2', {}, r.title),
      r.subtitle ? h('p', { class: 'muted' }, r.subtitle) : null,
      when,
      r.detail ? h('p', { class: 'small result-detail' }, r.detail) : null),
    action);
}

async function run() {
  const q = input.value.trim();
  history.replaceState(null, '', q ? `?${new URLSearchParams({ q, kind })}` : location.pathname);
  controller?.abort();  // a slow old response must not overwrite a newer one
  if (q.length < 2) {
    resultsEl.replaceChildren();
    setStatus('Type at least two letters. Small typos are fine.');
    return;
  }
  controller = new AbortController();
  setStatus([h('span', { class: 'spinner', 'aria-hidden': 'true' }), 'Searching…']);
  try {
    const data = await api(`/api/search?${new URLSearchParams({ q, kind, limit: 20 })}`, { signal: controller.signal, auth: false });
    resultsEl.replaceChildren(...data.results.map(resultItem));
    const time = data.tookMicros < 1000 ? `${data.tookMicros} µs` : `${(data.tookMicros / 1000).toFixed(1)} ms`;
    setStatus(data.results.length
      ? [icon('sparkle'), `${data.results.length} result${data.results.length === 1 ? '' : 's'} in ${time}`,
        h('span', { class: 'muted' }, data.cached ? ' (from cache)' : ' (trie + typo matching)')]
      : `Nothing matches “${q}”. Try a building code, a department or a shorter word.`);
  } catch (err) {
    if (err.name !== 'AbortError') setStatus(err.message, true);
  }
}

renderKinds();
input.addEventListener('input', debounce(run, 250));
initHeader();
if (input.value) run();
