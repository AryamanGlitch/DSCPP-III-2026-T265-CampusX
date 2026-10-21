import { api, ApiError, debounce, h, icon, initHeader, svg } from './common.js';
import { browserMeasure, layoutFloor, layoutOutdoor } from './mapLayout.js';

/*
 * Online:  the C++ server computes the route (GET /api/emergency/route).
 * Offline: the page follows the cached routing table that the server built
 *          with multi-source Dijkstra (graph.tables[mode].next). No algorithm
 *          runs in the browser, just "go to next[current]" until you arrive.
 */

const CACHE_KEY = 'campusx:emergency-graph:v1';
const POLL_MS = 30_000;
const TYPES = [
  { value: 'fire', label: 'Fire', icon: 'flame', advice: 'Leave now. Use stairs, never the lift. Do not stop for belongings.' },
  { value: 'earthquake', label: 'Earthquake', icon: 'quake', advice: 'Drop, cover and hold on until the shaking stops. Then leave and stay clear of buildings.' },
  { value: 'medical', label: 'Medical', icon: 'medical', advice: 'Call for help first if someone is unconscious or not breathing. The route goes to the medical centre.' },
];
// One icon per kind of direction step.
const STEP_ICONS = { walk: 'walk', stairs: 'stairs', elevator: 'lift', exit: 'door', enter: 'door', arrive: 'flag' };

const params = new URLSearchParams(location.search);
const state = {
  graph: null, source: null, savedAt: null,
  type: 'fire', accessible: false,
  from: params.get('from') || '',  // filled by room QR codes
  to: params.get('to') || '',      // filled by "Directions" in search
  route: null, user: null,
};
let routeRequest = 0;

// Everyday directions have their own page (navigate.html) so they never sit under
// the red Emergency tab. Old links like emergency.html?to=X are forwarded there.
const PAGE = document.body.dataset.page;  // 'emergency' or 'navigate'
if (PAGE === 'emergency' && state.to) location.replace(`/navigate.html${location.search}`);
if (PAGE === 'navigate' && !state.to) location.replace('/search.html');
const goToEmergency = () => { location.href = `/emergency.html${state.from ? `?from=${encodeURIComponent(state.from)}` : ''}`; };

/** "+910000000000" -> "+91 00000 00000": easier to read and can wrap on narrow cards. */
function formatPhone(raw) {
  const d = raw.replace(/[^\d+]/g, '');
  const m = d.match(/^\+91(\d{5})(\d{5})$/);
  return m ? `+91 ${m[1]} ${m[2]}` : raw;
}

const $ = (id) => document.getElementById(id);
const byId = () => new Map((state.graph?.nodes || []).map((n) => [n.id, n]));
const floorLabel = (f) => (f === 0 ? 'Ground floor' : `Floor ${f}`);
const edgeKey = (a, b) => (a < b ? `${a}|${b}` : `${b}|${a}`);
const blockedSet = () => new Set((state.graph?.hazards || []).map((x) => x.nodeId).filter(Boolean));

/* ---------------- Graph loading + offline cache ---------------- */
function readCache() {
  try { return JSON.parse(localStorage.getItem(CACHE_KEY)); } catch { return null; }
}
function writeCache(entry) {
  try { localStorage.setItem(CACHE_KEY, JSON.stringify(entry)); } catch { /* storage full or disabled */ }
}

async function loadGraph() {
  const before = state.graph?.generatedAt;
  const wasLive = state.source === 'live';
  try {
    const graph = await api('/api/emergency/graph', { auth: false });
    const savedAt = Date.now();
    writeCache({ graph, savedAt });
    Object.assign(state, { graph, source: 'live', savedAt });
  } catch {
    if (!state.graph) {
      const cached = readCache();
      if (cached) Object.assign(state, { graph: cached.graph, savedAt: cached.savedAt });
    }
    state.source = state.graph ? 'cache' : 'none';
  }
  renderStatus();
  // The server rebuilds the graph only when the map or hazards change. Skip
  // re-rendering otherwise, so a 30 s poll doesn't wipe a half-filled form.
  const changed = state.graph?.generatedAt !== before || (state.source === 'live') !== wasLive;
  if (changed || !state.route) {
    renderAll();
    await computeRoute();
  }
}

/* ---------------- Routing ---------------- */
function modeKey() {
  const base = state.type === 'medical' ? 'medical' : 'evacuate';
  return state.accessible ? `${base}_accessible` : base;
}

async function computeRoute() {
  const id = ++routeRequest;
  if (!state.graph || !state.from) { state.route = null; renderResult(); return; }
  const q = new URLSearchParams({ from: state.from, type: state.type, accessible: state.accessible ? '1' : '0' });
  if (state.to) q.set('to', state.to);
  let route;
  try {
    route = await api(`/api/emergency/route?${q}`, { auth: false });
  } catch (err) {
    if (err instanceof ApiError && err.status === 0) route = offlineRoute();
    else route = { found: false, reason: err.message };
  }
  if (id !== routeRequest) return;  // a newer request already finished
  state.route = route;
  renderResult();
}

/** Follow the cached next-hop table. Same answer the server would give. */
function offlineRoute() {
  if (state.to) return { found: false, reason: 'Everyday directions need a connection. Follow campus signs.' };
  const table = state.graph.tables?.[modeKey()];
  const nodes = byId();
  if (!table || !nodes.has(state.from) || table.dist[state.from] == null) {
    return { found: false, reason: 'No safe route found. Stay where you are, close doors between you and the hazard, and call an emergency contact.' };
  }
  const path = [state.from];
  const segments = [];
  for (let cur = state.from, guard = 0; table.next[cur] && guard <= nodes.size; guard++) {
    const nxt = table.next[cur];
    // Several edges may join two nodes; the one used is the one whose weight
    // equals the drop in distance-to-safety.
    const drop = table.dist[cur] - table.dist[nxt];
    const edge = state.graph.edges
      .filter((e) => edgeKey(e.from, e.to) === edgeKey(cur, nxt))
      .sort((a, b) => Math.abs(a.weight - drop) - Math.abs(b.weight - drop))[0];
    segments.push({ from: cur, to: nxt, weight: edge?.weight ?? drop, kind: edge?.kind ?? 'walk' });
    path.push(nxt);
    cur = nxt;
  }
  const target = nodes.get(path.at(-1));
  const distance = table.dist[state.from];
  return { found: true, offline: true, target, distance, etaSeconds: Math.round(distance / 1.2), path, segments, steps: offlineSteps(segments, target, nodes) };
}

/** Plain-language steps (mirrors NavService::directions in C++). */
function offlineSteps(segments, target, nodes) {
  if (!segments.length) return [{ kind: 'arrive', text: `You are already at ${target.name}.` }];
  const landmark = new Set(['exit', 'entrance', 'stairs', 'elevator', 'assembly', 'medical', 'refuge', 'room']);
  const steps = [];
  let walk = 0;
  const flush = (n) => { if (walk > 0) { steps.push({ kind: 'walk', text: `Walk ${Math.round(walk)} m to ${n.name}` }); walk = 0; } };
  segments.forEach((s, i) => {
    const a = nodes.get(s.from), b = nodes.get(s.to);
    if (s.kind === 'stairs' || s.kind === 'elevator') {
      flush(a);
      steps.push({ kind: s.kind, text: `Take the ${s.kind === 'stairs' ? 'stairs' : 'lift'} ${b.floor > a.floor ? 'up' : 'down'} to ${floorLabel(b.floor)}` });
      return;
    }
    if (a.building && !b.building) { flush(a); steps.push({ kind: 'exit', text: `Leave the building through ${a.name}` }); }
    if (!a.building && b.building) { flush(a); steps.push({ kind: 'enter', text: `Enter the building at ${b.name}` }); return; }
    walk += s.weight;
    if (i === segments.length - 1 || landmark.has(b.type)) flush(b);
  });
  return steps;
}

/* ---------------- Rendering ---------------- */
function renderAll() {
  renderStatus();
  renderContacts();
  renderMode();
  renderLocator();
  renderHazardBanner();
  renderAdmin();
}

function renderStatus() {
  const el = $('em-status');
  const time = state.savedAt ? new Date(state.savedAt).toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' }) : '';
  if (state.source === 'live') {
    el.className = 'pill pill-live';
    el.replaceChildren(h('span', { class: 'live-dot', 'aria-hidden': 'true' }), 'Live map');
    return;
  }
  el.className = 'pill pill-cache';
  el.replaceChildren(icon('wifi-off'), state.source === 'cache' ? `Offline copy from ${time}` : 'Map unavailable',
    h('button', { type: 'button', class: 'pill-btn', 'aria-label': 'Retry', title: 'Retry', onclick: loadGraph }, icon('refresh')));
}

function renderContacts() {
  const contacts = state.graph?.contacts;
  $('contacts').hidden = Boolean(state.to);  // everyday directions don't need emergency numbers
  if (!contacts?.length || state.to) return;
  $('contacts').replaceChildren(...contacts.map((c) =>
    h('a', { class: 'contact', href: `tel:${c.phone.replace(/[^\d+]/g, '')}`, 'aria-label': `Call ${c.name}, ${c.phone}` },
      icon('phone'),
      h('span', { class: 'contact-name' }, c.name),
      h('span', { class: 'contact-phone' }, formatPhone(c.phone)),
      c.available24x7 ? null : h('span', { class: 'contact-note' }, 'Not 24×7'))));
}

function renderMode() {
  const el = $('mode');
  if (!state.graph) { el.replaceChildren(); return; }
  if (state.to) {
    const dest = byId().get(state.to);
    $('em-title').textContent = `Directions to ${dest?.name ?? 'destination'}`;
    document.body.classList.add('nav-mode');
    el.replaceChildren(h('p', { class: 'nav-note' }, icon('route'), 'Everyday directions (lifts allowed). ',
      h('button', { type: 'button', class: 'btn-link', onclick: goToEmergency }, 'This is an emergency')),
      accessibleToggle());
    return;
  }
  const info = TYPES.find((t) => t.value === state.type);
  el.replaceChildren(h('fieldset', { class: 'type-picker' },
    h('legend', {}, 'What is happening?'),
    h('div', { class: 'type-buttons' }, TYPES.map((t) =>
      h('label', { class: `type-btn type-${t.value} ${state.type === t.value ? 'is-on' : ''}` },
        h('input', { type: 'radio', name: 'etype', value: t.value, checked: state.type === t.value,
          onchange: () => { state.type = t.value; renderMode(); computeRoute(); } }),
        icon(t.icon, { cls: 'icon-lg' }), h('span', {}, t.label)))),
    h('p', { class: 'advice' }, icon('alert'), info.advice),
    accessibleToggle()));
}

function accessibleToggle() {
  return h('label', { class: 'switch' },
    h('input', { type: 'checkbox', role: 'switch', checked: state.accessible,
      onchange: (e) => { state.accessible = e.target.checked; computeRoute(); } }),
    h('span', { class: 'switch-track', 'aria-hidden': 'true' }),
    icon('wheelchair'), "I can't use stairs");
}

/* ----- Where am I? ----- */
function renderLocator() {
  if (!state.graph) return;
  $('locator').hidden = false;
  const current = byId().get(state.from);
  const bSel = $('sel-building');
  const buildingCode = bSel.dataset.chosen ?? current?.building ?? '';
  bSel.replaceChildren(h('option', { value: '' }, 'Outdoors'),
    ...state.graph.buildings.map((b) => h('option', { value: b.code }, b.name)));
  bSel.value = buildingCode;
  fillFloorAndSpots(current);
}

function fillFloorAndSpots(current) {
  const bSel = $('sel-building'), fSel = $('sel-floor'), sSel = $('sel-spot');
  const building = state.graph.buildings.find((b) => b.code === bSel.value);
  $('floor-field').hidden = !building;
  $('spot-label').textContent = building ? 'Room or area' : 'Nearest landmark';
  if (building) {
    const prevFloor = fSel.value !== '' ? Number(fSel.value) : current?.floor ?? building.floors[0]?.level;
    fSel.replaceChildren(...building.floors.map((f) => h('option', { value: f.level }, f.name)));
    fSel.value = building.floors.some((f) => f.level === prevFloor) ? prevFloor : building.floors[0]?.level;
  }
  const spots = state.graph.nodes
    .filter((n) => (building ? n.building === building.code && n.floor === Number(fSel.value) : !n.building))
    .sort((a, b) => a.name.localeCompare(b.name));
  sSel.replaceChildren(h('option', { value: '', disabled: true }, 'Choose…'),
    ...spots.map((n) => h('option', { value: n.id }, n.name)));
  sSel.value = spots.some((n) => n.id === state.from) ? state.from : '';
}

$('sel-building').addEventListener('change', (e) => {
  e.target.dataset.chosen = e.target.value;
  $('sel-floor').value = '';
  fillFloorAndSpots(null);
});
$('sel-floor').addEventListener('change', () => fillFloorAndSpots(null));
$('sel-spot').addEventListener('change', (e) => { state.from = e.target.value; computeRoute(); });

$('gps').addEventListener('click', () => {
  const status = $('gps-status');
  if (!('geolocation' in navigator)) { status.textContent = 'This device has no GPS. Choose your location below.'; return; }
  status.textContent = 'Finding your position…';
  navigator.geolocation.getCurrentPosition(async (pos) => {
    const { latitude: lat, longitude: lng, accuracy } = pos.coords;
    let hit;
    try {
      hit = await api(`/api/emergency/nearest?${new URLSearchParams({ lat, lng })}`, { auth: false });
    } catch {
      hit = nearestOffline(lat, lng);
    }
    if (!hit || hit.distance > 300) { status.textContent = 'You appear to be off campus. Choose your location below.'; return; }
    state.from = hit.node.id;
    $('sel-building').dataset.chosen = '';
    renderLocator();
    status.textContent = `Matched to ${hit.node.name} (GPS accuracy about ${Math.round(accuracy)} m).` +
      (accuracy > 30 ? ' If you are inside a building, choose your room below instead.' : '');
    computeRoute();
  }, (err) => {
    status.textContent = err.code === err.PERMISSION_DENIED
      ? 'Location permission is off. Choose your location below.'
      : 'Could not get a GPS fix. Choose your location below.';
  }, { enableHighAccuracy: true, timeout: 10_000, maximumAge: 30_000 });
});

function nearestOffline(lat, lng) {
  const R = 6371000, rad = (d) => (d * Math.PI) / 180;
  let best = null;
  for (const n of state.graph?.nodes || []) {
    if (n.building || n.lat == null) continue;
    const a = Math.sin(rad(n.lat - lat) / 2) ** 2 + Math.cos(rad(lat)) * Math.cos(rad(n.lat)) * Math.sin(rad(n.lng - lng) / 2) ** 2;
    const d = 2 * R * Math.asin(Math.sqrt(a));
    if (!best || d < best.distance) best = { node: n, distance: d };
  }
  return best;
}

function renderHazardBanner() {
  const hz = state.graph?.hazards || [];
  const nodes = byId();
  $('hazard-banner').replaceChildren(hz.length
    ? h('div', { class: 'hazard-banner', role: 'status' }, icon('alert'), h('strong', {}, 'Blocked right now: '),
      hz.map((x) => nodes.get(x.nodeId)?.name ?? x.description).join(', '), '. Your route avoids these.')
    : '');
}

/* ----- Route result ----- */
function renderResult() {
  const el = $('result');
  const r = state.route;
  if (!state.graph) { el.replaceChildren(state.source === 'none' ? h('p', { class: 'error', role: 'alert' }, 'The campus map could not be loaded. Call one of the numbers above and follow the exit signs.') : ''); return; }
  if (!state.from) { el.replaceChildren(h('div', { class: 'empty-card' }, icon('pin', { cls: 'icon-lg' }), h('p', {}, 'Set your location above to see the safest route.'))); return; }
  if (!r) { el.replaceChildren(h('div', { class: 'skeleton skeleton-route' })); return; }
  if (!r.found) {
    el.replaceChildren(h('div', { class: 'route-fail', role: 'alert' }, icon('alert', { cls: 'icon-lg' }), h('p', {}, h('strong', {}, r.reason)),
      state.to ? null : h('p', {}, 'Use the call buttons at the top of this page.')));
    return;
  }
  const nodes = byId();
  const refuge = r.target.type === 'refuge'
    ? ' Wait here: rescuers check refuge areas first. Call security to say where you are.' : '';
  const areas = [];
  for (const id of r.path) {
    const n = nodes.get(id);
    const key = n?.building ? `${n.building}:${n.floor}` : 'outdoor';
    if (n && !areas.some((a) => a.key === key)) areas.push({ key, building: n.building, floor: n.floor });
  }
  el.replaceChildren(h('div', { class: 'route' },
    h('div', { class: 'route-summary' },
      h('span', { class: 'route-icon' }, icon(r.target.type === 'refuge' ? 'shield' : r.target.type === 'medical' ? 'medical' : 'flag')),
      h('div', {},
        h('p', { class: 'route-target' }, r.target.name),
        h('p', { class: 'route-meta' },
          h('span', {}, icon('route'), `${Math.round(r.distance)} m`),
          h('span', {}, icon('clock'), `about ${Math.max(1, Math.ceil(r.etaSeconds / 60))} min walking`)),
        refuge ? h('p', { class: 'route-note' }, refuge.trim()) : null,
        r.offline ? h('p', { class: 'route-note' }, icon('wifi-off'), 'Offline: route from the saved map. Hazards reported after it was saved are not included.') : null)),
    h('ol', { class: 'steps' }, r.steps.map((s, i) =>
      h('li', { class: `step step-${s.kind}`, style: `--i:${i}` }, h('span', { class: 'step-icon' }, icon(STEP_ICONS[s.kind] ?? 'walk')), h('span', {}, s.text)))),
    areas.map((a) => (a.key === 'outdoor' ? outdoorMap(r) : floorPlan(a.building, a.floor, r)))));
}

/** Width available for maps, in CSS pixels (minus the 2 px border on each side). */
const mapWidth = () => Math.max(320, Math.min(800, ($('result').clientWidth || 800) - 4));

function routeContext(route) {
  return {
    routeKeys: new Set(route.segments.map((s) => edgeKey(s.from, s.to))),
    routeIds: new Set(route.path),
    blocked: blockedSet(),
    startId: state.from,
    targetId: route.target.id,
    maxWidth: mapWidth(),
    measure: browserMeasure,
  };
}

/** Text with a halo, centred on (x, y). Drawn last so it sits above lines and dots. */
const mapLabel = (l) => svg('text', { x: l.x, y: l.y, class: l.cls, 'text-anchor': 'middle', 'dominant-baseline': 'central' }, l.text);

/** X mark for a blocked spot. */
const cross = (x, y, s) => `M${x - s} ${y - s} L${x + s} ${y + s} M${x + s} ${y - s} L${x - s} ${y + s}`;

/**
 * Route segments drawn in walking order, each with a staggered "draw" animation
 * (pathLength=1 lets CSS animate stroke-dashoffset from 1 to 0 for any length).
 */
function routeLines(edges, route, cls = 'fp-edge-route') {
  const segs = new Map(route.segments.map((s, i) => [edgeKey(s.from, s.to), { i, from: s.from }]));
  return edges.filter((e) => e.onRoute).map((e) => {
    const seg = segs.get(e.key);
    const forward = !seg || seg.from === e.from;  // draw in the direction you walk
    return svg('line', {
      x1: forward ? e.x1 : e.x2, y1: forward ? e.y1 : e.y2, x2: forward ? e.x2 : e.x1, y2: forward ? e.y2 : e.y1,
      class: cls, pathLength: 1, style: `--i:${seg?.i ?? 0}`,
    });
  });
}

function floorPlan(code, floor, route) {
  const building = state.graph.buildings.find((b) => b.code === code) ?? { name: code, floors: [] };
  const meta = building.floors.find((f) => f.level === floor) ?? { width: 60, height: 30, name: floorLabel(floor) };
  const onFloor = state.graph.nodes.filter((n) => n.building === code && n.floor === floor);
  const L = layoutFloor({ meta, nodes: onFloor, edges: state.graph.edges, ...routeContext(route) });
  return h('figure', { class: 'floorplan' },
    h('figcaption', {}, `${building.name}, ${meta.name}`),
    h('div', { class: 'scroll-x' }, svg('svg', {
      width: L.width, height: L.height, viewBox: `0 0 ${L.width} ${L.height}`, role: 'img',
      'aria-label': `Floor plan of ${building.name}, ${meta.name}, with your route highlighted`,
    },
    svg('rect', { class: 'fp-outline', x: L.outline.x, y: L.outline.y, width: L.outline.w, height: L.outline.h, rx: 3 }),
    L.edges.filter((e) => !e.onRoute).map((e) => svg('line', { x1: e.x1, y1: e.y1, x2: e.x2, y2: e.y2, class: 'fp-edge' })),
    routeLines(L.edges, route),
    L.nodes.map((n) => svg('g', { class: ['fp-node', `fp-${n.type}`, n.blocked && 'fp-blocked', n.start && 'fp-start'].filter(Boolean).join(' ') },
      svg('title', {}, n.blocked ? `${n.name} (blocked)` : n.name),  // hover name, also for unlabelled dots
      n.start ? svg('circle', { cx: n.x, cy: n.y, r: n.r, class: 'pulse' }) : null,
      svg('circle', { cx: n.x, cy: n.y, r: n.r }),
      n.blocked ? svg('path', { d: cross(n.x, n.y, n.r * 0.6), class: 'fp-x' }) : null)),
    L.labels.map(mapLabel))));
}

/** Schematic outdoor map drawn from node GPS points. No map tiles, so it works offline. */
function outdoorMap(route) {
  const nodes = byId();
  // If you start indoors, mark where the route comes out of the building.
  const leaveId = nodes.get(state.from)?.building ? route.path.find((id) => !nodes.get(id)?.building) : null;
  const L = layoutOutdoor({ nodes: state.graph.nodes, edges: state.graph.edges, leaveId, ...routeContext(route) });
  if (!L) return null;
  return h('figure', { class: 'outdoor-map' },
    h('figcaption', {}, 'Outdoor route (schematic, north is up)'),
    h('div', { class: 'scroll-x' }, svg('svg', {
      width: L.width, height: L.height, viewBox: `0 0 ${L.width} ${L.height}`, role: 'img',
      'aria-label': 'Outdoor campus map with your route',
    },
    L.edges.filter((e) => !e.onRoute).map((e) => svg('line', { x1: e.x1, y1: e.y1, x2: e.x2, y2: e.y2, class: 'om-edge' })),
    routeLines(L.edges, route, 'om-route'),
    L.nodes.map((n) => svg('g', { class: ['om-node', `om-${n.type}`, n.blocked && 'om-blocked', n.start && 'om-start'].filter(Boolean).join(' ') },
      svg('title', {}, n.blocked ? `${n.name} (blocked)` : n.name),
      n.start ? svg('circle', { cx: n.x, cy: n.y, r: n.r, class: 'pulse' }) : null,
      svg('circle', { cx: n.x, cy: n.y, r: n.r }))),
    L.labels.map(mapLabel))));
}

/* ----- Admin: hazards + map health (BFS) ----- */
function renderAdmin() {
  const el = $('admin');
  if (state.user?.role !== 'admin' || !state.graph) { el.replaceChildren(); return; }
  const nodes = state.graph.nodes;
  const label = (n) => `${n.name}${n.building ? ` (${n.building}, ${floorLabel(n.floor)})` : ''}`;
  const select = h('select', { required: true }, h('option', { value: '', disabled: true, selected: true }, 'Choose…'),
    nodes.map((n) => h('option', { value: n.id }, label(n))));
  const desc = h('input', { minlength: 3, maxlength: 200, required: true, placeholder: 'Smoke in stairwell' });
  const msg = h('p', { class: 'small', role: 'status' });

  const run = async (fn) => {
    msg.textContent = '';
    try { await fn(); await loadGraph(); } catch (err) { msg.textContent = err.message; msg.className = 'error'; }
  };
  const nameOf = (id) => label(nodes.find((n) => n.id === id) ?? { name: id });

  el.replaceChildren(h('details', { class: 'hazard-admin' },
    h('summary', {}, icon('shield'), 'Manage hazards (admin)'),
    h('form', { class: 'hazard-form', onsubmit: (e) => { e.preventDefault(); run(() => api('/api/emergency/hazards', { method: 'POST', form: { nodeId: select.value, description: desc.value } })); } },
      h('label', {}, 'Location', select), h('label', {}, 'What happened', desc),
      h('button', { type: 'submit', class: 'btn btn-danger' }, icon('alert'), 'Mark blocked')),
    state.graph.hazards.length
      ? h('ul', { class: 'hazard-list' }, state.graph.hazards.map((x) => h('li', {},
        `${x.nodeId ? nameOf(x.nodeId) : `${nameOf(x.edgeFrom)} to ${nameOf(x.edgeTo)}`}: ${x.description} `,
        h('button', { type: 'button', class: 'btn-link', onclick: () => run(() => api(`/api/emergency/hazards/${x.id}/resolve`, { method: 'PATCH' })) }, 'Clear'))))
      : h('p', { class: 'small muted' }, 'No active hazards.'),
    h('button', { type: 'button', class: 'btn btn-glass', onclick: async () => {
      try {
        const r = await api('/api/emergency/health');
        const stuck = Object.entries(r.unreachable).filter(([, ids]) => ids.length).map(([k, ids]) => `${k}: ${ids.join(', ')}`);
        msg.className = 'small';
        msg.textContent = `${r.nodes} points, ${r.edges} paths, ${r.connectedComponents} connected group${r.connectedComponents === 1 ? '' : 's'} (BFS). ` +
          (stuck.length ? `Cannot reach safety: ${stuck.join('; ')}` : 'Every point can reach safety in every mode.');
      } catch (err) { msg.textContent = err.message; msg.className = 'error'; }
    } }, icon('graph'), 'Check map health (BFS)'),
    msg));
}

/* ---------------- Start ---------------- */
const cached = readCache();
if (cached) Object.assign(state, { graph: cached.graph, savedAt: cached.savedAt, source: 'cache' });
renderAll();
renderResult();
initHeader().then((user) => { state.user = user; renderAdmin(); });
loadGraph();
setInterval(loadGraph, POLL_MS);
window.addEventListener('online', loadGraph);
window.addEventListener('resize', debounce(() => { if (state.route?.found) renderResult(); }, 200));
document.fonts?.ready.then(() => { if (state.route?.found) renderResult(); });
