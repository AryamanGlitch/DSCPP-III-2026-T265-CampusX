// Map layout for the emergency page.
//
// 1. Projects graph coordinates (metres / GPS) to real screen pixels, so text
//    is always 13 px instead of growing with the drawing.
// 2. Places labels greedily: for each label (most important first) try 8
//    positions around its dot and keep the cheapest one. Leaving the frame or
//    overlapping another label is forbidden; covering a dot or crossing a
//    line costs extra. A label with no acceptable spot is skipped, and the
//    dot still shows its name on hover.
//
// Pure functions with no DOM access, so they can be tested in Node.

export const LABEL_FONT_PX = 13;
const LABEL_H = 18;  // line box for 13 px text plus its halo
const HALO = 6;      // the text outline (CSS stroke) makes labels a bit wider

let ctx = null;
/** Text width in pixels, measured with the same font the CSS uses. */
export function browserMeasure(text) {
  ctx ??= document.createElement('canvas').getContext('2d');
  ctx.font = `700 ${LABEL_FONT_PX}px "Atkinson Hyperlegible", "Segoe UI", system-ui, sans-serif`;
  return ctx.measureText(text).width;
}

/* Rectangles are [left, top, width, height]. */
const inflate = ([l, t, w, h], p) => [l - p, t - p, w + 2 * p, h + 2 * p];

function rectsOverlap(a, b) {
  return a[0] < b[0] + b[2] && b[0] < a[0] + a[2] && a[1] < b[1] + b[3] && b[1] < a[1] + a[3];
}

function circleHitsRect(cx, cy, r, [l, t, w, h]) {
  const nx = Math.max(l, Math.min(cx, l + w));  // closest point of the rect to the centre
  const ny = Math.max(t, Math.min(cy, t + h));
  return (cx - nx) ** 2 + (cy - ny) ** 2 < r * r;
}

/** Liang–Barsky line clipping: does segment (x1,y1)-(x2,y2) pass through the rect? */
function segmentHitsRect(x1, y1, x2, y2, [l, t, w, h]) {
  const dx = x2 - x1, dy = y2 - y1;
  const p = [-dx, dx, -dy, dy];
  const q = [x1 - l, l + w - x1, y1 - t, t + h - y1];
  let t0 = 0, t1 = 1;
  for (let i = 0; i < 4; i++) {
    if (p[i] === 0) {
      if (q[i] < 0) return false;  // parallel to this edge and outside it
    } else {
      const r = q[i] / p[i];
      if (p[i] < 0) { if (r > t1) return false; if (r > t0) t0 = r; }
      else { if (r < t0) return false; if (r < t1) t1 = r; }
    }
  }
  return true;
}

/**
 * items:    [{ id, text, x, y, r, width, priority, cls }]
 * nodes:    [{ id, x, y, r }]                      dots to keep visible
 * segments: [{ x1, y1, x2, y2, width, onRoute }]   lines to avoid
 * Returns   [{ id, text, cls, x, y }] with x/y = label centre.
 */
export function placeLabels(items, { width, height, nodes, segments, margin = 4 }) {
  const placed = [];
  const out = [];
  const order = [...items].sort((a, b) => b.priority - a.priority);

  for (const it of order) {
    const w = it.width + HALO, h = LABEL_H, g = it.r + 5, d = g * 0.75;
    const { x, y } = it;
    const candidates = [
      [x - w / 2, y - g - h],  // above
      [x - w / 2, y + g],      // below
      [x + g, y - h / 2],      // right
      [x - g - w, y - h / 2],  // left
      [x + d, y - d - h],      // above right
      [x - d - w, y - d - h],  // above left
      [x + d, y + d],          // below right
      [x - d - w, y + d],      // below left
    ];

    let best = null;
    candidates.forEach(([l, t], i) => {
      const box = [l, t, w, h];
      let cost = i * 0.5;  // slight preference for the order above
      if (l < margin || t < margin || l + w > width - margin || t + h > height - margin) cost += 1000;
      for (const p of placed) if (rectsOverlap(box, inflate(p, 2))) cost += 500;
      for (const n of nodes) if (n.id !== it.id && circleHitsRect(n.x, n.y, n.r + 2, box)) cost += 200;
      for (const s of segments) {
        if (segmentHitsRect(s.x1, s.y1, s.x2, s.y2, inflate(box, s.width / 2))) cost += s.onRoute ? 40 : 8;
      }
      if (!best || cost < best.cost) best = { box, cost };
    });

    // "You are here" may cover a dot if it must; ordinary labels may not.
    const limit = it.priority >= 100 ? 500 : 200;
    if (best.cost >= limit) continue;
    placed.push(best.box);
    out.push({ id: it.id, text: it.text, cls: it.cls, x: best.box[0] + best.box[2] / 2, y: best.box[1] + best.box[3] / 2 });
  }
  return out;
}

const labelCls = (start) => (start ? 'map-label map-label-here' : 'map-label');

/** One building floor. Node x/y are metres on the plan. */
export function layoutFloor({ meta, nodes, edges, routeKeys, routeIds, blocked, startId, targetId, maxWidth, measure }) {
  const pad = 30;
  // At least 9 px per metre: on a phone the plan scrolls sideways instead of
  // shrinking until rooms are too close together to label.
  const scale = Math.max(9, (maxWidth - 2 * pad) / meta.width);  // pixels per metre
  const width = Math.round(meta.width * scale + 2 * pad);
  const height = Math.round(meta.height * scale + 2 * pad);

  const ns = nodes.map((n) => {
    const start = n.id === startId;
    return {
      id: n.id, name: n.name, type: n.type, start, blocked: blocked.has(n.id),
      x: pad + n.x * scale, y: pad + n.y * scale,
      r: start ? 10 : n.type === 'corridor' ? 4 : 8,
    };
  });
  const byId = new Map(ns.map((n) => [n.id, n]));
  const es = edges.filter((e) => byId.has(e.from) && byId.has(e.to)).map((e) => {
    const a = byId.get(e.from), b = byId.get(e.to);
    const key = edgeKey(e.from, e.to);
    const onRoute = routeKeys.has(key);
    return { key, from: e.from, to: e.to, x1: a.x, y1: a.y, x2: b.x, y2: b.y, onRoute, width: onRoute ? 10 : 6 };
  });

  const items = ns.filter((n) => n.type !== 'corridor' || n.start).map((n) => {
    const text = n.start ? 'You are here' : n.name;
    const priority = n.start ? 100 : n.id === targetId ? 90 : routeIds.has(n.id) ? 50 : 10;
    return { id: n.id, text, x: n.x, y: n.y, r: n.r, width: measure(text), priority, cls: labelCls(n.start) };
  });

  // The building walls count as lines too, so labels don't sit on top of them.
  const outline = { x: pad, y: pad, w: meta.width * scale, h: meta.height * scale };
  const [l, t, r, b] = [outline.x, outline.y, outline.x + outline.w, outline.y + outline.h];
  const walls = [[l, t, r, t], [r, t, r, b], [r, b, l, b], [l, b, l, t]]
    .map(([x1, y1, x2, y2]) => ({ x1, y1, x2, y2, width: 2, onRoute: false }));

  return {
    width, height, outline, nodes: ns, edges: es,
    labels: placeLabels(items, { width, height, nodes: ns, segments: [...es, ...walls] }),
  };
}

/** Outdoor schematic from GPS points (north up). */
export function layoutOutdoor({ nodes, edges, routeKeys, routeIds, blocked, startId, leaveId, targetId, maxWidth, maxHeight = 460, measure }) {
  const outdoor = nodes.filter((n) => !n.building && n.lat != null);
  if (!outdoor.length) return null;

  const M = 111320;  // metres per degree of latitude
  const lat0 = outdoor.reduce((s, n) => s + n.lat, 0) / outdoor.length;
  const kx = M * Math.cos((lat0 * Math.PI) / 180);
  const minLng = Math.min(...outdoor.map((n) => n.lng));
  const maxLat = Math.max(...outdoor.map((n) => n.lat));
  const metres = outdoor.map((n) => ({ n, mx: (n.lng - minLng) * kx, my: (maxLat - n.lat) * M }));
  const spanX = Math.max(1, ...metres.map((p) => p.mx));
  const spanY = Math.max(1, ...metres.map((p) => p.my));

  const pad = 36;
  const drawWidth = Math.max(560, maxWidth);  // same idea: scroll on phones rather than squash
  const scale = Math.min((drawWidth - 2 * pad) / spanX, (maxHeight - 2 * pad) / spanY);
  const width = Math.round(spanX * scale + 2 * pad);
  const height = Math.round(spanY * scale + 2 * pad);

  const ns = metres.map(({ n, mx, my }) => {
    const start = n.id === startId || n.id === leaveId;
    const big = start || ['assembly', 'medical', 'entrance'].includes(n.type);
    return {
      id: n.id, name: n.name, type: n.type, start, blocked: blocked.has(n.id),
      x: pad + mx * scale, y: pad + my * scale, r: start ? 11 : big ? 9 : 5,
    };
  });
  const byId = new Map(ns.map((n) => [n.id, n]));
  const es = edges.filter((e) => byId.has(e.from) && byId.has(e.to)).map((e) => {
    const a = byId.get(e.from), b = byId.get(e.to);
    const key = edgeKey(e.from, e.to);
    const onRoute = routeKeys.has(key);
    return { key, from: e.from, to: e.to, x1: a.x, y1: a.y, x2: b.x, y2: b.y, onRoute, width: onRoute ? 8 : 4 };
  });

  // Plain path junctions stay unlabelled to keep the map readable.
  const items = ns.filter((n) => n.start || n.id === targetId || ['assembly', 'medical', 'entrance'].includes(n.type)).map((n) => {
    const text = n.id === startId ? 'You are here' : n.id === leaveId ? 'You come out here' : n.name;
    const priority = n.start ? 100 : n.id === targetId ? 90 : routeIds.has(n.id) ? 50 : 30;
    return { id: n.id, text, x: n.x, y: n.y, r: n.r, width: measure(text), priority, cls: labelCls(n.start) };
  });

  return { width, height, nodes: ns, edges: es, labels: placeLabels(items, { width, height, nodes: ns, segments: es }) };
}

export const edgeKey = (a, b) => (a < b ? `${a}|${b}` : `${b}|${a}`);
