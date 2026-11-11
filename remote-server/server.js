// DSi Shop Remote — serveur relais entre la DSi et ton téléphone.
//
// - La DSi s'enregistre (POST /api/device/register) et affiche un QR code qui
//   pointe vers /p/<CODE> (code d'appairage à usage unique, 10 min).
// - Sur le téléphone, tu te connectes avec Google : la DSi est liée à ton compte.
// - Le site envoie des commandes (installer, désinstaller, annuler, redémarrer)
//   que la DSi récupère en "long polling" et dont elle renvoie le résultat.
//
// Aucune dépendance : Node.js >= 18 suffit. Configuration par variables
// d'environnement (voir README.md) :
//   GOOGLE_CLIENT_ID  ID client OAuth "Application Web" (obligatoire en prod)
//   PUBLIC_URL        URL publique du site, ex. https://dsishop.mondomaine.fr
//   PORT              port d'écoute (défaut 8080)
//   DATA_FILE         fichier de sauvegarde (défaut ./data.json)
//   DEV_LOGIN=1       connexion sans Google, UNIQUEMENT pour tester en local

'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');

const CONFIG = {
  googleClientId: process.env.GOOGLE_CLIENT_ID || '',
  publicUrl: (process.env.PUBLIC_URL || '').replace(/\/+$/, ''),
  port: Number(process.env.PORT) || 8080,
  dataFile: process.env.DATA_FILE || path.join(__dirname, 'data.json'),
  devLogin: process.env.DEV_LOGIN === '1',
};

const PAIR_CODE_TTL_MS = 10 * 60 * 1000;
const SESSION_TTL_MS = 30 * 24 * 3600 * 1000;
const DEVICE_ONLINE_MS = 45 * 1000;
const MAX_POLL_WAIT_S = 25;
const MAX_BODY = 64 * 1024;
const PAIR_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'; // sans 0/O/1/I

// ---------------------------------------------------------------------------
// Stockage (en mémoire + fichier JSON)
// ---------------------------------------------------------------------------

let db = { users: {}, devices: {}, sessions: {} };
try {
  db = { ...db, ...JSON.parse(fs.readFileSync(CONFIG.dataFile, 'utf8')) };
} catch (_) {
  // Premier lancement
}

// Données volatiles (non sauvegardées)
const pairCodes = new Map();    // code -> { deviceId, expires }
const pendingCmds = new Map();  // deviceId -> [commandes à envoyer]
const pollWaiters = new Map();  // deviceId -> callback qui réveille un long poll
const liveStatus = new Map();   // deviceId -> dernier statut envoyé par la DSi

let saveTimer = null;
function save() {
  if (saveTimer) return;
  saveTimer = setTimeout(() => {
    saveTimer = null;
    const tmp = CONFIG.dataFile + '.tmp';
    fs.writeFileSync(tmp, JSON.stringify(db));
    fs.renameSync(tmp, CONFIG.dataFile);
  }, 200);
}

const randomHex = (n) => crypto.randomBytes(n).toString('hex');
const sha256 = (s) => crypto.createHash('sha256').update(s).digest('hex');

function newPairCode(deviceId) {
  for (const [code, p] of pairCodes) {
    if (p.deviceId === deviceId || p.expires < Date.now()) pairCodes.delete(code);
  }
  let code = '';
  const bytes = crypto.randomBytes(8);
  for (const b of bytes) code += PAIR_ALPHABET[b % PAIR_ALPHABET.length];
  pairCodes.set(code, { deviceId, expires: Date.now() + PAIR_CODE_TTL_MS });
  return code;
}

// ---------------------------------------------------------------------------
// Limitation de débit très simple (par IP et par action)
// ---------------------------------------------------------------------------

const rate = new Map();
function rateLimited(key, max, windowMs) {
  const now = Date.now();
  const r = rate.get(key);
  if (!r || r.reset < now) {
    rate.set(key, { n: 1, reset: now + windowMs });
    return false;
  }
  r.n += 1;
  return r.n > max;
}
setInterval(() => {
  const now = Date.now();
  for (const [k, r] of rate) if (r.reset < now) rate.delete(k);
  for (const [c, p] of pairCodes) if (p.expires < now) pairCodes.delete(c);
  for (const [s, v] of Object.entries(db.sessions)) {
    if (v.expires < now) { delete db.sessions[s]; save(); }
  }
}, 60 * 1000).unref();

// ---------------------------------------------------------------------------
// Vérification des jetons Google (ID token JWT signé RS256)
// ---------------------------------------------------------------------------

let googleKeys = { keys: [], expires: 0 };

async function getGoogleKeys() {
  if (googleKeys.expires > Date.now()) return googleKeys.keys;
  const res = await fetch('https://www.googleapis.com/oauth2/v3/certs');
  if (!res.ok) throw new Error('certs Google indisponibles');
  const json = await res.json();
  const maxAge = /max-age=(\d+)/.exec(res.headers.get('cache-control') || '');
  googleKeys = {
    keys: json.keys,
    expires: Date.now() + (maxAge ? Number(maxAge[1]) * 1000 : 3600 * 1000),
  };
  return googleKeys.keys;
}

const b64url = (s) => Buffer.from(s, 'base64url');

async function verifyGoogleIdToken(token) {
  const parts = String(token).split('.');
  if (parts.length !== 3) throw new Error('jeton invalide');
  const header = JSON.parse(b64url(parts[0]).toString('utf8'));
  const payload = JSON.parse(b64url(parts[1]).toString('utf8'));
  if (header.alg !== 'RS256') throw new Error('algorithme inattendu');

  const jwk = (await getGoogleKeys()).find((k) => k.kid === header.kid);
  if (!jwk) throw new Error('clé inconnue');
  const key = crypto.createPublicKey({ key: jwk, format: 'jwk' });
  const ok = crypto.verify('RSA-SHA256', Buffer.from(parts[0] + '.' + parts[1]),
    key, b64url(parts[2]));
  if (!ok) throw new Error('signature invalide');

  const now = Math.floor(Date.now() / 1000);
  if (!['accounts.google.com', 'https://accounts.google.com'].includes(payload.iss)) {
    throw new Error('émetteur invalide');
  }
  if (payload.aud !== CONFIG.googleClientId) throw new Error('audience invalide');
  if (!(payload.exp > now - 60)) throw new Error('jeton expiré');
  if (!payload.sub) throw new Error('jeton sans identifiant');
  return {
    id: 'google:' + payload.sub,
    email: payload.email_verified ? payload.email || '' : '',
    name: payload.name || payload.given_name || payload.email || 'Utilisateur',
    picture: payload.picture || '',
  };
}

// ---------------------------------------------------------------------------
// Utilitaires HTTP
// ---------------------------------------------------------------------------

function send(res, status, body, type = 'application/json; charset=utf-8', extra = {}) {
  const data = typeof body === 'string' || Buffer.isBuffer(body) ? body : JSON.stringify(body);
  res.writeHead(status, {
    'Content-Type': type,
    'Cache-Control': 'no-store',
    'X-Content-Type-Options': 'nosniff',
    ...extra,
  });
  res.end(data);
}

const sendText = (res, status, text) => send(res, status, text, 'text/plain; charset=utf-8');

function readBody(req) {
  return new Promise((resolve, reject) => {
    let size = 0;
    const chunks = [];
    req.on('data', (c) => {
      size += c.length;
      if (size > MAX_BODY) {
        reject(new Error('trop gros'));
        req.destroy();
        return;
      }
      chunks.push(c);
    });
    req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
    req.on('error', reject);
  });
}

async function readJson(req) {
  const text = await readBody(req);
  try {
    return text ? JSON.parse(text) : {};
  } catch (_) {
    return {};
  }
}

function clientIp(req) {
  // Derrière un proxy (Render, Fly...), l'IP réelle est dans X-Forwarded-For
  const fwd = req.headers['x-forwarded-for'];
  return (fwd ? String(fwd).split(',')[0] : req.socket.remoteAddress || '').trim();
}

function parseCookies(req) {
  const out = {};
  for (const part of String(req.headers.cookie || '').split(';')) {
    const i = part.indexOf('=');
    if (i > 0) out[part.slice(0, i).trim()] = decodeURIComponent(part.slice(i + 1).trim());
  }
  return out;
}

function isHttps(req) {
  return CONFIG.publicUrl.startsWith('https://') ||
    req.headers['x-forwarded-proto'] === 'https';
}

function sessionUser(req) {
  const sid = parseCookies(req).dsishop_session;
  const s = sid && db.sessions[sha256(sid)];
  if (!s || s.expires < Date.now()) return null;
  return db.users[s.userId] || null;
}

function startSession(req, res, user) {
  db.users[user.id] = { ...(db.users[user.id] || {}), ...user };
  const sid = randomHex(32);
  db.sessions[sha256(sid)] = { userId: user.id, expires: Date.now() + SESSION_TTL_MS };
  save();
  const cookie = [
    `dsishop_session=${sid}`, 'Path=/', 'HttpOnly', 'SameSite=Lax',
    `Max-Age=${SESSION_TTL_MS / 1000}`,
  ];
  if (isHttps(req)) cookie.push('Secure');
  return cookie.join('; ');
}

function publicBase(req) {
  if (CONFIG.publicUrl) return CONFIG.publicUrl;
  return `${isHttps(req) ? 'https' : 'http'}://${req.headers.host}`;
}

// Champs envoyés par la DSi : pas de tabulation ni de retour à la ligne
const clean = (s, max = 200) => String(s ?? '').replace(/[\t\r\n]/g, ' ').slice(0, max);

// ---------------------------------------------------------------------------
// API de la DSi (texte "TSV" simple, authentifiée par jeton d'appareil)
// ---------------------------------------------------------------------------

function deviceFromToken(req) {
  const m = /^Bearer\s+([a-f0-9]{64})$/i.exec(String(req.headers.authorization || ''));
  if (!m) return null;
  const hash = sha256(m[1].toLowerCase());
  return Object.values(db.devices).find((d) => d.tokenHash === hash) || null;
}

function ownerLine(device) {
  const u = device.ownerId && db.users[device.ownerId];
  return u ? `owner\t${clean(u.name, 60)}\t${clean(u.email, 90)}` : 'owner\t-';
}

function pairLines(req, code) {
  return `pair\t${code}\t${publicBase(req)}/p/${code}\t${PAIR_CODE_TTL_MS / 1000}`;
}

async function deviceApi(req, res, route) {
  if (route === 'register' && req.method === 'POST') {
    if (rateLimited('register:' + clientIp(req), 20, 3600 * 1000)) {
      return sendText(res, 429, 'error\tTrop de tentatives, réessaie plus tard');
    }
    const body = await readBody(req);
    const name = clean(/^name\t(.*)$/m.exec(body)?.[1] || 'Nintendo DSi', 40);
    const id = randomHex(8);
    const token = randomHex(32);
    db.devices[id] = {
      id, name, tokenHash: sha256(token), ownerId: null,
      createdAt: Date.now(), lastSeen: Date.now(), history: [],
    };
    save();
    const code = newPairCode(id);
    return sendText(res, 200, `device\t${id}\t${token}\n${pairLines(req, code)}\n`);
  }

  const device = deviceFromToken(req);
  if (!device) return sendText(res, 401, 'error\tAppareil inconnu');
  device.lastSeen = Date.now();

  if (route === 'pair-code' && req.method === 'POST') {
    const code = newPairCode(device.id);
    return sendText(res, 200, `${ownerLine(device)}\n${pairLines(req, code)}\n`);
  }

  if (route === 'poll' && req.method === 'GET') {
    const url = new URL(req.url, 'http://x');
    const wait = Math.min(MAX_POLL_WAIT_S, Math.max(0, Number(url.searchParams.get('wait')) || 0));
    const reply = () => {
      const cmds = pendingCmds.get(device.id) || [];
      pendingCmds.delete(device.id);
      const lines = [ownerLine(device)];
      for (const c of cmds) {
        lines.push(['cmd', c.id, c.type, c.arg || ''].join('\t'));
        const h = device.history.find((x) => x.id === c.id);
        if (h) h.state = 'sent';
      }
      device.lastSeen = Date.now();
      sendText(res, 200, lines.join('\n') + '\n');
    };

    if ((pendingCmds.get(device.id) || []).length > 0 || wait === 0) return reply();

    // Long polling : on répond dès qu'une commande arrive, ou après `wait` s
    const prev = pollWaiters.get(device.id);
    if (prev) prev(); // Un seul poll en attente par appareil
    let done = false;
    const finish = () => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      if (pollWaiters.get(device.id) === finish) pollWaiters.delete(device.id);
      reply();
    };
    const timer = setTimeout(finish, wait * 1000);
    pollWaiters.set(device.id, finish);
    req.on('close', () => {
      if (!done) {
        done = true;
        clearTimeout(timer);
        if (pollWaiters.get(device.id) === finish) pollWaiters.delete(device.id);
      }
    });
    return undefined;
  }

  if (route === 'status' && req.method === 'POST') {
    const body = await readBody(req);
    const st = { updatedAt: Date.now(), games: [], state: 'idle', message: '' };
    for (const line of body.split('\n')) {
      const f = line.split('\t');
      switch (f[0]) {
        case 'state': st.state = clean(f[1], 20); break;
        case 'message': st.message = clean(f[1], 200); break;
        case 'progress': st.progress = { now: Number(f[1]) || 0, total: Number(f[2]) || 0 }; break;
        case 'file': st.file = clean(f[1], 128); break;
        case 'release': st.release = clean(f[1], 64); break;
        case 'dir': st.dir = clean(f[1], 64); break;
        case 'free': st.free = Number(f[1]) || 0; break;
        case 'version': st.appVersion = clean(f[1], 20); break;
        case 'game':
          if (st.games.length < 500) {
            st.games.push({
              file: clean(f[1], 128), size: Number(f[2]) || 0,
              source: clean(f[3], 300), version: clean(f[4], 64),
              url: clean(f[5], 512),
            });
          }
          break;
        case 'ack': {
          const h = device.history.find((x) => x.id === f[1]);
          if (h) {
            h.state = f[2] === 'ok' ? 'ok' : 'error';
            h.result = clean(f[3], 200);
            save();
          }
          break;
        }
        default: break;
      }
    }
    liveStatus.set(device.id, st);
    return sendText(res, 200, 'ok\n');
  }

  return sendText(res, 404, 'error\tRoute inconnue');
}

// ---------------------------------------------------------------------------
// API du site web (JSON, authentifiée par cookie de session)
// ---------------------------------------------------------------------------

function deviceView(d) {
  const st = liveStatus.get(d.id) || null;
  return {
    id: d.id,
    name: d.name,
    online: Date.now() - d.lastSeen < DEVICE_ONLINE_MS,
    lastSeen: d.lastSeen,
    status: st,
    history: d.history.slice(-15).reverse(),
  };
}

// Nom de ROM sans dossier (la DSi revérifie de son côté)
function romName(arg) {
  const f = String(arg || '');
  if (!/^[^/\\\t\r\n]{1,128}\.(nds|dsi)$/i.test(f) || f.includes('..')) return null;
  return f;
}

const COMMANDS = {
  install: (arg) => {
    const link = String(arg || '').trim();
    if (!link || link.length > 300 || /[\t\r\n\s]/.test(link)) return null;
    return link;
  },
  uninstall: (arg) => romName(arg),
  update: (arg) => romName(arg),
  cancel: () => '',
  reboot: () => '',
  refresh: () => '',
};

async function webApi(req, res, route) {
  // Protection CSRF : en-tête personnalisé obligatoire (impossible à envoyer
  // depuis un autre site sans requête "preflight" CORS, que l'on refuse)
  if (req.method !== 'GET' && req.headers['x-dsishop'] !== '1') {
    return send(res, 403, { error: 'Requête refusée' });
  }

  if (route === 'config' && req.method === 'GET') {
    return send(res, 200, { googleClientId: CONFIG.googleClientId, devLogin: CONFIG.devLogin });
  }

  if (route === 'auth/google' && req.method === 'POST') {
    if (rateLimited('login:' + clientIp(req), 30, 600 * 1000)) {
      return send(res, 429, { error: 'Trop de tentatives' });
    }
    if (!CONFIG.googleClientId) return send(res, 500, { error: 'GOOGLE_CLIENT_ID non configuré' });
    const { credential } = await readJson(req);
    try {
      const user = await verifyGoogleIdToken(credential);
      const cookie = startSession(req, res, user);
      return send(res, 200, { user }, undefined, { 'Set-Cookie': cookie });
    } catch (e) {
      return send(res, 401, { error: 'Connexion Google refusée : ' + e.message });
    }
  }

  if (route === 'auth/dev' && req.method === 'POST') {
    if (!CONFIG.devLogin) return send(res, 404, { error: 'Désactivé' });
    const { name, email } = await readJson(req);
    const user = {
      id: 'dev:' + sha256(String(email || 'dev')).slice(0, 16),
      name: clean(name || 'Testeur', 60), email: clean(email || '', 90), picture: '',
    };
    const cookie = startSession(req, res, user);
    return send(res, 200, { user }, undefined, { 'Set-Cookie': cookie });
  }

  if (route === 'auth/logout' && req.method === 'POST') {
    const sid = parseCookies(req).dsishop_session;
    if (sid) { delete db.sessions[sha256(sid)]; save(); }
    return send(res, 200, { ok: true }, undefined,
      { 'Set-Cookie': 'dsishop_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax' });
  }

  const user = sessionUser(req);
  if (!user) return send(res, 401, { error: 'Non connecté' });

  if (route === 'me' && req.method === 'GET') {
    const devices = Object.values(db.devices).filter((d) => d.ownerId === user.id).map(deviceView);
    return send(res, 200, { user, devices });
  }

  if (route === 'pair' && req.method === 'POST') {
    if (rateLimited('pair:' + user.id, 20, 600 * 1000)) {
      return send(res, 429, { error: 'Trop de tentatives' });
    }
    const { code } = await readJson(req);
    const p = pairCodes.get(String(code || '').toUpperCase());
    if (!p || p.expires < Date.now() || !db.devices[p.deviceId]) {
      return send(res, 404, { error: 'Code expiré ou invalide : affiche un nouveau QR code sur la DSi' });
    }
    pairCodes.delete(String(code).toUpperCase());
    const d = db.devices[p.deviceId];
    d.ownerId = user.id;
    save();
    // Réveille la DSi pour qu'elle affiche tout de suite le compte
    pollWaiters.get(d.id)?.();
    return send(res, 200, { device: deviceView(d) });
  }

  const m = /^devices\/([a-f0-9]{16})(?:\/(commands))?$/.exec(route);
  if (m) {
    const d = db.devices[m[1]];
    if (!d || d.ownerId !== user.id) return send(res, 404, { error: 'DSi introuvable' });

    if (!m[2] && req.method === 'GET') return send(res, 200, { device: deviceView(d) });

    if (!m[2] && req.method === 'DELETE') {
      d.ownerId = null;
      save();
      pollWaiters.get(d.id)?.();
      return send(res, 200, { ok: true });
    }

    if (m[2] && req.method === 'POST') {
      const { type, arg } = await readJson(req);
      const check = COMMANDS[type];
      const value = check ? check(arg) : null;
      if (value === null) return send(res, 400, { error: 'Commande invalide' });
      const queue = pendingCmds.get(d.id) || [];
      if (queue.length >= 20) return send(res, 429, { error: 'Trop de commandes en attente' });
      const cmd = { id: randomHex(6), type, arg: value, at: Date.now(), state: 'queued' };
      queue.push(cmd);
      pendingCmds.set(d.id, queue);
      d.history.push({ ...cmd });
      if (d.history.length > 50) d.history.splice(0, d.history.length - 50);
      save();
      pollWaiters.get(d.id)?.();
      return send(res, 200, { command: cmd });
    }
  }

  return send(res, 404, { error: 'Route inconnue' });
}

// ---------------------------------------------------------------------------
// Fichiers statiques
// ---------------------------------------------------------------------------

const PUBLIC_DIR = path.join(__dirname, 'public');
const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
};
const CSP = [
  "default-src 'self'",
  "script-src 'self' https://accounts.google.com/gsi/client",
  "style-src 'self' 'unsafe-inline' https://accounts.google.com/gsi/style",
  "frame-src https://accounts.google.com/gsi/",
  "connect-src 'self' https://accounts.google.com/gsi/ https://api.github.com",
  "img-src 'self' data: https://*.googleusercontent.com https://avatars.githubusercontent.com",
].join('; ');

function serveStatic(req, res, pathname) {
  // Toutes les pages (/, /p/CODE) servent la même application
  let file = pathname === '/' || pathname.startsWith('/p/') ? '/index.html' : pathname;
  file = path.normalize(file).replace(/^(\.\.[/\\])+/, '');
  const full = path.join(PUBLIC_DIR, file);
  if (!full.startsWith(PUBLIC_DIR)) return send(res, 404, 'Introuvable', 'text/plain');
  fs.readFile(full, (err, data) => {
    if (err) return send(res, 404, 'Introuvable', 'text/plain; charset=utf-8');
    const headers = {
      'Content-Security-Policy': CSP,
      'Referrer-Policy': 'no-referrer-when-downgrade',
      'Cross-Origin-Opener-Policy': 'same-origin-allow-popups',
    };
    return send(res, 200, data, MIME[path.extname(full)] || 'application/octet-stream', headers);
  });
  return undefined;
}

// ---------------------------------------------------------------------------

const server = http.createServer(async (req, res) => {
  try {
    const { pathname } = new URL(req.url, 'http://x');
    if (pathname.startsWith('/api/device/')) {
      return await deviceApi(req, res, pathname.slice('/api/device/'.length));
    }
    if (pathname.startsWith('/api/')) {
      return await webApi(req, res, pathname.slice('/api/'.length));
    }
    if (pathname === '/healthz') return sendText(res, 200, 'ok');
    return serveStatic(req, res, pathname);
  } catch (e) {
    console.error(e);
    if (!res.headersSent) send(res, 500, { error: 'Erreur interne' });
    return undefined;
  }
});

// Les long polls durent jusqu'à 25 s
server.requestTimeout = 60 * 1000;
server.keepAliveTimeout = 65 * 1000;

if (require.main === module) {
  server.listen(CONFIG.port, () => {
    console.log(`DSi Shop Remote sur le port ${CONFIG.port}`);
    if (!CONFIG.googleClientId) console.warn('Attention : GOOGLE_CLIENT_ID non défini');
    if (CONFIG.devLogin) console.warn('ATTENTION : DEV_LOGIN=1, connexion sans Google activée (tests uniquement)');
  });
}

module.exports = { server, verifyGoogleIdToken, CONFIG };
