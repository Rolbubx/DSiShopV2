// Tests du serveur : node --test   (aucune dépendance)
'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'dsishop-'));
process.env.DATA_FILE = path.join(tmp, 'data.json');
process.env.DEV_LOGIN = '1';
process.env.GOOGLE_CLIENT_ID = 'test-client.apps.googleusercontent.com';

const { server } = require('./server.js');

let base;
test.before(() => new Promise((r) => server.listen(0, () => {
  base = `http://127.0.0.1:${server.address().port}`;
  r();
})));
test.after(() => { server.closeAllConnections(); server.close(); });

async function web(method, p, body, cookie) {
  const res = await fetch(base + '/api/' + p, {
    method,
    headers: { 'Content-Type': 'application/json', 'X-DSiShop': '1', ...(cookie ? { Cookie: cookie } : {}) },
    body: body ? JSON.stringify(body) : undefined,
  });
  return { status: res.status, json: await res.json().catch(() => ({})), cookie: res.headers.get('set-cookie') };
}

async function dev(method, p, token, body) {
  const res = await fetch(base + '/api/device/' + p, {
    method,
    headers: token ? { Authorization: 'Bearer ' + token } : {},
    body,
  });
  return { status: res.status, text: await res.text() };
}

const lines = (text) => text.trim().split('\n').map((l) => l.split('\t'));

test('parcours complet : enregistrement, appairage, commande, statut', async () => {
  // 1. La DSi s'enregistre
  const reg = await dev('POST', 'register', null, 'name\tDSi de test\n');
  assert.equal(reg.status, 200);
  const l = lines(reg.text);
  const [, deviceId, token] = l.find((x) => x[0] === 'device');
  const [, code, url] = l.find((x) => x[0] === 'pair');
  assert.match(token, /^[a-f0-9]{64}$/);
  assert.match(code, /^[A-HJ-NP-Z2-9]{8}$/);
  assert.equal(url, `${base}/p/${code}`);

  // 2. Pas encore de propriétaire
  let poll = await dev('GET', 'poll?wait=0', token);
  assert.equal(poll.text, 'owner\t-\n');

  // 3. Connexion (mode test) puis appairage
  const login = await web('POST', 'auth/dev', { name: 'Alice', email: 'alice@example.com' });
  const cookie = login.cookie.split(';')[0];
  assert.equal((await web('POST', 'pair', { code: 'ZZZZZZZZ' }, cookie)).status, 404);

  // Le long poll en cours doit être réveillé par l'appairage
  const pending = dev('GET', 'poll?wait=20', token);
  await new Promise((r) => setTimeout(r, 100));
  const pair = await web('POST', 'pair', { code }, cookie);
  assert.equal(pair.status, 200);
  poll = await pending;
  assert.equal(poll.text, 'owner\tAlice\talice@example.com\n');

  // Le code est à usage unique
  assert.equal((await web('POST', 'pair', { code }, cookie)).status, 404);

  // 4. Un autre compte ne voit pas cette DSi
  const bob = (await web('POST', 'auth/dev', { name: 'Bob', email: 'bob@example.com' })).cookie.split(';')[0];
  assert.equal((await web('GET', `devices/${deviceId}`, null, bob)).status, 404);
  assert.equal((await web('POST', `devices/${deviceId}/commands`, { type: 'reboot' }, bob)).status, 404);

  // 5. Commandes : validation
  assert.equal((await web('POST', `devices/${deviceId}/commands`, { type: 'rm -rf' }, cookie)).status, 400);
  assert.equal((await web('POST', `devices/${deviceId}/commands`,
    { type: 'uninstall', arg: '../../boot.nds' }, cookie)).status, 400);
  assert.equal((await web('POST', `devices/${deviceId}/commands`,
    { type: 'install', arg: 'a b' }, cookie)).status, 400);

  // Le long poll se réveille dès qu'une commande arrive
  const waiting = dev('GET', 'poll?wait=20', token);
  await new Promise((r) => setTimeout(r, 100));
  const t0 = Date.now();
  const cmd = await web('POST', `devices/${deviceId}/commands`,
    { type: 'install', arg: 'github.com/auteur/projet' }, cookie);
  assert.equal(cmd.status, 200);
  poll = await waiting;
  assert.ok(Date.now() - t0 < 2000, 'le long poll aurait dû se réveiller tout de suite');
  const c = lines(poll.text).find((x) => x[0] === 'cmd');
  assert.deepEqual(c, ['cmd', cmd.json.command.id, 'install', 'github.com/auteur/projet']);

  // Livrée une seule fois
  poll = await dev('GET', 'poll?wait=0', token);
  assert.equal(lines(poll.text).filter((x) => x[0] === 'cmd').length, 0);

  // 6. La DSi envoie son statut + l'acquittement
  const status = [
    'state\tdownloading', 'message\tTéléchargement de jeu.zip…', 'progress\t1048576\t4194304',
    'game\tPong.nds\t131072\tgithub.com/a/pong\tv1.0', 'game\tTetris.dsi\t262144\t\t',
    `ack\t${cmd.json.command.id}\tok\tInstallation lancée`, 'free\t1000000',
  ].join('\n');
  assert.equal((await dev('POST', 'status', token, status)).status, 200);

  const view = (await web('GET', `devices/${deviceId}`, null, cookie)).json.device;
  assert.equal(view.online, true);
  assert.equal(view.status.state, 'downloading');
  assert.deepEqual(view.status.progress, { now: 1048576, total: 4194304 });
  assert.equal(view.status.games.length, 2);
  assert.equal(view.status.games[0].version, 'v1.0');
  assert.equal(view.history[0].state, 'ok');
  assert.equal(view.history[0].result, 'Installation lancée');

  // 7. Mauvais jeton
  assert.equal((await dev('GET', 'poll', 'a'.repeat(64))).status, 401);

  // 8. Délier : la DSi n'a plus de propriétaire
  assert.equal((await web('DELETE', `devices/${deviceId}`, null, cookie)).status, 200);
  poll = await dev('GET', 'poll?wait=0', token);
  assert.equal(poll.text, 'owner\t-\n');

  // Nouveau code d'appairage pour la même DSi
  const pc = await dev('POST', 'pair-code', token);
  assert.equal(pc.status, 200);
  assert.ok(lines(pc.text).some((x) => x[0] === 'pair'));
});

test('protection CSRF : en-tête X-DSiShop obligatoire', async () => {
  const res = await fetch(base + '/api/auth/dev', { method: 'POST', body: '{}' });
  assert.equal(res.status, 403);
});

test('pages : / et /p/CODE servent l’application, pas de sortie du dossier public', async () => {
  for (const p of ['/', '/p/ABCD2345']) {
    const res = await fetch(base + p);
    assert.equal(res.status, 200);
    assert.match(res.headers.get('content-security-policy'), /default-src 'self'/);
    assert.match(await res.text(), /DSi Shop/);
  }
  const res = await fetch(base + '/..%2fserver.js');
  assert.equal(res.status, 404);
});

test('vérification des jetons Google (signature RS256, audience, expiration)', async () => {
  const mod = require('./server.js');
  const { publicKey, privateKey } = crypto.generateKeyPairSync('rsa', { modulusLength: 2048 });
  const jwk = { ...publicKey.export({ format: 'jwk' }), kid: 'k1', alg: 'RS256', use: 'sig' };

  // Remplace le téléchargement des clés Google par notre clé de test
  const realFetch = global.fetch;
  global.fetch = async (url, opts) => (String(url).includes('googleapis.com/oauth2/v3/certs')
    ? new Response(JSON.stringify({ keys: [jwk] }), { headers: { 'cache-control': 'max-age=60' } })
    : realFetch(url, opts));

  const sign = (payload, key = privateKey, kid = 'k1') => {
    const h = Buffer.from(JSON.stringify({ alg: 'RS256', kid, typ: 'JWT' })).toString('base64url');
    const p = Buffer.from(JSON.stringify(payload)).toString('base64url');
    const s = crypto.sign('RSA-SHA256', Buffer.from(h + '.' + p), key).toString('base64url');
    return `${h}.${p}.${s}`;
  };
  const now = Math.floor(Date.now() / 1000);
  const good = {
    iss: 'https://accounts.google.com', aud: mod.CONFIG.googleClientId, sub: '42',
    email: 'a@gmail.com', email_verified: true, name: 'Alice', exp: now + 600,
  };

  try {
    const user = await mod.verifyGoogleIdToken(sign(good));
    assert.equal(user.id, 'google:42');
    assert.equal(user.email, 'a@gmail.com');

    await assert.rejects(mod.verifyGoogleIdToken(sign({ ...good, aud: 'autre' })), /audience/);
    await assert.rejects(mod.verifyGoogleIdToken(sign({ ...good, exp: now - 3600 })), /expiré/);
    await assert.rejects(mod.verifyGoogleIdToken(sign({ ...good, iss: 'evil.com' })), /émetteur/);
    const other = crypto.generateKeyPairSync('rsa', { modulusLength: 2048 }).privateKey;
    await assert.rejects(mod.verifyGoogleIdToken(sign(good, other)), /signature/);
    await assert.rejects(mod.verifyGoogleIdToken(sign(good, privateKey, 'inconnue')), /clé/);

    // Bout en bout : POST /api/auth/google crée une session
    const res = await web('POST', 'auth/google', { credential: sign(good) });
    assert.equal(res.status, 200);
    assert.match(res.cookie, /dsishop_session=[a-f0-9]{64}; Path=\/; HttpOnly; SameSite=Lax/);
    const bad = await web('POST', 'auth/google', { credential: sign({ ...good, aud: 'x' }) });
    assert.equal(bad.status, 401);
  } finally {
    global.fetch = realFetch;
  }
});
