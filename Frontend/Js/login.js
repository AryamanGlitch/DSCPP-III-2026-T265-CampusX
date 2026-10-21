import { api, initHeader, token } from './common.js';

const form = document.getElementById('auth-form');
const title = document.getElementById('auth-title');
const nameField = document.getElementById('name-field');
const submit = document.getElementById('auth-submit');
const errorEl = document.getElementById('auth-error');
const switchText = document.getElementById('switch-text');
const switchBtn = document.getElementById('switch-mode');
const password = form.elements.password;

// Only allow redirects to pages on this site.
const nextParam = new URLSearchParams(location.search).get('next') || '/';
const next = nextParam.startsWith('/') && !nextParam.startsWith('//') ? nextParam : '/';
let mode = 'login';

function setMode(m) {
  mode = m;
  const reg = m === 'register';
  title.textContent = reg ? 'Create your account' : 'Log in to CampusX';
  nameField.hidden = !reg;
  submit.textContent = reg ? 'Create account' : 'Log in';
  switchText.textContent = reg ? 'Already have an account?' : 'New here?';
  switchBtn.textContent = reg ? 'Log in' : 'Create an account';
  password.autocomplete = reg ? 'new-password' : 'current-password';
  errorEl.textContent = '';
}

switchBtn.addEventListener('click', () => setMode(mode === 'login' ? 'register' : 'login'));

form.addEventListener('submit', async (e) => {
  e.preventDefault();
  errorEl.textContent = '';
  submit.disabled = true;
  submit.textContent = 'Please wait…';
  const data = Object.fromEntries(new FormData(form));
  try {
    const endpoint = mode === 'login' ? '/api/auth/login' : '/api/auth/register';
    if (mode === 'login') delete data.name;
    const res = await api(endpoint, { method: 'POST', form: data, auth: false });
    token.set(res.token);
    location.href = next;
  } catch (err) {
    errorEl.textContent = err.message;
    setMode(mode);  // restore button text
    errorEl.textContent = err.message;
  } finally {
    submit.disabled = false;
  }
});

initHeader().then((user) => { if (user) location.href = next; });
