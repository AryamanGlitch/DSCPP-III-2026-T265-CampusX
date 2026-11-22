import { api, avatar, h, icon, initHeader, timeAgo } from './common.js';

const CATEGORY_LABELS = {
  general: 'General', announcement: 'Announcement', academic: 'Academic', event: 'Event', lost_found: 'Lost & found',
};
const FILTERS = [['', 'All'], ...Object.entries(CATEGORY_LABELS)];

const postsEl = document.getElementById('posts');
const statusEl = document.getElementById('feed-status');
const loadMoreBtn = document.getElementById('load-more');
const filtersEl = document.getElementById('filters');
const composer = document.getElementById('composer');
const textarea = document.getElementById('composer-text');
const categorySel = document.getElementById('composer-category');
const countEl = document.getElementById('composer-count');
const composerError = document.getElementById('composer-error');
const postBtn = composer.querySelector('button[type=submit]');

let user = null;
let category = '';
let cursor = null;
let requestId = 0;  // ignore responses from a filter the user already left

function setStatus(text, isError = false) {
  statusEl.textContent = text;
  statusEl.className = isError ? 'status-line error' : 'status-line';
}

/* ---------- One post ---------- */
function postCard(post) {
  let liked = post.likedByMe;
  let likeCount = post.likeCount;
  let commentCount = post.commentCount;
  let busy = false;
  let commentsLoaded = false;

  const likeBtn = h('button', { type: 'button', class: 'action-btn like-btn', title: 'Like' });
  const commentBtn = h('button', { type: 'button', class: 'action-btn', title: 'Comments', 'aria-expanded': 'false' });
  const errorEl = h('p', { class: 'error', role: 'alert' });
  const commentList = h('div');
  const commentInput = h('input', { maxlength: 500, placeholder: 'Write a comment', 'aria-label': 'Write a comment' });
  const commentsBox = h('div', { class: 'comments', hidden: true }, commentList,
    h('form', { class: 'comment-form', onsubmit: addComment }, commentInput,
      h('button', { type: 'submit', class: 'icon-btn icon-btn-accent', 'aria-label': 'Send comment', title: 'Send' }, icon('send'))));

  function paint() {
    likeBtn.replaceChildren(icon('heart'), h('span', {}, String(likeCount)), h('span', { class: 'sr-only' }, liked ? ' likes, liked by you' : ' likes'));
    likeBtn.classList.toggle('is-on', liked);
    likeBtn.setAttribute('aria-pressed', String(liked));
    commentBtn.replaceChildren(icon('comment'), h('span', {}, String(commentCount)), h('span', { class: 'sr-only' }, ' comments'));
  }

  likeBtn.addEventListener('click', async () => {
    if (busy) return;
    busy = true;
    const before = { liked, likeCount };
    liked = !liked;                       // optimistic: feels instant...
    likeCount += liked ? 1 : -1;
    paint();
    if (liked) {                          // little "pop" so the tap feels confirmed
      likeBtn.classList.remove('pop');
      void likeBtn.offsetWidth;
      likeBtn.classList.add('pop');
    }
    try {
      const r = await api(`/api/feed/${post.id}/like`, { method: 'POST' });
      liked = r.liked;
      likeCount = r.likeCount;            // ...then trust the server's numbers
    } catch (err) {
      ({ liked, likeCount } = before);    // ...or roll back
      errorEl.textContent = err.message;
    } finally {
      busy = false;
      paint();
    }
  });

  commentBtn.addEventListener('click', async () => {
    const open = commentsBox.hidden;
    commentsBox.hidden = !open;
    commentBtn.setAttribute('aria-expanded', String(open));
    if (open && !commentsLoaded) {
      commentList.replaceChildren(h('p', { class: 'small muted' }, 'Loading comments…'));
      try {
        const { comments } = await api(`/api/feed/${post.id}/comments`);
        commentsLoaded = true;
        commentList.replaceChildren(...(comments.length ? comments.map(commentLine)
          : [h('p', { class: 'small muted' }, 'No comments yet. Start the conversation.')]));
      } catch (err) {
        errorEl.textContent = err.message;
      }
    }
  });

  async function addComment(e) {
    e.preventDefault();
    const content = commentInput.value.trim();
    if (!content) return;
    try {
      const c = await api(`/api/feed/${post.id}/comments`, { method: 'POST', form: { content } });
      if (commentCount === 0) commentList.replaceChildren();
      commentList.append(commentLine(c));
      commentCount += 1;
      commentInput.value = '';
      paint();
    } catch (err) {
      errorEl.textContent = err.message;
    }
  }

  async function remove() {
    if (!confirm('Delete this post and its comments?')) return;
    try {
      await api(`/api/feed/${post.id}`, { method: 'DELETE' });
      card.remove();
    } catch (err) {
      errorEl.textContent = err.message;
    }
  }

  const card = h('article', { class: `post panel ${post.isOfficial ? 'post-official' : ''}` },
    h('header', { class: 'post-head' },
      avatar(post.author.name),
      h('div', { class: 'post-who' },
        h('strong', {}, post.author.name),
        h('span', { class: 'post-meta' },
          h('time', { datetime: new Date(post.createdAt).toISOString() }, timeAgo(post.createdAt)),
          post.author.role ? ` as ${post.author.role}` : '')),
      h('span', { class: 'post-tags' },
        post.isOfficial ? h('span', { class: 'tag tag-official' }, icon('shield'), 'Official') : null,
        h('span', { class: `tag tag-${post.category}` }, CATEGORY_LABELS[post.category] || post.category))),
    h('p', { class: 'post-body' }, post.content),
    h('div', { class: 'post-actions' }, likeBtn, commentBtn,
      post.canDelete ? h('button', { type: 'button', class: 'action-btn action-danger', title: 'Delete post', 'aria-label': 'Delete post', onclick: remove }, icon('trash')) : null),
    errorEl,
    commentsBox);
  paint();
  return card;
}

function commentLine(c) {
  return h('div', { class: 'comment' }, avatar(c.author.name, 'avatar-xs'),
    h('p', {}, h('strong', {}, c.author.name), ' ', c.content, h('span', { class: 'small muted' }, ` ${timeAgo(c.createdAt)}`)));
}

/* ---------- List + paging ---------- */
async function load(reset) {
  const id = ++requestId;
  if (reset) { postsEl.replaceChildren(); cursor = null; }
  loadMoreBtn.hidden = true;
  setStatus('Loading posts…');
  const params = new URLSearchParams({ limit: 10 });
  if (cursor) params.set('cursor', cursor);
  if (category) params.set('category', category);
  try {
    const data = await api(`/api/feed?${params}`);
    if (id !== requestId) return;
    postsEl.append(...data.posts.map(postCard));
    cursor = data.nextCursor;
    loadMoreBtn.hidden = !cursor;
    setStatus(postsEl.children.length ? '' : 'No posts here yet. Write the first one above.');
  } catch (err) {
    if (id !== requestId) return;
    setStatus(err.message, true);
    loadMoreBtn.hidden = !cursor;
  }
}

function renderFilters() {
  filtersEl.replaceChildren(...FILTERS.map(([value, label]) =>
    h('button', {
      type: 'button', role: 'radio', 'aria-checked': String(category === value),
      class: `chip ${category === value ? 'is-on' : ''}`,
      onclick: () => { category = value; renderFilters(); load(true); },
    }, label)));
}

/* ---------- Composer ---------- */
function setupComposer() {
  const canAnnounce = ['faculty', 'admin'].includes(user.role);
  document.getElementById('composer-avatar').replaceChildren(avatar(user.name));
  categorySel.replaceChildren(...Object.entries(CATEGORY_LABELS)
    .filter(([v]) => v !== 'announcement' || canAnnounce)
    .map(([v, l]) => h('option', { value: v }, l)));

  textarea.addEventListener('input', () => {
    countEl.textContent = `${textarea.value.length}/2000`;
    postBtn.disabled = !textarea.value.trim();
  });

  composer.addEventListener('submit', async (e) => {
    e.preventDefault();
    composerError.textContent = '';
    postBtn.disabled = true;
    postBtn.replaceChildren(icon('send'), 'Posting…');
    try {
      const post = await api('/api/feed', { method: 'POST', form: { content: textarea.value, category: categorySel.value } });
      if (!category || category === post.category) postsEl.prepend(postCard(post));
      textarea.value = '';
      countEl.textContent = '0/2000';
      setStatus('');
      loadTrending();
    } catch (err) {
      composerError.textContent = err.message;
    } finally {
      postBtn.replaceChildren(icon('send'), 'Post');
      postBtn.disabled = !textarea.value.trim();
    }
  });
}

/* ---------- Trending (computed in C++ with a top-K heap) ---------- */
async function loadTrending() {
  const box = document.getElementById('trending');
  try {
    const t = await api('/api/feed/trending?k=5');
    box.replaceChildren(
      t.hashtags.length
        ? h('div', { class: 'tag-cloud' }, t.hashtags.map((x) => h('span', { class: 'hashtag', title: `${x.count} post${x.count === 1 ? '' : 's'}` }, icon('hash'), x.tag, h('span', { class: 'hashtag-count' }, x.count))))
        : h('p', { class: 'small muted' }, 'No hashtags yet.'),
      h('h3', { class: 'trending-sub' }, 'Hot posts'),
      h('ol', { class: 'hot-list' }, t.posts.map((p) =>
        h('li', {}, h('p', { class: 'hot-text' }, p.snippet),
          h('p', { class: 'small muted hot-meta' }, icon('heart'), ` ${p.likeCount}  `, icon('comment'), ` ${p.commentCount}  ${p.author}`)))));
  } catch {
    box.replaceChildren(h('p', { class: 'small muted' }, 'Trending is unavailable right now.'));
  }
}

/* ---------- Start ---------- */
user = await initHeader();
if (!user) {
  location.href = `/login.html?next=${encodeURIComponent('/feed.html')}`;
} else {
  setupComposer();
  renderFilters();
  loadMoreBtn.addEventListener('click', () => load(false));
  load(true);
  loadTrending();
}
