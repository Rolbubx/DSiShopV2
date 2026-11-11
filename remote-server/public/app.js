// DSi Shop Remote — interface web (téléphone)
'use strict';

const $ = (sel) => document.querySelector(sel);
const state = { user: null, devices: [], current: null, pollTimer: null, config: null };

// ---------------------------------------------------------------------------
// Utilitaires
// ---------------------------------------------------------------------------

async function api(method, path, body) {
  const res = await fetch('/api/' + path, {
    method,
    headers: { 'Content-Type': 'application/json', 'X-DSiShop': '1' },
    body: body ? JSON.stringify(body) : undefined,
    credentials: 'same-origin',
  });
  let data = {};
  try { data = await res.json(); } catch (_) { /* vide */ }
  if (!res.ok) throw Object.assign(new Error(data.error || 'Erreur ' + res.status), { status: res.status });
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

function ago(ts) {
  const s = Math.round((Date.now() - ts) / 1000);
  if (s < 60) return 'il y a ' + s + ' s';
  if (s < 3600) return 'il y a ' + Math.round(s / 60) + ' min';
  return new Date(ts).toLocaleString('fr-FR');
}

function show(id, visible) { $(id).hidden = !visible; }

// ---------------------------------------------------------------------------
// Connexion
// ---------------------------------------------------------------------------

function pairCodeFromUrl() {
  const m = /^\/p\/([A-Za-z0-9]{4,16})$/.exec(location.pathname);
  return m ? m[1].toUpperCase() : null;
}

async function onLoggedIn(user) {
  state.user = user;
  show('#login', false);
  show('#user', true);
  $('#user-pic').src = user.picture || '/icon.svg';
  $('#user-pic').title = user.name + (user.email ? ' — ' + user.email : '');

  const code = pairCodeFromUrl();
  if (code) {
    show('#pairing', true);
    $('#pair-msg').textContent = 'Code ' + code + '…';
    try {
      const { device } = await api('POST', 'pair', { code });
      $('#pair-msg').textContent = 'C’est fait ! « ' + device.name + ' » est liée à ton compte.';
      state.current = device.id;
      history.replaceState(null, '', '/');
      setTimeout(() => show('#pairing', false), 4000);
    } catch (e) {
      $('#pair-msg').textContent = e.message;
      $('#pair-msg').className = 'error';
    }
  }
  await refresh();
  startPolling();
}

function setupGoogle(clientId) {
  const s = document.createElement('script');
  s.src = 'https://accounts.google.com/gsi/client';
  s.async = true;
  s.onload = () => {
    google.accounts.id.initialize({
      client_id: clientId,
      callback: async ({ credential }) => {
        try {
          const { user } = await api('POST', 'auth/google', { credential });
          onLoggedIn(user);
        } catch (e) {
          $('#login-error').textContent = e.message;
          show('#login-error', true);
        }
      },
      ux_mode: 'popup',
    });
    google.accounts.id.renderButton($('#google-btn'), {
      theme: 'filled_blue', size: 'large', shape: 'pill', text: 'continue_with', locale: 'fr',
    });
  };
  document.head.append(s);
}

function showLogin() {
  show('#login', true);
  show('#dashboard', false);
  show('#no-device', false);
  show('#user', false);
  if (pairCodeFromUrl()) {
    $('#login-text').textContent =
      'Pour associer ta DSi, connecte-toi avec ton compte Google. Tu pourras ensuite la piloter depuis ce téléphone.';
  }
  if (state.config.googleClientId) setupGoogle(state.config.googleClientId);
  else if (!state.config.devLogin) {
    $('#login-error').textContent = 'Le serveur n’a pas d’identifiant Google configuré (GOOGLE_CLIENT_ID).';
    show('#login-error', true);
  }
  show('#dev-login', state.config.devLogin);
}

$('#dev-login').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const f = new FormData(ev.target);
  try {
    const { user } = await api('POST', 'auth/dev', { name: f.get('name'), email: f.get('email') });
    onLoggedIn(user);
  } catch (e) { toast(e.message); }
});

$('#logout').addEventListener('click', async () => {
  await api('POST', 'auth/logout').catch(() => {});
  location.href = '/';
});

// ---------------------------------------------------------------------------
// Tableau de bord
// ---------------------------------------------------------------------------

function currentDevice() {
  return state.devices.find((d) => d.id === state.current) || null;
}

async function refresh() {
  let data;
  try {
    data = await api('GET', 'me');
  } catch (e) {
    if (e.status === 401) { stopPolling(); showLogin(); }
    return;
  }
  state.devices = data.devices;
  if (!currentDevice()) state.current = state.devices[0]?.id || null;
  render();
}

function startPolling() {
  stopPolling();
  state.pollTimer = setInterval(refresh, 2000);
}
function stopPolling() {
  clearInterval(state.pollTimer);
  state.pollTimer = null;
}

const STATE_LABELS = {
  idle: 'Prête', resolving: 'Recherche de la release…', downloading: 'Téléchargement…',
  extracting: 'Extraction…', verifying: 'Vérification…', done: 'Installation terminée',
  error: 'Erreur',
};

function render() {
  const d = currentDevice();
  show('#no-device', !d);
  show('#dashboard', !!d);
  if (!d) return;

  // Sélecteur de DSi
  const sel = $('#device-select');
  sel.replaceChildren(...state.devices.map((x) =>
    el('option', { value: x.id, selected: x.id === d.id ? '' : null }, x.name)));

  const online = $('#online');
  online.textContent = d.online ? '● En ligne' : '○ Hors ligne (' + ago(d.lastSeen) + ')';
  online.className = 'pill ' + (d.online ? 'on' : 'off');

  const st = d.status || {};
  const label = STATE_LABELS[st.state] || st.state || 'En attente de la DSi…';
  $('#status-line').textContent = st.message && st.state !== 'idle' ? label + ' — ' + st.message : label;
  const p = st.progress || {};
  const busy = ['resolving', 'downloading', 'extracting', 'verifying'].includes(st.state);
  show('#progress', busy);
  $('#progress div').style.width = p.total ? Math.min(100, (100 * p.now) / p.total) + '%' : '0';
  $('#progress-text').textContent = busy
    ? [st.file, p.total ? formatSize(p.now) + ' / ' + formatSize(p.total) : formatSize(p.now)]
      .filter(Boolean).join(' · ')
    : (st.free ? 'Espace libre sur la SD : ' + formatSize(st.free) : '');
  $('#btn-cancel').disabled = !busy || !d.online;
  $('#btn-reboot').disabled = !d.online;

  // Jeux
  const games = st.games || [];
  $('#games-count').textContent = games.length ? '(' + games.length + ')' : '';
  $('#games').replaceChildren(...(games.length ? games.map((g) => el('li', {},
    el('div', { class: 'grow' },
      el('div', { class: 'title' }, g.file),
      el('div', { class: 'sub' }, [formatSize(g.size), g.version, g.source].filter(Boolean).join(' · '))),
    el('button', {
      class: 'danger',
      onclick: () => {
        if (confirm('Désinstaller « ' + g.file + ' » de la DSi ?')) sendCommand('uninstall', g.file);
      },
    }, 'Désinstaller'))) : [el('li', { class: 'muted' }, d.status ? 'Aucun jeu installé.' : 'La DSi n’a pas encore envoyé sa liste.')]));

  // Historique
  const labels = { queued: 'en attente', sent: 'envoyée', ok: 'OK', error: 'erreur' };
  const names = { install: 'Installer', uninstall: 'Désinstaller', cancel: 'Annuler', reboot: 'Redémarrer', refresh: 'Actualiser' };
  $('#history').replaceChildren(...(d.history.length ? d.history.map((h) => el('li', {},
    el('div', { class: 'grow' }, (names[h.type] || h.type) + (h.arg ? ' ' + h.arg : ''),
      h.result ? el('div', { class: 'sub' }, h.result) : null),
    el('span', { class: 'state-' + h.state }, labels[h.state] || h.state))) : [el('li', { class: 'muted' }, 'Rien pour l’instant.')]));
}

async function sendCommand(type, arg) {
  const d = currentDevice();
  if (!d) return;
  try {
    await api('POST', 'devices/' + d.id + '/commands', { type, arg });
    toast(d.online ? 'Envoyé à la DSi' : 'Envoyé : la DSi le recevra en se connectant');
    refresh();
  } catch (e) { toast(e.message); }
}

$('#device-select').addEventListener('change', (ev) => { state.current = ev.target.value; render(); });
$('#btn-cancel').addEventListener('click', () => sendCommand('cancel'));
$('#btn-reboot').addEventListener('click', () => {
  if (confirm('Redémarrer la DSi ?')) sendCommand('reboot');
});
$('#btn-unlink').addEventListener('click', async () => {
  const d = currentDevice();
  if (!d || !confirm('Délier « ' + d.name + ' » de ton compte ?')) return;
  await api('DELETE', 'devices/' + d.id).catch((e) => toast(e.message));
  state.current = null;
  refresh();
});
$('#install-form').addEventListener('submit', (ev) => {
  ev.preventDefault();
  const link = $('#install-link').value.trim();
  if (link) sendCommand('install', link);
});

// ---------------------------------------------------------------------------
// GitHub : recherche de projets et de releases (API publique, sans compte)
// ---------------------------------------------------------------------------

const ROM_RE = /\.(nds|dsi|zip)$/i;

async function github(path) {
  const res = await fetch('https://api.github.com/' + path, {
    headers: { Accept: 'application/vnd.github+json' },
  });
  if (res.status === 403 || res.status === 429) throw new Error('GitHub limite les recherches : réessaie dans une minute');
  if (res.status === 404) throw new Error('Aucune release publiée');
  if (!res.ok) throw new Error('GitHub : erreur ' + res.status);
  return res.json();
}

$('#gh-form').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const q = $('#gh-query').value.trim();
  const list = $('#gh-results');
  list.className = 'list gh';
  list.replaceChildren(el('li', { class: 'muted' }, 'Recherche…'));
  try {
    const data = await github('search/repositories?per_page=15&q=' + encodeURIComponent(q));
    if (!data.items.length) {
      list.replaceChildren(el('li', { class: 'muted' }, 'Aucun résultat.'));
      return;
    }
    list.replaceChildren(...data.items.map(repoItem));
  } catch (e) {
    list.replaceChildren(el('li', { class: 'error' }, e.message));
  }
});

function repoItem(repo) {
  const assets = el('div', { class: 'assets', hidden: '' });
  const li = el('li', {},
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
          if (!files.length) {
            assets.replaceChildren(el('div', { class: 'muted small' },
              'La release ' + rel.tag_name + ' ne contient pas de .nds, .dsi ou .zip.'));
            return;
          }
          assets.replaceChildren(el('div', { class: 'muted small' }, 'Release ' + rel.tag_name + ' :'),
            ...files.map((a) => el('div', { class: 'asset' },
              el('div', { class: 'grow' }, a.name + ' · ' + formatSize(a.size)),
              el('button', { class: 'primary', onclick: () => sendCommand('install', a.browser_download_url) },
                'Installer sur la DSi'))));
        } catch (e) {
          assets.replaceChildren(el('div', { class: 'error small' }, e.message));
        } finally {
          ev.target.disabled = false;
        }
      },
    }, 'Releases'),
    assets);
  return li;
}

// ---------------------------------------------------------------------------

(async function init() {
  state.config = await api('GET', 'config');
  try {
    const { user } = await api('GET', 'me');
    onLoggedIn(user);
  } catch (_) {
    showLogin();
  }
})();
