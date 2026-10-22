import { api, avatar, countUp, h, icon, initHeader, timeAgo } from './common.js';

const $ = (id) => document.getElementById(id);

function greeting(user) {
  const hr = new Date().getHours();
  const part = hr < 5 ? 'Up late' : hr < 12 ? 'Good morning' : hr < 17 ? 'Good afternoon' : 'Good evening';
  return user ? `${part}, ${user.name.split(' ')[0]}` : 'Welcome to CampusX';
}

/* ---------- Public stats: campus map + search index ---------- */
async function loadStats() {
  try {
    const g = await api('/api/emergency/graph', { auth: false });
    countUp($('stat-points'), g.nodes.length);
    $('stat-points-sub').textContent = `${g.buildings.length} building${g.buildings.length === 1 ? '' : 's'}, ${g.edges.length} paths`;
    countUp($('stat-hazards'), g.hazards.length);
    $('stat-hazards-sub').textContent = g.hazards.length ? 'Routes avoid these right now' : 'All routes clear';
    $('stat-hazard-card').classList.toggle('stat-alert', g.hazards.length > 0);
    countUp($('stat-contacts'), g.contacts.length);
    const allDay = g.contacts.filter((c) => c.available24x7).length;
    $('stat-contacts-sub').textContent = `${allDay} available 24×7`;
    $('hood-graph').textContent = `${g.nodes.length} / ${g.edges.length}`;
    countUp($('hood-tables'), Object.keys(g.tables || {}).length);
  } catch {
    $('stat-hazards-sub').textContent = 'Map unavailable';
  }

  try {
    const health = await api('/api/health', { auth: false });
    countUp($('stat-docs'), health.search.documents);
    $('stat-docs-sub').textContent = `${health.search.uniqueWords} unique words indexed`;
    countUp($('hood-trie'), health.search.trieNodes);
  } catch { /* leave the dashes */ }

  try {
    // A real query, timed by the server: shows the trie at work.
    const r = await api('/api/search?q=library&limit=5', { auth: false });
    $('hood-speed').textContent = r.tookMicros < 1000 ? `${r.tookMicros} µs` : `${(r.tookMicros / 1000).toFixed(1)} ms`;
  } catch { /* leave the dash */ }
}

/* ---------- Feed preview (needs login) ---------- */
function loginPrompt(text) {
  return h('div', { class: 'empty-card' }, icon('login', { cls: 'icon-lg' }), h('p', {}, text),
    h('a', { class: 'btn btn-primary btn-sm', href: '/login.html?next=/' }, 'Log in'));
}

async function loadFeed(user) {
  const postsEl = $('home-posts');
  const trendEl = $('home-trending');
  if (!user) {
    postsEl.replaceChildren(loginPrompt('Log in to see what campus is talking about.'));
    trendEl.replaceChildren(loginPrompt('Trending posts and #tags appear after you log in.'));
    return;
  }
  postsEl.replaceChildren(...[1, 2, 3].map(() => h('div', { class: 'skeleton skeleton-post' })));
  try {
    const { posts } = await api('/api/feed?limit=4');
    postsEl.replaceChildren(...(posts.length ? posts.map((p) =>
      h('a', { class: 'mini-post', href: '/feed.html' },
        avatar(p.author.name, 'avatar-sm'),
        h('span', { class: 'mini-post-body' },
          h('span', { class: 'mini-post-meta' }, h('strong', {}, p.author.name), ` ${timeAgo(p.createdAt)}`),
          h('span', { class: 'mini-post-text' }, p.content)),
        p.isOfficial ? h('span', { class: 'tag tag-official' }, 'Official') : null))
      : [h('p', { class: 'muted' }, 'No posts yet. Be the first to post.')]));
  } catch (err) {
    postsEl.replaceChildren(h('p', { class: 'error' }, err.message));
  }

  try {
    const t = await api('/api/feed/trending?k=5');
    trendEl.replaceChildren(
      t.hashtags.length
        ? h('div', { class: 'tag-cloud' }, t.hashtags.map((x) => h('span', { class: 'hashtag' }, icon('hash'), x.tag, h('span', { class: 'hashtag-count' }, x.count))))
        : h('p', { class: 'muted small' }, 'No hashtags yet.'),
      h('ol', { class: 'hot-list' }, t.posts.slice(0, 3).map((p) =>
        h('li', {}, h('p', { class: 'hot-text' }, p.snippet),
          h('p', { class: 'small muted hot-meta' }, icon('heart'), ` ${p.likeCount}  `, icon('comment'), ` ${p.commentCount}  ${p.author}`)))));
  } catch {
    trendEl.replaceChildren(h('p', { class: 'muted small' }, 'Trending is unavailable right now.'));
  }
}

/* ---------- Start ---------- */
$('home-search').addEventListener('submit', (e) => {
  e.preventDefault();
  const q = $('home-q').value.trim();
  location.href = q ? `/search.html?${new URLSearchParams({ q })}` : '/search.html';
});

const user = await initHeader();
$('greeting').textContent = greeting(user);
loadStats();
loadFeed(user);
