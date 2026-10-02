import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm, readFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import https from 'node:https';
import tls from 'node:tls';
import { createApplication } from '../server/index.mjs';
import { generateCertificate } from '../server/pairing.mjs';

async function fixture(t) {
  const directory = await mkdtemp(path.join(os.tmpdir(), 'aiq-service-test-'));
  const app = await createApplication({ directory, adminPort: 0, dataPort: 0, executables: { codex: null, claude: null }, interfaces: () => [] });
  const address = await app.listen();
  t.after(async () => { await app.close(); await rm(directory, { recursive: true, force: true }); });
  return { app, directory, url: `http://127.0.0.1:${address.port}` };
}
test('loopback admin denies foreign origin, rebinding Host, and missing CSRF', async t => {
  const { app, url } = await fixture(t);
  const state = await fetch(`${url}/api/state`).then(response => response.json());
  assert.equal(state.device.enabled, false); assert.equal(state.accounts.length, 0);
  const body = JSON.stringify({ refresh_seconds: 60, auto_refresh: false });
  assert.equal((await fetch(`${url}/api/settings`, { method: 'PATCH', headers: { 'Content-Type': 'application/json' }, body })).status, 403);
  assert.equal((await fetch(`${url}/api/settings`, { method: 'PATCH', headers: { 'Content-Type': 'application/json', 'X-AIQ-CSRF': state.csrf_token, Origin: 'https://evil.example' }, body })).status, 403);
  const valid = await fetch(`${url}/api/settings`, { method: 'PATCH', headers: { 'Content-Type': 'application/json', 'X-AIQ-CSRF': state.csrf_token, Origin: url }, body });
  assert.equal(valid.status, 200); assert.equal(app.store.state.settings.refresh_seconds, 60);
  const hostile = await new Promise(resolve => { http.get(`${url}/api/state`, { headers: { Host: 'evil.example' } }, response => { response.resume(); resolve(response.statusCode); }); });
  assert.equal(hostile, 403);
  assert.equal((await fetch(`${url}/api/state`, { headers: { Origin: 'https://evil.example' } })).status, 403);
  assert.equal((await fetch(`${url}/api/accounts`, { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-AIQ-CSRF': state.csrf_token }, body: JSON.stringify({ provider: 'codex' }) })).status, 503);
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
  assert.equal(snapshot.body.accounts[0].seven_day, null); assert.equal(JSON.stringify(snapshot.body).includes('test-pair-token'), false);
  assert.equal((await request('/v1/settings', { token: 'test-pair-token', method: 'PATCH', body: { refresh_seconds: 900, auto_refresh: true } })).status, 200);
  assert.equal(app.store.state.settings.refresh_seconds, 900);
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
