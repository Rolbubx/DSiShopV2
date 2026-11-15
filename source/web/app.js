// DSi Shop — Remote local (page servie par la DSi elle-même)
'use strict';

const $ = (s) => document.querySelector(s);
let timer = null;
let lastState = '';

async function api(method, path, body) {
  const res = await fetch('/api/' + path, {
    method,
    headers: { 'Content-Type': 'application/json', 'X-DSiShop': '1' },
    body: body ? JSON.stringify(body) : undefined,
    credentials: 'same-origin',
  });
  let data = {};
  try { data = await res.json(); } catch (_) { /* vide */ }
  if (!res.ok) throw Object.assign(new Error(data.error || data.message || 'Erreur ' + res.status), { status: res.status });
  return data;
}

let toastTimer = null;
function toast(msg) {
  const t = $('#toast');
  t.textContent = msg;
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, 3500);
}

function el(tag, attrs = {}, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'class') e.className = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (v !== undefined && v !== null) e.setAttribute(k, v);
  }
  for (const c of children) if (c != null) e.append(c);
  return e;
}

function formatSize(n) {
  if (!n) return '';
  const u = ['o', 'Ko', 'Mo', 'Go'];
  let i = 0;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return (i ? n.toFixed(1).replace('.', ',') : n) + ' ' + u[i];
}

const STATES = {
  idle: 'Prête', resolving: 'Recherche de la release…', downloading: 'Téléchargement…',
  extracting: 'Extraction…', verifying: 'Vérification…', done: 'Terminé', error: 'Erreur',
};

function showLogin() {
  clearInterval(timer);
  $('#login').hidden = false;
  $('#dashboard').hidden = true;
  $('#logout').hidden = true;
}

function showDashboard() {
  $('#login').hidden = true;
  $('#dashboard').hidden = false;
  $('#logout').hidden = false;
  clearInterval(timer);
  timer = setInterval(refresh, 2000);
}

async function refresh() {
  let s;
  try {
    s = await api('GET', 'status');
  } catch (e) {
    if (e.status === 401) showLogin();
    else {
      $('#online').textContent = '○ Injoignable';
      $('#online').className = 'pill off';
    }
    return;
  }
  $('#online').textContent = '● Connectée';
  $('#online').className = 'pill on';
  render(s);
}

function render(s) {
  const busy = ['resolving', 'downloading', 'extracting', 'verifying'].includes(s.state);
  const label = STATES[s.state] || s.state;
  $('#status-line').textContent = s.message && s.state !== 'idle' ? label + ' — ' + s.message : label;
  $('#progress').hidden = !busy;
  const p = s.progress || {};
  $('#progress div').style.width = p.total ? Math.min(100, (100 * p.now) / p.total) + '%' : '0';
  $('#progress-text').textContent = busy
    ? [s.file, p.total ? formatSize(p.now) + ' / ' + formatSize(p.total) : formatSize(p.now)].filter(Boolean).join(' · ')
    : (s.free ? 'Espace libre sur la SD : ' + formatSize(s.free) : '');
  $('#btn-cancel').disabled = !busy;

  if (lastState && lastState !== s.state && (s.state === 'done' || s.state === 'error')) {
    toast(s.message || label);
  }
  lastState = s.state;

  $('#games-count').textContent = s.games.length ? '(' + s.games.length + ')' : '';
  $('#games').replaceChildren(...(s.games.length ? s.games.map((g) => {
    const badge = g.update === 'available' ? el('span', { class: 'badge available' }, 'Mise à jour dispo')
      : g.update === 'uptodate' ? el('span', { class: 'badge uptodate' }, 'À jour') : null;
    return el('li', { class: 'game' },
      el('div', { class: 'grow' },
        el('div', { class: 'title' }, g.file, badge),
        el('div', { class: 'sub' }, [formatSize(g.size), g.version, g.source].filter(Boolean).join(' · ')),
        g.url ? el('div', { class: 'sub url' }, 'Téléchargé depuis : ' + g.url) : null),
      el('div', { class: 'btns' },
        (g.source || g.url) ? el('button', { class: 'secondary', onclick: () => command('update', g.file) },
          'Mettre à jour') : null,
        el('button', {
          class: 'danger',
          onclick: () => {
            if (confirm('Désinstaller « ' + g.file + ' » ? (les sauvegardes sont gardées)')) command('uninstall', g.file);
          },
        }, 'Désinstaller')));
  }) : [el('li', { class: 'muted' }, 'Aucun jeu installé.')]));
}

async function command(type, arg) {
  try {
    const r = await api('POST', 'command', { type, arg });
    toast(r.message || 'OK');
    refresh();
  } catch (e) {
    if (e.status === 401) showLogin();
    toast(e.message);
  }
}

$('#login-form').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const f = new FormData(ev.target);
  try {
    await api('POST', 'login', { user: f.get('user'), pass: f.get('pass') });
    $('#login-error').hidden = true;
    ev.target.reset();
    showDashboard();
    refresh();
  } catch (e) {
    $('#login-error').textContent = e.message;
    $('#login-error').hidden = false;
  }
});

$('#logout').addEventListener('click', async () => {
  await api('POST', 'logout').catch(() => {});
  showLogin();
});
$('#btn-cancel').addEventListener('click', () => command('cancel'));
$('#btn-check').addEventListener('click', () => command('check'));
$('#btn-reboot').addEventListener('click', () => { if (confirm('Redémarrer la DSi ?')) command('reboot'); });
$('#install-form').addEventListener('submit', (ev) => {
  ev.preventDefault();
  const link = $('#install-link').value.trim();
  if (link) command('install', link);
});

// --- GitHub (appelé par le téléphone, qui a Internet) ---------------------------

const ROM_RE = /\.(nds|dsi|zip)$/i;

async function github(path) {
  const res = await fetch('https://api.github.com/' + path, { headers: { Accept: 'application/vnd.github+json' } });
  if (res.status === 403 || res.status === 429) throw new Error('GitHub limite les recherches : réessaie dans une minute');
  if (res.status === 404) throw new Error('Aucune release publiée');
  if (!res.ok) throw new Error('GitHub : erreur ' + res.status);
  return res.json();
}

$('#gh-form').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const list = $('#gh-results');
  list.replaceChildren(el('li', { class: 'muted' }, 'Recherche…'));
  try {
    const data = await github('search/repositories?per_page=15&q=' + encodeURIComponent($('#gh-query').value.trim()));
    list.replaceChildren(...(data.items.length ? data.items.map(repoItem) : [el('li', { class: 'muted' }, 'Aucun résultat.')]));
  } catch (e) {
    list.replaceChildren(el('li', { class: 'error' }, e.message));
  }
});

function repoItem(repo) {
  const assets = el('div', { class: 'assets', hidden: '' });
  return el('li', {},
    el('div', { class: 'grow' },
      el('div', { class: 'title' }, repo.full_name),
      el('div', { class: 'sub' }, '★ ' + repo.stargazers_count + (repo.description ? ' · ' + repo.description : ''))),
    el('button', {
      class: 'secondary',
      onclick: async (ev) => {
        ev.target.disabled = true;
        assets.hidden = false;
        assets.replaceChildren(el('div', { class: 'muted small' }, 'Chargement de la release…'));
        try {
          const rel = await github('repos/' + repo.full_name + '/releases/latest');
          const files = rel.assets.filter((a) => ROM_RE.test(a.name));
          assets.replaceChildren(...(files.length
            ? [el('div', { class: 'muted small' }, 'Release ' + rel.tag_name + ' :'),
              ...files.map((a) => el('div', { class: 'asset' },
                el('div', { class: 'grow' }, a.name + ' · ' + formatSize(a.size)),
                el('button', { class: 'primary', onclick: () => command('install', a.browser_download_url) },
                  'Installer sur la DSi')))]
            : [el('div', { class: 'muted small' }, 'Pas de .nds, .dsi ou .zip dans la release ' + rel.tag_name + '.')]));
        } catch (e) {
          assets.replaceChildren(el('div', { class: 'error small' }, e.message));
        } finally {
          ev.target.disabled = false;
        }
      },
    }, 'Releases'),
    assets);
}

// Au chargement : déjà connecté ?
api('GET', 'status').then((s) => { showDashboard(); render(s); }).catch(showLogin);
