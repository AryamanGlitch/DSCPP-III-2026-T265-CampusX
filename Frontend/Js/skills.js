import { api, h, icon, initHeader } from './common.js';

// Skill gap + learning recommendations. The gap and the ranking are computed in
// C++ (dsa/SkillGap.cpp); Gemini only suggests candidate video titles, and the
// API key never reaches the browser.

const goalsEl = document.getElementById('goals');
const view = document.getElementById('view');
const statusEl = document.getElementById('skills-status');
const GOAL_KEY = 'campusx:learning-goal';

let goals = [];
let goal = null;
let lastRun = null;  // summary of the latest Gemini run, shown above the cards
let lastAnalysis = null;
let busy = false;

const STATUS_TEXT = { sufficient: 'Sufficient', improve: 'Improve', missing: 'Missing' };
const WEIGHT_TEXT = { 3: 'High', 2: 'Medium', 1: 'Low' };

function setStatus(text, isError = false) {
  statusEl.textContent = text;
  statusEl.className = isError ? 'status-line error' : 'status-line';
}

const youtubeSearch = (title) => `https://www.youtube.com/results?search_query=${encodeURIComponent(title)}`;

function scoreRing(value, label) {
  return h('div', { class: 'score-ring', style: `--p:${Math.max(0, Math.min(100, value))}`, role: 'img', 'aria-label': `${Math.round(value)}% ${label}` },
    h('span', { class: 'score-num' }, `${Math.round(value)}%`), h('span', { class: 'score-label' }, label));
}

/* ---------- Goal picker ---------- */
function renderGoals() {
  goalsEl.replaceChildren(...goals.map((g) => h('button', {
    type: 'button', role: 'radio', 'aria-checked': String(goal === g.key),
    class: `goal-card panel ${goal === g.key ? 'is-on' : ''}`,
    onclick: () => { if (goal !== g.key) { goal = g.key; lastRun = null; remember(); renderGoals(); load(); } },
  }, h('strong', {}, g.title), h('span', { class: 'small muted' }, g.skills.join(' · ')))));
}

function remember() {
  try { localStorage.setItem(GOAL_KEY, goal); } catch { /* not remembered, still works */ }
}

/* ---------- Sections ---------- */
function mySkillsPanel(a) {
  const edit = h('a', { class: 'btn btn-glass btn-sm', href: '/partners.html#profile' }, icon('user'), a.hasSkills ? 'Edit my skills' : 'Add my skills');
  return h('section', { class: 'panel learn-panel' },
    h('div', { class: 'panel-head' }, h('h2', {}, icon('user'), 'My skills'), edit),
    a.hasSkills
      ? h('div', { class: 'skill-chips' }, a.mySkills.map((s) => h('span', { class: 'skill-chip' }, s.name, h('span', { class: 'skill-level' }, s.label))))
      : h('p', { class: 'muted' }, 'You have no skills in your profile yet. Add them so the gap can be calculated.'),
    h('p', { class: 'small muted' }, 'Same skills as your Find Project Partner profile. Levels are your own estimate, not a test result.'));
}

function gapPanel(a) {
  const rows = a.rows.map((r) => h('tr', {},
    h('th', { scope: 'row' }, r.skill),
    h('td', {}, r.currentLabel),
    h('td', {}, r.targetLabel),
    h('td', {}, h('span', { class: `gap-pill gap-${r.status}` }, STATUS_TEXT[r.status])),
    h('td', {}, WEIGHT_TEXT[r.weight]),
    h('td', { class: 'num' }, r.priority ? String(r.priority) : '–')));
  const summary = a.focus.length
    ? `Focus first: ${a.focus.slice(0, 3).map((f) => f.skill).join(', ')}.`
    : 'No skill gaps for this goal: you meet every target.';
  return h('section', { class: 'panel learn-panel' },
    h('div', { class: 'panel-head' }, h('h2', {}, icon('layers'), 'Skill gap'), scoreRing(a.gapPercent, 'gap')),
    h('p', {}, summary),
    h('div', { class: 'scroll-x' }, h('table', { class: 'gap-table' },
      h('thead', {}, h('tr', {}, ['Skill', 'You', 'Target', 'Status', 'Importance', 'Priority'].map((t) => h('th', { scope: 'col' }, t)))),
      h('tbody', {}, rows))),
    h('p', { class: 'small muted' }, 'Gap = target − your level. Priority = gap × importance (high 3, medium 2, low 1). The gap % weights each skill by importance.'));
}

function recCard(r, rank) {
  const setRecStatus = async (status) => {
    try {
      await api(`/api/learning/recommendations/${r.id}/status`, { method: 'POST', form: { status } });
      load();
    } catch (err) { setStatus(err.message, true); }
  };
  const actions = [h('a', { class: 'btn btn-primary btn-sm', href: youtubeSearch(r.title), target: '_blank', rel: 'noopener noreferrer' }, icon('play'), 'Search on YouTube')];
  if (r.status === 'new') actions.push(h('button', { type: 'button', class: 'btn btn-glass btn-sm', onclick: () => setRecStatus('saved') }, icon('bookmark'), 'Save for later'));
  if (r.status === 'saved') actions.push(h('button', { type: 'button', class: 'btn btn-glass btn-sm', onclick: () => setRecStatus('new') }, 'Unsave'));
  if (r.status !== 'completed') actions.push(h('button', { type: 'button', class: 'btn btn-glass btn-sm', onclick: () => setRecStatus('completed') }, icon('check'), 'Mark completed'));
  else actions.push(h('button', { type: 'button', class: 'btn-link', onclick: () => setRecStatus('saved') }, 'Undo'));

  return h('article', { class: `cand panel rec-card rec-${r.status}` },
    h('div', { class: 'cand-top' },
      rank ? h('span', { class: 'cand-rank', 'aria-label': `Rank ${rank}` }, `#${rank}`) : null,
      h('div', { class: 'cand-who' }, h('strong', {}, r.title),
        h('span', { class: 'small muted' }, [r.skill, r.level].filter(Boolean).join(' · '))),
      scoreRing(r.score, 'score')),
    r.description ? h('p', { class: 'cand-reason' }, r.description) : null,
    r.topics.length ? h('div', { class: 'skill-chips' }, r.topics.map((t) => h('span', { class: 'skill-chip' }, t))) : null,
    h('div', { class: 'cand-actions' },
      r.status !== 'new' ? h('span', { class: `tag tag-${r.status === 'completed' ? 'completed' : 'pending'}` }, r.status === 'completed' ? 'Completed' : 'Saved') : null,
      actions));
}

function runSummary(run) {
  if (!run) return null;
  const parts = [];
  if (run.source === 'gemini') {
    const f = run.filtered;
    const removed = [[f.unrelated, 'unrelated'], [f.duplicates, 'duplicate'], [f.tooAdvanced, 'too advanced'], [f.alreadySaved, 'already saved'], [f.empty, 'empty']]
      .filter(([n]) => n > 0).map(([n, t]) => `${n} ${t}`);
    parts.push(`Gemini (${run.model}) suggested ${run.received} titles`);
    if (removed.length) parts.push(`removed ${removed.join(', ')}`);
    parts.push(`C++ scored ${removed.length ? 'the rest' : 'them all'} and kept the top ${run.shown}`);
  }
  return h('div', {},
    run.warning ? h('p', { class: 'advice' }, icon('alert'), run.warning) : null,
    parts.length ? h('p', { class: 'small muted funnel' }, icon('sparkle'), `${parts.join(' → ')}.`) : null);
}

function recommendationsPanel(a) {
  const fresh = a.recommendations.filter((r) => r.status === 'new');
  const kept = a.recommendations.filter((r) => r.status !== 'new');
  const canRun = a.hasSkills && a.focus.length > 0;
  const button = h('button', { type: 'button', class: 'btn btn-primary', disabled: !canRun || busy }, icon('sparkle'),
    busy ? 'Asking Gemini…' : fresh.length ? 'Get new recommendations' : 'Get recommendations');
  button.addEventListener('click', recommend);

  let body;
  if (busy) body = h('p', { class: 'status-line' }, h('span', { class: 'spinner' }), 'Asking Gemini and ranking the answers. This usually takes 5–20 seconds.');
  else if (fresh.length) body = h('div', { class: 'cand-list' }, fresh.map((r, i) => recCard(r, i + 1)));
  else if (!a.hasSkills) body = h('p', { class: 'muted' }, 'Add your skills first.');
  else if (!a.focus.length) body = h('p', { class: 'muted' }, 'Nothing to learn for this goal. Pick another one.');
  else body = h('p', { class: 'muted' }, 'Press the button to get five ranked learning videos for your biggest gaps.');

  return h('section', { class: 'panel learn-panel' },
    h('div', { class: 'panel-head' }, h('h2', {}, icon('play'), 'Recommended learning'), button),
    a.geminiConfigured ? null : h('p', { class: 'advice' }, icon('alert'), 'Gemini is not set up on this server, so you will get standard search suggestions.'),
    busy ? null : runSummary(lastRun),
    body,
    h('p', { class: 'small muted' }, 'Titles are suggestions, not verified videos: “Search on YouTube” opens matching search results.'),
    kept.length ? [h('h3', { class: 'mini-title' }, 'Saved and completed'), h('div', { class: 'cand-list' }, kept.map((r) => recCard(r)))] : null);
}

function progressPanel(a) {
  const p = a.progress;
  const done = Object.entries(p.completedBySkill);
  const stat = (num, label) => h('div', { class: 'progress-stat' }, h('span', { class: 'progress-num' }, String(num)), h('span', { class: 'small muted' }, label));
  return h('section', { class: 'panel learn-panel' },
    h('div', { class: 'panel-head' }, h('h2', {}, icon('trending'), 'Progress')),
    h('div', { class: 'progress-grid' },
      stat(`${p.skillsAtTarget}/${p.skillsTotal}`, 'skills at target'),
      stat(p.remainingGaps, 'gaps left'),
      stat(p.completed, p.completed === 1 ? 'video completed' : 'videos completed'),
      stat(p.saved, 'saved for later')),
    done.length ? h('p', { class: 'small' }, 'Completed by skill: ', done.map(([s, n]) => `${s} (${n})`).join(', ')) : null,
    h('p', { class: 'small muted' }, 'Finishing a video doesn’t raise your level automatically. When you feel ready, update the level in your profile and the gap recalculates.'));
}

/* ---------- Load + actions ---------- */
async function load() {
  if (!goal) return;
  const requested = goal;
  try {
    const a = await api(`/api/learning/analysis?goal=${encodeURIComponent(goal)}`);
    if (requested !== goal) return;  // the user switched goals meanwhile
    render(a);
  } catch (err) {
    setStatus(err.message, true);
  }
}

function render(a) {
  lastAnalysis = a;
  view.replaceChildren(h('div', { class: 'learn-grid' },
    h('div', { class: 'learn-col' }, mySkillsPanel(a), gapPanel(a), progressPanel(a)),
    h('div', { class: 'learn-col' }, recommendationsPanel(a))));
}

async function recommend() {
  if (busy) return;
  busy = true;
  setStatus('');
  const requested = goal;
  if (lastAnalysis) render(lastAnalysis);  // repaint with the spinner
  try {
    const a = await api('/api/learning/recommendations', { method: 'POST', form: { goal: requested } });
    lastRun = a.run;
    busy = false;
    if (requested === goal) render(a);
  } catch (err) {
    busy = false;
    setStatus(err.message, true);
    load();
  }
}

/* ---------- Start ---------- */
const user = await initHeader();
if (!user) {
  location.href = `/login.html?next=${encodeURIComponent('/skills.html')}`;
} else {
  try {
    ({ goals } = await api('/api/learning/goals'));
    let saved = null;
    try { saved = localStorage.getItem(GOAL_KEY); } catch { /* ignore */ }
    goal = goals.some((g) => g.key === saved) ? saved : goals[0].key;
    renderGoals();
    load();
  } catch (err) {
    setStatus(err.message, true);
  }
}
