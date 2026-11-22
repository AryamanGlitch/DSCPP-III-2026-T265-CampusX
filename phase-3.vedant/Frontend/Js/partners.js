import { api, avatar, h, icon, initHeader, timeAgo } from './common.js';

// Project partner matching. All ranking happens in C++ (dsa/Matcher.cpp);
// this page only collects input and explains the results.

const view = document.getElementById('view');
const statusEl = document.getElementById('partners-status');
const badge = document.getElementById('invite-badge');
const dialog = document.getElementById('student-dialog');

let user = null;
let meta = null;
let renderId = 0;  // ignore slow responses for a tab the user already left

const STATUS_LABELS = {
  recruiting: 'Recruiting', team_complete: 'Team complete', in_progress: 'In progress', completed: 'Completed', cancelled: 'Cancelled',
  pending: 'Pending', accepted: 'Accepted', rejected: 'Declined', withdrawn: 'Withdrawn',
};
const IMPORTANCE = [[1, 'Nice to have'], [2, 'Useful'], [3, 'Important'], [4, 'Very important'], [5, 'Critical']];

function setStatus(text, isError = false) {
  statusEl.textContent = text;
  statusEl.className = isError ? 'status-line error' : 'status-line';
}

const levelName = (n) => meta.levels.find((l) => l.value === n)?.label || '';
const domainLabel = (key) => meta.domains.find((d) => d.key === key)?.label || key;
const slotLabel = (i) => `${meta.days[Math.floor(i / 3)]} ${meta.periods[i % 3]}`;
const pct = (n) => `${Math.round(n)}%`;

function statusTag(status) {
  return h('span', { class: `tag tag-status tag-${status}` }, STATUS_LABELS[status] || status);
}

function emptyCard(iconName, title, text, action) {
  return h('div', { class: 'empty-card panel' }, icon(iconName, { cls: 'icon-lg' }), h('h2', {}, title), h('p', { class: 'muted' }, text), action || null);
}

function skillChips(skills, highlight = new Set()) {
  return h('div', { class: 'skill-chips' }, skills.map((s) => {
    const name = typeof s === 'string' ? s : s.name;
    const on = highlight.has(name.toLowerCase());
    return h('span', { class: `skill-chip ${on ? 'is-match' : ''}`, title: s.level ? levelName(s.level) : null },
      on ? icon('check') : null, name, s.level ? h('span', { class: 'skill-level' }, ['', 'Beg', 'Int', 'Adv'][s.level]) : null);
  }));
}

/* ---------- Score display ---------- */
function scoreRing(total, label = 'match') {
  return h('div', { class: 'score-ring', style: `--p:${Math.max(0, Math.min(100, total))}`, role: 'img', 'aria-label': `${pct(total)} ${label}` },
    h('span', { class: 'score-num' }, pct(total)), h('span', { class: 'score-label' }, label));
}

function breakdown(score) {
  const rows = [['Skills', score.skill], ['Free time', score.availability], ['Experience', score.experience], ['Interest', score.interest]];
  return h('dl', { class: 'breakdown' }, rows.map(([name, value]) => h('div', { class: 'breakdown-row' },
    h('dt', {}, name),
    h('dd', {}, h('span', { class: 'bar' }, h('span', { class: 'bar-fill', style: `width:${value}%` })), h('span', { class: 'bar-num' }, pct(value))))));
}

/* ---------- Form widgets ---------- */
function slotGrid(selected = []) {
  const chosen = new Set(selected);
  const boxes = [];
  const grid = h('div', { class: 'slot-grid', role: 'group', 'aria-label': 'Weekly availability' },
    h('span'), meta.days.map((d) => h('span', { class: 'slot-day' }, d)),
    meta.periods.map((period, p) => [
      h('span', { class: 'slot-period' }, period),
      meta.days.map((day, d) => {
        const i = d * 3 + p;
        const box = h('input', { type: 'checkbox', value: i, checked: chosen.has(i), 'aria-label': `${day} ${period}` });
        boxes.push(box);
        return h('label', { class: 'slot' }, box, h('span', { 'aria-hidden': 'true' }));
      }),
    ]));
  return { el: grid, value: () => boxes.filter((b) => b.checked).map((b) => b.value).join(',') };
}

let catalogEl = null;
function skillCatalog() {
  if (!catalogEl) {
    catalogEl = h('datalist', { id: 'skill-catalog' }, meta.skills.map((s) => h('option', { value: s })));
    document.body.append(catalogEl);
  }
  return 'skill-catalog';
}

function levelSelect(value = 2, label = 'Level') {
  return h('select', { 'aria-label': label }, meta.levels.map((l) => h('option', { value: l.value, selected: l.value === value }, l.label)));
}

/** Rows of skill name + level (+ importance and mandatory for projects). */
function skillEditor(initial, { project = false } = {}) {
  const rows = [];
  const list = h('div', { class: 'skill-rows' });
  const add = (s = {}) => {
    const name = h('input', { value: s.name || '', list: skillCatalog(), maxlength: 40, placeholder: 'e.g. Python', 'aria-label': 'Skill name' });
    const level = levelSelect(s.level || (project ? 1 : 2), project ? 'Level needed' : 'Your level');
    const importance = project ? h('select', { 'aria-label': 'Importance' }, IMPORTANCE.map(([v, l]) => h('option', { value: v, selected: v === (s.importance || 3) }, l))) : null;
    const mandatory = project ? h('input', { type: 'checkbox', checked: !!s.mandatory, 'aria-label': 'Must have' }) : null;
    const row = { name, level, importance, mandatory };
    const el = h('div', { class: `skill-row ${project ? 'skill-row-project' : ''}` }, name, level, importance,
      project ? h('label', { class: 'check', title: 'Every recommended student must have this skill' }, mandatory, 'Must have') : null,
      h('button', { type: 'button', class: 'icon-btn', 'aria-label': 'Remove skill', title: 'Remove', onclick: () => { rows.splice(rows.indexOf(row), 1); el.remove(); } }, icon('x')));
    rows.push(row);
    list.append(el);
  };
  (initial.length ? initial : [{}]).forEach(add);
  const el = h('div', { class: 'skill-editor' }, list,
    h('button', { type: 'button', class: 'btn btn-glass btn-sm', onclick: () => add() }, icon('plus'), 'Add skill'));
  const value = () => rows.filter((r) => r.name.value.trim()).map((r) => {
    const name = r.name.value.trim().replace(/[:,]/g, ' ');
    return project ? `${name}:${r.level.value}:${r.importance.value}:${r.mandatory.checked ? 1 : 0}` : `${name}:${r.level.value}`;
  }).join(',');
  return { el, value };
}

function field(labelText, control, hint) {
  return h('label', { class: 'field' }, h('span', {}, labelText), control, hint ? h('span', { class: 'small muted hint' }, hint) : null);
}

/* ---------- Student profile dialog ---------- */
async function showStudent(id) {
  const body = document.getElementById('student-dialog-body');
  body.replaceChildren(h('p', { class: 'muted' }, 'Loading…'));
  dialog.showModal();
  try {
    const { profile: p } = await api(`/api/partners/students/${id}`);
    body.replaceChildren(
      h('div', { class: 'cand-head' }, avatar(p.name), h('div', {}, h('strong', {}, p.name),
        h('p', { class: 'small muted' }, `${p.department} · Semester ${p.semester} · ${p.experienceLabel}`))),
      p.bio ? h('p', { class: 'student-bio' }, p.bio) : null,
      h('h3', { class: 'mini-title' }, 'Skills'), skillChips(p.skills),
      h('h3', { class: 'mini-title' }, 'Interests'),
      h('p', {}, p.interests.length ? p.interests.map(domainLabel).join(', ') : 'None listed'),
      h('h3', { class: 'mini-title' }, 'Free time'),
      h('p', {}, p.availability.length ? p.availability.map(slotLabel).join(', ') : 'Not listed'),
      h('p', { class: 'small muted' }, `In ${p.activeProjects} active project${p.activeProjects === 1 ? '' : 's'} (limit ${meta.maxActiveProjects}).`));
  } catch (err) {
    body.replaceChildren(h('p', { class: 'error' }, err.message));
  }
}
dialog.addEventListener('click', (e) => { if (e.target === dialog || e.target.closest('[data-close]')) dialog.close(); });

/* ---------- Invite button (shared by ranked list and suggested team) ---------- */
function inviteButton(projectId, c, onInvited) {
  const inv = c.invitation;
  if (c.declined) return h('span', { class: 'tag tag-rejected' }, 'Declined your invite');
  if (inv && inv.status === 'pending') {
    return h('span', { class: 'tag tag-pending' }, inv.kind === 'request' ? 'Asked to join: see Invitations' : 'Invited');
  }
  const btn = h('button', { type: 'button', class: 'btn btn-primary btn-sm' }, icon('send'), 'Invite');
  btn.addEventListener('click', async () => {
    btn.disabled = true;
    btn.replaceChildren(icon('send'), 'Sending…');
    try {
      const r = await api(`/api/partners/projects/${projectId}/invite`, { method: 'POST', form: { studentId: c.student.id, message: '' } });
      c.invitation = { id: r.id, kind: 'invite', status: 'pending' };
      btn.replaceWith(inviteButton(projectId, c));
      onInvited?.();
    } catch (err) {
      btn.disabled = false;
      btn.replaceChildren(icon('send'), 'Invite');
      setStatus(err.message, true);
    }
  });
  return btn;
}

function candidateCard(projectId, c, rank, extra) {
  const matched = new Set(c.matchedSkills.map((s) => s.toLowerCase()));
  const s = c.student;
  return h('article', { class: 'cand panel' },
    h('div', { class: 'cand-top' },
      rank ? h('span', { class: 'cand-rank', 'aria-label': `Rank ${rank}` }, `#${rank}`) : null,
      avatar(s.name),
      h('div', { class: 'cand-who' }, h('strong', {}, s.name),
        h('span', { class: 'small muted' }, `${s.department} · Sem ${s.semester} · ${s.experienceLabel}`)),
      scoreRing(c.score.total)),
    extra || null,
    h('p', { class: 'cand-reason' }, c.reason),
    breakdown(c.score),
    skillChips(s.skills, matched),
    c.missingSkills.length ? h('p', { class: 'small muted' }, `Doesn't list: ${c.missingSkills.join(', ')}`) : null,
    h('div', { class: 'cand-actions' },
      h('button', { type: 'button', class: 'btn btn-glass btn-sm', onclick: () => showStudent(s.id) }, icon('user'), 'View profile'),
      inviteButton(projectId, c, refreshBadge)));
}

/* ---------- Tab: Find teammates ---------- */
async function renderFind(id, selectedId) {
  const { projects } = await api('/api/partners/projects?scope=mine');
  if (id !== renderId) return;
  const mine = projects.filter((p) => p.isCreator && p.status === 'recruiting');
  if (!mine.length) {
    view.replaceChildren(emptyCard('team', 'No project is recruiting yet',
      'Create a project with the skills you need. CampusX ranks students who fit and suggests a balanced team.',
      h('a', { class: 'btn btn-primary', href: '#create' }, icon('plus'), 'New project')));
    return;
  }
  const project = mine.find((p) => p.id === selectedId) || mine[0];
  const picker = mine.length > 1
    ? field('Project', h('select', { onchange: (e) => { location.hash = `#find/${e.target.value}`; } },
      mine.map((p) => h('option', { value: p.id, selected: p.id === project.id }, p.title))))
    : null;

  view.replaceChildren(picker || '', h('p', { class: 'status-line' }, h('span', { class: 'spinner' }), 'Ranking students…'));
  const m = await api(`/api/partners/projects/${project.id}/matches?limit=20`);
  if (id !== renderId) return;
  const p = m.project;

  const summary = h('section', { class: 'panel project-summary' },
    h('div', { class: 'panel-head' }, h('h2', {}, icon('flag'), p.title), statusTag(p.status)),
    h('p', { class: 'muted' }, `${p.domainLabel} · ${p.durationWeeks} weeks · ${p.experienceLabel} level · team of ${p.teamSize} (${p.memberCount} in, ${m.spotsLeft} to find)`),
    h('h3', { class: 'mini-title' }, 'Required skills'),
    h('div', { class: 'skill-chips' }, p.skills.map((s) => h('span', { class: `skill-chip ${s.covered ? 'is-covered' : 'is-missing'}`, title: s.covered ? 'Already in your team' : 'Not in your team yet' },
      s.covered ? icon('check') : null, s.name, s.mandatory ? h('span', { class: 'skill-level' }, 'must') : null))),
    h('p', { class: 'small muted' }, 'Green = someone in your team already has it.'));

  const warnings = m.warnings.map((w) => h('p', { class: 'advice' }, icon('alert'), w));

  const ex = m.excluded;
  const reasons = [[ex.missingMandatory, 'missing a must-have skill'], [ex.noSharedTime, 'no shared time slot'],
    [ex.tooManyProjects, `already in ${meta.maxActiveProjects} projects`], [ex.noSkillOverlap, 'no required skill']]
    .filter(([n]) => n > 0).map(([n, t]) => `${n} ${t}`);
  const funnel = h('p', { class: 'small muted funnel' }, icon('layers'),
    `${m.totalProfiles} student profiles → ${m.considered} share a required skill → ${m.eligible} eligible` + (reasons.length ? ` (left out: ${reasons.join(', ')})` : ''));

  let team = null;
  if (m.suggestedTeam.length) {
    const inviteAll = h('button', { type: 'button', class: 'btn btn-primary btn-sm' }, icon('send'), 'Invite all suggested');
    const toInvite = () => m.suggestedTeam.filter((c) => !c.declined && !(c.invitation && c.invitation.status === 'pending'));
    inviteAll.hidden = !toInvite().length;
    inviteAll.addEventListener('click', async () => {
      inviteAll.disabled = true;
      for (const c of toInvite()) {
        try {
          await api(`/api/partners/projects/${project.id}/invite`, { method: 'POST', form: { studentId: c.student.id } });
        } catch (err) { setStatus(err.message, true); }
      }
      refreshBadge();
      route();
    });
    const after = m.coverage.missing.length
      ? h('p', { class: 'advice' }, icon('alert'), `Still missing after this team: ${m.coverage.missing.join(', ')}. Nobody eligible lists it.`)
      : h('p', { class: 'nav-note' }, icon('check'), 'With this team, every required skill is covered.');
    team = h('section', { class: 'team-suggest panel' },
      h('div', { class: 'panel-head' }, h('h2', {}, icon('sparkle'), `Suggested balanced team (${m.suggestedTeam.length} of ${m.spotsLeft})`), inviteAll),
      h('p', { class: 'small muted' }, 'Picked greedily: each round takes the student with the best mix of match score and missing skills they add. The top scorers often know the same things, so this team can differ from the ranking below.'),
      h('div', { class: 'team-grid' }, m.suggestedTeam.map((c) => candidateCard(project.id, c, null,
        h('p', { class: 'covers' }, c.covers.length ? [icon('plus'), `Adds ${c.covers.join(', ')}`] : 'Adds no new skill, highest remaining match')))),
      after);
  }

  const ranked = m.candidates.length
    ? h('div', { class: 'cand-list' }, m.candidates.map((c, i) => candidateCard(project.id, c, i + 1)))
    : emptyCard('search', 'No matching students yet', 'Nobody with a partner profile fits the must-have rules. Loosen them, or share the project so classmates create profiles.');

  view.replaceChildren(picker || '', summary, ...warnings, team || '', h('div', { class: 'section-head' }, h('h2', {}, 'All recommendations, best first'), funnel), ranked);
}

/* ---------- Tab: My projects (team details) ---------- */
function memberLine(m) {
  return h('li', { class: 'member' }, avatar(m.name, 'avatar-sm'),
    h('div', {}, h('strong', {}, m.name), m.role === 'creator' ? h('span', { class: 'small muted' }, ' · creator') : null,
      m.skills.length ? skillChips(m.skills) : h('p', { class: 'small muted' }, 'No partner profile')),
    m.hasProfile ? h('button', { type: 'button', class: 'btn-link', onclick: () => showStudent(m.id) }, 'Profile') : null);
}

function projectActions(p) {
  const act = async (status, question) => {
    if (question && !confirm(question)) return;
    try {
      await api(`/api/partners/projects/${p.id}/status`, { method: 'PATCH', form: { status } });
      refreshBadge();
      route();
    } catch (err) { setStatus(err.message, true); }
  };
  const btns = [];
  if (p.isCreator) {
    if (p.status === 'recruiting') btns.push(h('a', { class: 'btn btn-primary btn-sm', href: `#find/${p.id}` }, icon('search'), 'Find teammates'));
    if (p.status === 'recruiting' || p.status === 'team_complete') btns.push(h('button', { type: 'button', class: 'btn btn-glass btn-sm', onclick: () => act('in_progress', 'Start the project? Pending invitations will be withdrawn.') }, 'Start project'));
    if (p.status === 'team_complete' && p.spotsLeft > 0) btns.push(h('button', { type: 'button', class: 'btn btn-glass btn-sm', onclick: () => act('recruiting') }, 'Reopen recruiting'));
    if (p.status === 'in_progress') btns.push(h('button', { type: 'button', class: 'btn btn-glass btn-sm', onclick: () => act('completed') }, icon('check'), 'Mark completed'));
    if (!['completed', 'cancelled'].includes(p.status)) btns.push(h('button', { type: 'button', class: 'btn-link action-danger', onclick: () => act('cancelled', 'Cancel this project for everyone?') }, 'Cancel project'));
  } else if (!['completed', 'cancelled'].includes(p.status)) {
    btns.push(h('button', { type: 'button', class: 'btn btn-glass btn-sm', onclick: async () => {
      if (!confirm(`Leave “${p.title}”?`)) return;
      try { await api(`/api/partners/projects/${p.id}/leave`, { method: 'POST' }); route(); } catch (err) { setStatus(err.message, true); }
    } }, 'Leave team'));
  }
  return h('div', { class: 'cand-actions' }, btns);
}

async function renderProjects(id) {
  const { projects } = await api('/api/partners/projects?scope=mine');
  if (id !== renderId) return;
  if (!projects.length) {
    view.replaceChildren(emptyCard('team', 'You are not in any project yet', 'Create one, or browse open projects and ask to join.',
      h('div', { class: 'cand-actions' }, h('a', { class: 'btn btn-primary', href: '#create' }, icon('plus'), 'New project'), h('a', { class: 'btn btn-glass', href: '#browse' }, 'Browse projects'))));
    return;
  }
  view.replaceChildren(h('div', { class: 'project-list' }, projects.map((p) => h('article', { class: 'panel project-card' },
    h('div', { class: 'panel-head' }, h('h2', {}, p.title), statusTag(p.status)),
    h('p', { class: 'small muted' }, `${p.isCreator ? 'You created this' : `Created by ${p.creator.name}`} · ${p.domainLabel} · ${p.durationWeeks} weeks · ${p.memberCount}/${p.teamSize} members`),
    h('p', {}, p.description),
    h('h3', { class: 'mini-title' }, 'Skill coverage'),
    h('div', { class: 'skill-chips' }, p.skills.map((s) => h('span', { class: `skill-chip ${s.covered ? 'is-covered' : 'is-missing'}` }, s.covered ? icon('check') : null, s.name))),
    h('h3', { class: 'mini-title' }, 'Team'),
    h('ul', { class: 'members' }, p.members.map(memberLine)),
    projectActions(p)))));
}

/* ---------- Tab: Browse open projects ---------- */
async function renderBrowse(id) {
  const [{ projects, hasProfile }, mine] = await Promise.all([api('/api/partners/projects?scope=open'), api('/api/partners/profile')]);
  if (id !== renderId) return;
  const mySkills = new Set((mine.profile?.skills || []).map((s) => s.name.toLowerCase()));
  const intro = hasProfile ? h('p', { class: 'small muted' }, 'Sorted by how well each project fits your profile (same scoring as recommendations, run the other way).')
    : h('p', { class: 'advice' }, icon('user'), h('span', {}, 'Create ', h('a', { href: '#profile' }, 'your partner profile'), ' to see your fit and ask to join.'));
  if (!projects.length) {
    view.replaceChildren(intro, emptyCard('flag', 'No open projects right now', 'Check back later, or start your own.'));
    return;
  }
  view.replaceChildren(intro, h('div', { class: 'project-list' }, projects.map((p) => {
    const inv = p.myInvitation;
    let action;
    if (inv && inv.status === 'pending') {
      action = h('span', { class: 'tag tag-pending' }, inv.kind === 'invite' ? 'You are invited: see Invitations' : 'Request sent');
    } else if (inv && inv.status === 'rejected' && inv.kind === 'request') {
      action = h('span', { class: 'tag tag-rejected' }, 'Request declined');
    } else if (hasProfile) {
      const msg = h('input', { maxlength: 300, placeholder: 'Short message (optional)', 'aria-label': 'Message to the creator' });
      const btn = h('button', { type: 'submit', class: 'btn btn-primary btn-sm' }, icon('send'), 'Ask to join');
      action = h('form', { class: 'comment-form', onsubmit: async (e) => {
        e.preventDefault();
        btn.disabled = true;
        try {
          await api(`/api/partners/projects/${p.id}/request`, { method: 'POST', form: { message: msg.value } });
          action.replaceWith(h('span', { class: 'tag tag-pending' }, 'Request sent'));
        } catch (err) { btn.disabled = false; setStatus(err.message, true); }
      } }, msg, btn);
    }
    return h('article', { class: 'panel project-card' },
      h('div', { class: 'cand-top' },
        h('div', { class: 'cand-who' }, h('h2', {}, p.title),
          h('span', { class: 'small muted' }, `by ${p.creator.name} · ${p.domainLabel} · ${p.durationWeeks} weeks · ${p.spotsLeft} spot${p.spotsLeft === 1 ? '' : 's'} left`)),
        p.myFit != null ? scoreRing(p.myFit, 'your fit') : null),
      h('p', {}, p.description),
      skillChips(p.skills.map((s) => s.name), mySkills),
      p.availability.length ? h('p', { class: 'small muted' }, icon('clock'), ` ${p.availability.map(slotLabel).join(', ')}${p.scheduleRequired ? ' (required)' : ''}`) : null,
      action || null);
  })));
}

/* ---------- Tab: Invitations ---------- */
function invitationRow(inv, received) {
  const project = h('strong', {}, inv.project.title);
  let text;
  if (inv.kind === 'invite') text = received ? [inv.creator.name, ' invited you to join ', project] : ['You invited ', inv.student.name, ' to ', project];
  else text = received ? [inv.student.name, ' asked to join ', project] : ['You asked to join ', project, ` (by ${inv.creator.name})`];
  const other = inv.kind === 'invite' ? (received ? null : inv.student) : (received ? inv.student : null);

  const answer = (action, question) => async () => {
    if (question && !confirm(question)) return;
    try {
      await api(`/api/partners/invitations/${inv.id}/respond`, { method: 'POST', form: { action } });
      refreshBadge();
      route();
    } catch (err) { setStatus(err.message, true); }
  };
  return h('li', { class: 'panel invite' },
    avatar(received ? (inv.kind === 'invite' ? inv.creator.name : inv.student.name) : inv.project.title, 'avatar-sm'),
    h('div', { class: 'invite-body' },
      h('p', {}, ...text),
      inv.message ? h('p', { class: 'invite-msg' }, `“${inv.message}”`) : null,
      h('p', { class: 'small muted' }, timeAgo(inv.createdAt), ' · ', statusTag(inv.status))),
    h('div', { class: 'cand-actions' },
      other ? h('button', { type: 'button', class: 'btn-link', onclick: () => showStudent(other.id) }, 'View profile') : null,
      inv.canRespond ? h('button', { type: 'button', class: 'btn btn-primary btn-sm', onclick: answer('accept') }, icon('check'), 'Accept') : null,
      inv.canRespond ? h('button', { type: 'button', class: 'btn btn-glass btn-sm', onclick: answer('reject', 'Decline? They will not be able to send this again.') }, 'Decline') : null,
      inv.canWithdraw ? h('button', { type: 'button', class: 'btn btn-glass btn-sm', onclick: answer('withdraw') }, 'Withdraw') : null));
}

async function renderInvites(id) {
  const data = await api('/api/partners/invitations');
  if (id !== renderId) return;
  updateBadge(data.pendingReceived);
  const list = (items, received, emptyText) => items.length
    ? h('ul', { class: 'invite-list' }, items.map((i) => invitationRow(i, received)))
    : h('p', { class: 'small muted' }, emptyText);
  view.replaceChildren(
    h('h2', { class: 'section-title' }, icon('inbox'), 'Received'),
    list(data.received, true, 'Nothing waiting for you. Invitations and join requests for your projects show up here.'),
    h('h2', { class: 'section-title' }, icon('send'), 'Sent'),
    list(data.sent, false, 'You have not invited anyone or asked to join a project yet.'));
}

/* ---------- Tab: My profile ---------- */
async function renderProfile(id) {
  const { profile, canHaveProfile } = await api('/api/partners/profile');
  if (id !== renderId) return;
  if (!canHaveProfile) {
    view.replaceChildren(emptyCard('user', 'Partner profiles are for students',
      'You can still create projects (for example research work) and invite students who fit.'));
    return;
  }
  const p = profile || { department: '', semester: 1, experience: 1, interests: [], availability: [], skills: [], bio: '' };
  const dept = h('input', { value: p.department, maxlength: 60, required: true, placeholder: 'e.g. CSE' });
  const sem = h('select', {}, Array.from({ length: 10 }, (_, i) => h('option', { value: i + 1, selected: i + 1 === p.semester }, `Semester ${i + 1}`)));
  const exp = levelSelect(p.experience, 'Experience');
  const interests = meta.domains.map((d) => h('input', { type: 'checkbox', value: d.key, checked: p.interests.includes(d.key) }));
  const skills = skillEditor(p.skills);
  const slots = slotGrid(p.availability);
  const bio = h('textarea', { rows: 3, maxlength: 500, placeholder: 'Projects you have built, what you want to learn…' }, p.bio);
  const error = h('p', { class: 'error', role: 'alert' });
  const save = h('button', { type: 'submit', class: 'btn btn-primary' }, icon('check'), profile ? 'Save profile' : 'Create profile');

  const form = h('form', { class: 'panel partner-form' },
    h('p', { class: 'small muted' }, profile
      ? 'Project creators see this profile as last saved. Only students with a profile are recommended.'
      : 'Only students with a profile are recommended to project creators. Your email is never shown.'),
    h('div', { class: 'form-grid' }, field('Department', dept), field('Semester', sem),
      field('Overall experience', exp, 'Beginner: course work only. Intermediate: built a project. Advanced: several or led one.')),
    h('fieldset', { class: 'field' }, h('legend', {}, 'Skills and your level'), skills.el),
    h('fieldset', { class: 'field' }, h('legend', {}, 'Interests'),
      h('div', { class: 'check-chips' }, meta.domains.map((d, i) => h('label', { class: 'check-chip' }, interests[i], h('span', {}, d.label))))),
    h('fieldset', { class: 'field' }, h('legend', {}, 'When you are usually free'), slots.el),
    field('About you (optional)', bio),
    error,
    h('div', { class: 'cand-actions' }, save,
      profile ? h('button', { type: 'button', class: 'btn-link action-danger', onclick: async () => {
        if (!confirm('Delete your partner profile? You will stop appearing in recommendations, and pending invitations to you are withdrawn.')) return;
        try { await api('/api/partners/profile', { method: 'DELETE' }); route(); } catch (err) { error.textContent = err.message; }
      } }, 'Delete profile') : null));

  form.addEventListener('submit', async (e) => {
    e.preventDefault();
    error.textContent = '';
    save.disabled = true;
    try {
      await api('/api/partners/profile', { method: 'POST', form: {
        department: dept.value, semester: sem.value, experience: exp.value,
        interests: interests.filter((b) => b.checked).map((b) => b.value).join(','),
        availability: slots.value(), skills: skills.value(), bio: bio.value,
      } });
      meta = await api('/api/partners/meta');  // new skill names join the suggestions
      catalogEl?.remove();
      catalogEl = null;
      await route();  // re-render first: route() clears the status line
      setStatus('Profile saved. Recommendations use it straight away.');
    } catch (err) {
      error.textContent = err.message;
    } finally {
      save.disabled = false;
    }
  });
  view.replaceChildren(form);
}

/* ---------- Tab: Create project ---------- */
function renderCreate() {
  const title = h('input', { maxlength: 100, required: true, placeholder: 'e.g. AI-Based Crop Recommendation System' });
  const desc = h('textarea', { rows: 3, maxlength: 1000, required: true, placeholder: 'What you are building and why' });
  const domain = h('select', {}, meta.domains.map((d) => h('option', { value: d.key }, d.label)));
  const size = h('select', {}, [2, 3, 4, 5, 6, 7, 8].map((n) => h('option', { value: n, selected: n === 4 }, `${n} members`)));
  const weeks = h('input', { type: 'number', min: 1, max: 52, value: 8, required: true });
  const exp = levelSelect(2, 'Preferred experience');
  const skills = skillEditor([], { project: true });
  const slots = slotGrid([]);
  const required = h('input', { type: 'checkbox' });
  const extra = h('textarea', { rows: 2, maxlength: 500, placeholder: 'Anything else teammates should know (optional)' });
  const error = h('p', { class: 'error', role: 'alert' });
  const submit = h('button', { type: 'submit', class: 'btn btn-primary' }, icon('sparkle'), 'Create and find teammates');

  const form = h('form', { class: 'panel partner-form' },
    h('h2', {}, 'New project'),
    field('Project name', title), field('Description', desc),
    h('div', { class: 'form-grid' }, field('Domain', domain), field('Team size', size, 'Including you'),
      field('Duration (weeks)', weeks), field('Preferred experience', exp)),
    h('fieldset', { class: 'field' }, h('legend', {}, 'Required skills'),
      h('p', { class: 'small muted hint' }, 'Importance sets how much each skill counts in the match score. “Must have” filters out every student without it, so use it sparingly: teammates are meant to complement each other.'),
      skills.el),
    h('fieldset', { class: 'field' }, h('legend', {}, 'Preferred working times'), slots.el,
      h('label', { class: 'switch' }, required, h('span', { class: 'switch-track', 'aria-hidden': 'true' }),
        h('span', {}, 'Schedule is mandatory (hide students with no shared slot)'))),
    field('Additional requirements', extra),
    error, submit);

  form.addEventListener('submit', async (e) => {
    e.preventDefault();
    error.textContent = '';
    submit.disabled = true;
    try {
      const p = await api('/api/partners/projects', { method: 'POST', form: {
        title: title.value, description: desc.value, domain: domain.value, teamSize: size.value, durationWeeks: weeks.value,
        experience: exp.value, availability: slots.value(), scheduleRequired: required.checked ? 1 : 0,
        skills: skills.value(), extra: extra.value,
      } });
      location.hash = `#find/${p.id}`;
    } catch (err) {
      error.textContent = err.message;
      submit.disabled = false;
    }
  });
  view.replaceChildren(form);
}

/* ---------- Router ---------- */
function updateBadge(n) {
  badge.hidden = !n;
  badge.textContent = String(n || '');
}
async function refreshBadge() {
  try { updateBadge((await api('/api/partners/invitations')).pendingReceived); } catch { /* badge stays as it was */ }
}

async function route() {
  const [tab = 'find', arg] = location.hash.slice(1).split('/');
  const known = ['find', 'projects', 'browse', 'invites', 'profile', 'create'];
  const current = known.includes(tab) ? tab : 'find';
  document.querySelectorAll('.partner-tabs .chip').forEach((a) => {
    const on = a.dataset.tab === current;
    a.classList.toggle('is-on', on);
    if (on) a.setAttribute('aria-current', 'page'); else a.removeAttribute('aria-current');
  });
  const id = ++renderId;
  setStatus('');
  if (current !== 'find') view.replaceChildren(h('div', { class: 'skeleton skeleton-route' }));
  try {
    if (current === 'find') await renderFind(id, Number(arg) || null);
    else if (current === 'projects') await renderProjects(id);
    else if (current === 'browse') await renderBrowse(id);
    else if (current === 'invites') await renderInvites(id);
    else if (current === 'profile') await renderProfile(id);
    else renderCreate();
  } catch (err) {
    if (id === renderId) {
      view.replaceChildren();
      setStatus(err.message, true);
    }
  }
}

/* ---------- Start ---------- */
user = await initHeader();
if (!user) {
  location.href = `/login.html?next=${encodeURIComponent('/partners.html')}`;
} else {
  try {
    meta = await api('/api/partners/meta');
    window.addEventListener('hashchange', route);
    refreshBadge();
    route();
  } catch (err) {
    setStatus(err.message, true);
  }
}
