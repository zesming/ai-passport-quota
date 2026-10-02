import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm, readFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import https from 'node:https';
import tls from 'node:tls';
import { createApplication } from '../server/index.mjs';
import { AccountManager } from '../server/accounts.mjs';
import { generateCertificate } from '../server/pairing.mjs';

async function fixture(t, options = {}) {
  const directory = await mkdtemp(path.join(os.tmpdir(), 'aiq-service-test-'));
  const app = await createApplication({ directory, adminPort: 0, dataPort: 0, executables: { codex: null, claude: null }, interfaces: () => [], ...options });
  const address = await app.listen();
  t.after(async () => { await app.close(); await rm(directory, { recursive: true, force: true }); });
  return { app, directory, url: `http://127.0.0.1:${address.port}` };
}
test('loopback admin denies foreign origin, rebinding Host, and missing CSRF', async t => {
  const { app, url } = await fixture(t);
  const state = await fetch(`${url}/api/state`).then(response => response.json());
  assert.equal(state.device.enabled, false); assert.equal(state.accounts.length, 0);
  assert.deepEqual(state.settings, { refresh_seconds: 300, auto_refresh: true, screen_timeout_seconds: 120 });
  const body = JSON.stringify({ refresh_seconds: 60, auto_refresh: false });
  assert.equal((await fetch(`${url}/api/settings`, { method: 'PATCH', headers: { 'Content-Type': 'application/json' }, body })).status, 403);
  assert.equal((await fetch(`${url}/api/settings`, { method: 'PATCH', headers: { 'Content-Type': 'application/json', 'X-AIQ-CSRF': state.csrf_token, Origin: 'https://evil.example' }, body })).status, 403);
  const valid = await fetch(`${url}/api/settings`, { method: 'PATCH', headers: { 'Content-Type': 'application/json', 'X-AIQ-CSRF': state.csrf_token, Origin: url }, body });
  assert.equal(valid.status, 200);
  assert.deepEqual((await valid.json()).settings, { refresh_seconds: 60, auto_refresh: false, screen_timeout_seconds: 120 });
  assert.equal(app.store.state.settings.refresh_seconds, 60);
  const extended = await fetch(`${url}/api/settings`, { method: 'PATCH', headers: { 'Content-Type': 'application/json', 'X-AIQ-CSRF': state.csrf_token, Origin: url }, body: JSON.stringify({ refresh_seconds: 60, auto_refresh: false, screen_timeout_seconds: 600 }) });
  assert.deepEqual((await extended.json()).settings, { refresh_seconds: 60, auto_refresh: false, screen_timeout_seconds: 600 });
  const legacyPatch = await fetch(`${url}/api/settings`, { method: 'PATCH', headers: { 'Content-Type': 'application/json', 'X-AIQ-CSRF': state.csrf_token, Origin: url }, body: JSON.stringify({ refresh_seconds: 900, auto_refresh: true }) });
  assert.deepEqual((await legacyPatch.json()).settings, { refresh_seconds: 900, auto_refresh: true, screen_timeout_seconds: 600 });
  const hostile = await new Promise(resolve => { http.get(`${url}/api/state`, { headers: { Host: 'evil.example' } }, response => { response.resume(); resolve(response.statusCode); }); });
  assert.equal(hostile, 403);
  assert.equal((await fetch(`${url}/api/state`, { headers: { Origin: 'https://evil.example' } })).status, 403);
  assert.equal((await fetch(`${url}/api/accounts`, { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-AIQ-CSRF': state.csrf_token }, body: JSON.stringify({ provider: 'codex' }) })).status, 503);
  assert.equal(Object.hasOwn(state, 'preview_status'), false);
});
test('synthetic device status is only returned when a preview fixture supplies it', async t => {
  const realState = await fixture(t);
  const syntheticState = await fixture(t, { previewStatus: { wifi_connected: true, battery_percent: 76 } });
  assert.equal(Object.hasOwn(await fetch(`${realState.url}/api/state`).then(response => response.json()), 'preview_status'), false);
  assert.deepEqual((await fetch(`${syntheticState.url}/api/state`).then(response => response.json())).preview_status, { wifi_connected: true, battery_percent: 76 });
});
test('DeepSeek create, alias edit, key replacement, and login rejection stay scoped and secret-free', async t => {
  const calls = []; let claudeRuns = 0;
  const balance = { is_available: true, balance_infos: [{ currency: 'CNY', total_balance: '9.00', granted_balance: '0', topped_up_balance: '9.00' }] };
  const { app, url } = await fixture(t, {
    managerFactory: (store, cli) => new AccountManager(store, cli, {
      claudeRun: async () => { claudeRuns += 1; throw new Error('unexpected CLI call'); },
      deepSeekFetch: async (endpoint, options) => {
        calls.push({ endpoint, authorization: options.headers.Authorization, method: options.method });
        return new Response(JSON.stringify(balance), { status: 200, headers: { 'content-type': 'application/json' } });
      },
    }),
  });
  const csrf = (await fetch(`${url}/api/state`).then(response => response.json())).csrf_token;
  const post = (route, body) => fetch(`${url}${route}`, {
    method: 'POST', headers: { 'Content-Type': 'application/json', 'X-AIQ-CSRF': csrf }, body: JSON.stringify(body),
  });
  const firstKey = 'sk-route-first'; const secondKey = 'sk-route-second';
  const addedResponse = await post('/api/accounts', { provider: 'deepseek', api_key: firstKey, label: 'Route API' });
  assert.equal(addedResponse.status, 201);
  const added = await addedResponse.json(); const id = added.account_id;
  assert.equal(JSON.stringify(added).includes(firstKey), false);
  assert.deepEqual(calls[0], { endpoint: 'https://api.deepseek.com/user/balance', authorization: `Bearer ${firstKey}`, method: 'GET' });

  let state = await fetch(`${url}/api/state`).then(response => response.json());
  assert.equal(state.accounts[0].provider, 'deepseek'); assert.equal(state.accounts[0].label, 'Route API');
  assert.deepEqual(state.accounts[0].balance, balance); assert.equal(JSON.stringify(state).includes(firstKey), false);

  const renameResponse = await post(`/api/accounts/${id}/api-key`, { label: 'Renamed API' });
  assert.equal(renameResponse.status, 200); assert.equal(calls.length, 1);
  state = await fetch(`${url}/api/state`).then(response => response.json());
  assert.equal(state.accounts[0].label, 'Renamed API'); assert.deepEqual(state.accounts[0].balance, balance);

  const replacement = await post(`/api/accounts/${id}/api-key`, { api_key: secondKey, label: 'Replacement API' });
  assert.equal(replacement.status, 200); assert.equal(JSON.stringify(await replacement.json()).includes(secondKey), false);
  assert.equal(calls.length, 2); assert.equal(calls[1].authorization, `Bearer ${secondKey}`);
  state = await fetch(`${url}/api/state`).then(response => response.json());
  assert.equal(state.accounts[0].label, 'Replacement API');
  assert.equal(JSON.stringify(state).includes(firstKey), false); assert.equal(JSON.stringify(state).includes(secondKey), false);

  const loginResponse = await post(`/api/accounts/${id}/login`, {});
  assert.equal(loginResponse.status, 400); assert.equal((await loginResponse.json()).error, 'unsupported_account');
  assert.equal(claudeRuns, 0);
  assert.equal((await post(`/api/accounts/${id}/api-key`, { label: 'x'.repeat(33) })).status, 400);
});
test('screen timeout-only patches keep the quota refresh timer', async t => {
  const originalSetInterval = globalThis.setInterval;
  const originalClearInterval = globalThis.clearInterval;
  const refreshTimers = [];
  globalThis.setInterval = (callback, delay) => {
    const timer = { callback, delay, cleared: false, unref() {} };
    refreshTimers.push(timer);
    return timer;
  };
  globalThis.clearInterval = timer => { if (timer) timer.cleared = true; };

  const directory = await mkdtemp(path.join(os.tmpdir(), 'aiq-refresh-timer-test-'));
  let app;
  t.after(async () => {
    if (app) await app.close();
    await rm(directory, { recursive: true, force: true });
    globalThis.setInterval = originalSetInterval;
    globalThis.clearInterval = originalClearInterval;
  });
  app = await createApplication({
    directory, adminPort: 0, dataPort: 0, autoRestore: false,
    executables: { codex: null, claude: null }, interfaces: () => [],
    managerFactory: () => ({ publicAccounts: () => [], publicJobs: () => [], schedule() {}, close() {} }),
  });
  const address = await app.listen();
  const url = `http://127.0.0.1:${address.port}`;
  const state = await fetch(`${url}/api/state`).then(response => response.json());
  assert.deepEqual(refreshTimers.map(timer => timer.delay), [300000]);

  const patch = body => fetch(`${url}/api/settings`, {
    method: 'PATCH',
    headers: { 'Content-Type': 'application/json', 'X-AIQ-CSRF': state.csrf_token, Origin: url },
    body: JSON.stringify(body),
  });
  assert.equal((await patch({ refresh_seconds: 300, auto_refresh: true, screen_timeout_seconds: 30 })).status, 200);
  assert.deepEqual(refreshTimers.map(timer => timer.delay), [300000]);
  assert.equal(refreshTimers[0].cleared, false);

  assert.equal((await patch({ refresh_seconds: 60, auto_refresh: true, screen_timeout_seconds: 30 })).status, 200);
  assert.deepEqual(refreshTimers.map(timer => timer.delay), [300000, 60000]);
  assert.equal(refreshTimers[0].cleared, true);
  assert.equal(refreshTimers[1].cleared, false);
});
test('HTTPS device route requires bearer and pinned certificate; settings synchronize', async t => {
  const { app, directory } = await fixture(t);
  const certificate = await generateCertificate(path.join(directory, 'test-certificate'), '192.168.88.1');
  const cert = await readFile(certificate.cert); const key = await readFile(certificate.key);
  app.pairing.config = { enabled: true, token: 'test-pair-token' };
  app.store.state.accounts.push({ id: 'd'.repeat(32), provider: 'codex', email: 'device@example.com', plan: 'Plus', status: 'ok', observed_at: 1700000000, five_hour: { remaining_percent: 0, resets_at: 2000000000 }, seven_day: null, authenticated: true });
  const server = https.createServer({ cert, key }, app.pairing.handler);
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise(resolve => { server.closeAllConnections(); server.close(resolve); }));
  const request = (route, { token, method = 'GET', body, ca = cert } = {}) => new Promise((resolve, reject) => {
    const payload = body ? JSON.stringify(body) : undefined;
    const req = https.request({ hostname: '127.0.0.1', port: server.address().port, path: route, method, ca, checkServerIdentity: (_host, peer) => tls.checkServerIdentity('192.168.88.1', peer), headers: { ...(token ? { Authorization: `Bearer ${token}` } : {}), ...(payload ? { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(payload) } : {}) } }, response => { let data = ''; response.on('data', chunk => { data += chunk; }); response.on('end', () => resolve({ status: response.statusCode, body: JSON.parse(data) })); });
    req.on('error', reject); req.end(payload);
  });
  assert.equal((await request('/v1/snapshot')).status, 401); assert.equal((await request('/v1/snapshot', { token: 'wrong-token' })).status, 401);
  const snapshot = await request('/v1/snapshot', { token: 'test-pair-token' });
  assert.equal(snapshot.status, 200); assert.equal(snapshot.body.accounts[0].five_hour.remaining_percent, 0);
  assert.deepEqual(snapshot.body.settings, { refresh_seconds: 300, auto_refresh: true, screen_timeout_seconds: 120 });
  assert.equal(snapshot.body.accounts[0].seven_day, null); assert.equal(JSON.stringify(snapshot.body).includes('test-pair-token'), false);
  const never = await request('/v1/settings', { token: 'test-pair-token', method: 'PATCH', body: { refresh_seconds: 900, auto_refresh: true, screen_timeout_seconds: 0 } });
  assert.equal(never.status, 200); assert.deepEqual(never.body.settings, { refresh_seconds: 900, auto_refresh: true, screen_timeout_seconds: 0 });
  const legacyUpdate = await request('/v1/settings', { token: 'test-pair-token', method: 'PATCH', body: { refresh_seconds: 60, auto_refresh: false } });
  assert.deepEqual(legacyUpdate.body.settings, { refresh_seconds: 60, auto_refresh: false, screen_timeout_seconds: 0 });
  assert.deepEqual(app.store.state.settings, { refresh_seconds: 60, auto_refresh: false, screen_timeout_seconds: 0 });
  assert.equal((await request('/v1/settings', { token: 'test-pair-token', method: 'PATCH', body: { refresh_seconds: 300, auto_refresh: true, screen_timeout_seconds: 90 } })).status, 400);
  assert.equal((await request('/v1/settings', { token: 'test-pair-token', method: 'PATCH', body: { refresh_seconds: 2, auto_refresh: true } })).status, 400);
  await assert.rejects(request('/v1/snapshot', { token: 'test-pair-token', ca: null }));
});
test('pairing operations serialize and re-pairing repairs an unusable certificate', async t => {
  const { PairingService } = await import('../server/pairing.mjs'); const directory = await mkdtemp(path.join(os.tmpdir(), 'aiq-pair-lifecycle-')); t.after(() => rm(directory, { recursive: true, force: true }));
  const service = new PairingService(directory, () => {}, { interfaces: () => [{ name: 'test', address: '192.168.88.1' }] }); const events = [];
  // Test lifecycle ordering without binding a LAN socket.
  service.listen = async () => { events.push('listen'); service.config.enabled = true; service.server = { listening: true }; }; service.closeServer = async () => { events.push('close'); service.server = null; };
  const [paired] = await Promise.all([service.start('192.168.88.1'), service.stop()]); assert.deepEqual(events.slice(-2), ['listen', 'close']); assert.equal(service.config.enabled, false);
  service.usableCertificate = async () => false; const repaired = await service.start('192.168.88.1'); assert.notEqual(repaired.pair_token, paired.pair_token); assert.equal(repaired.server_cert_pem.includes('BEGIN CERTIFICATE'), true);
});
test('a stale failed pairing attempt cannot revoke an active or newer pairing', async t => {
  const { PairingService } = await import('../server/pairing.mjs'); const directory = await mkdtemp(path.join(os.tmpdir(), 'aiq-pair-session-')); t.after(() => rm(directory, { recursive: true, force: true }));
  const service = new PairingService(directory, () => {}, { interfaces: () => [{ name: 'test', address: '192.168.88.1' }, { name: 'other', address: '192.168.88.2' }] });
  service.listen = async () => { service.config.enabled = true; service.server = { listening: true }; }; service.closeServer = async () => { service.server = null; };
  const first = await service.start('192.168.88.1'); const second = await service.start('192.168.88.1');
  await service.abort(first.pairing_session); await service.abort(second.pairing_session);
  assert.equal(service.config.enabled, true); assert.equal(service.config.token, first.pair_token);
  await assert.rejects(service.start('192.168.88.2'), /pairing_address_active/); assert.equal(service.config.token, first.pair_token);
  await service.stop(); const fresh = await service.start('192.168.88.1'); await service.abort(fresh.pairing_session); assert.equal(service.config.enabled, false);
});
