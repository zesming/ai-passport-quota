import http from 'node:http';
import path from 'node:path';
import os from 'node:os';
import { readFile } from 'node:fs/promises';
import { randomBytes } from 'node:crypto';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { StateStore, loadState } from './storage.mjs';
import { AccountManager } from './accounts.mjs';
import { findCLI } from './clients.mjs';
import { PairingService, availableInterfaces } from './pairing.mjs';
import { constantToken, makeSnapshot, validSettings, epoch } from './protocol.mjs';

export function json(response, status, body) {
  const payload = Buffer.from(JSON.stringify(body));
  response.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8', 'Content-Length': payload.length, 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff' }); response.end(payload);
}
async function readBody(request, max = 8192) {
  if (!request.headers['content-type']?.toLowerCase().startsWith('application/json')) throw new Error('invalid_content_type');
  let size = 0; const chunks = [];
  for await (const chunk of request) { size += chunk.length; if (size > max) throw new Error('request_too_large'); chunks.push(chunk); }
  try { return JSON.parse(Buffer.concat(chunks).toString('utf8')); } catch { throw new Error('invalid_json'); }
}
const errorStatuses = {
  account_not_found: 404,
  invalid_provider: 400, invalid_interface: 400, invalid_settings: 400, invalid_login_code: 400,
  invalid_api_key: 400, invalid_account_label: 400, invalid_json: 400, invalid_content_type: 400,
  account_limit: 400, unsupported_account: 400,
  request_too_large: 413, cli_unavailable: 503, pairing_address_active: 409, account_busy: 409,
};
function errorStatus(error) { return Object.hasOwn(errorStatuses, error.message) ? errorStatuses[error.message] : 500; }
export async function createApplication({ directory = process.env.AIQ_STATE_DIR ?? path.join(os.homedir(), '.local/share/ai-passport-quota'), adminPort = 4317, dataPort = 4318, executables, managerFactory, interfaces = availableInterfaces, autoRestore = true, previewStatus } = {}) {
  directory = path.resolve(directory);
  const store = new StateStore(directory, await loadState(directory));
  const cli = executables ?? { codex: await findCLI('codex'), claude: await findCLI('claude') };
  const manager = managerFactory ? managerFactory(store, cli) : new AccountManager(store, cli);
  const csrf = randomBytes(32).toString('base64url');
  let pairing; let refreshTimer;
  const beginTimer = () => {
    if (refreshTimer) clearInterval(refreshTimer);
    refreshTimer = null;
    if (store.state.settings.auto_refresh) {
      refreshTimer = setInterval(() => manager.schedule(), store.state.settings.refresh_seconds * 1000);
      refreshTimer.unref();
    }
  };
  const settings = async body => {
    if (!validSettings(body)) throw new Error('invalid_settings');
    const previous = store.state.settings;
    const sourceSettingsChanged = previous.refresh_seconds !== body.refresh_seconds || previous.auto_refresh !== body.auto_refresh;
    store.state.settings = {
      refresh_seconds: body.refresh_seconds,
      auto_refresh: body.auto_refresh,
      screen_timeout_seconds: body.screen_timeout_seconds ?? previous.screen_timeout_seconds,
    };
    await store.changed();
    if (sourceSettingsChanged) beginTimer();
    return { v: 1, settings: { ...store.state.settings } };
  };
  const dataHandler = async (request, response) => {
    try {
      const expected = pairing.config?.enabled ? `Bearer ${pairing.config.token}` : null;
      if (!constantToken(request.headers.authorization, expected)) { json(response, 401, { error: 'unauthorized' }); return; }
      pairing.lastSeen = epoch();
      if (request.url === '/v1/snapshot' && request.method === 'GET') { json(response, 200, makeSnapshot(store.state)); return; }
      if (request.url === '/v1/refresh' && request.method === 'POST') { await readBody(request, 1024); manager.schedule(); json(response, 202, { v: 1, accepted: true }); return; }
      if (request.url === '/v1/settings' && request.method === 'PATCH') { json(response, 200, await settings(await readBody(request, 1024))); return; }
      json(response, 404, { error: 'not_found' });
    } catch (error) { json(response, errorStatus(error), { error: Object.hasOwn(errorStatuses, error.message) ? error.message : 'local_service_error' }); }
  };
  pairing = new PairingService(directory, dataHandler, { dataPort, interfaces });
  if (autoRestore) await pairing.restore();
  const frontend = path.resolve(fileURLToPath(new URL('../dist/client/', import.meta.url)));
  const admin = http.createServer(async (request, response) => {
    try {
      const port = admin.address()?.port ?? adminPort;
      const allowedHosts = new Set([`127.0.0.1:${port}`, `localhost:${port}`]);
      if (!allowedHosts.has(request.headers.host)) { json(response, 403, { error: 'invalid_host' }); return; }
      const origin = request.headers.origin;
      if (origin && ![`http://127.0.0.1:${port}`, `http://localhost:${port}`, process.env.AIQ_DEV_ORIGIN].filter(Boolean).includes(origin)) { json(response, 403, { error: 'invalid_origin' }); return; }
      if (request.url.startsWith('/api/')) {
        if (!['GET', 'HEAD'].includes(request.method) && !constantToken(request.headers['x-aiq-csrf'], csrf)) { json(response, 403, { error: 'invalid_csrf' }); return; }
        if (request.method === 'GET' && request.url === '/api/state') {
          const state = { accounts: manager.publicAccounts(), settings: store.state.settings, cli: { codex: !!cli.codex, claude: !!cli.claude }, interfaces: interfaces(), device: pairing.publicState(), pending_logins: manager.publicJobs(), csrf_token: csrf };
          if (previewStatus) state.preview_status = {
            wifi_connected: typeof previewStatus.wifi_connected === 'boolean' ? previewStatus.wifi_connected : null,
            battery_percent: Number.isInteger(previewStatus.battery_percent) && previewStatus.battery_percent >= 0 && previewStatus.battery_percent <= 100 ? previewStatus.battery_percent : null,
          };
          json(response, 200, state); return;
        }
        if (request.method === 'POST' && request.url === '/api/accounts') { const body = await readBody(request); json(response, 201, await manager.add(body.provider, body)); return; }
        if (request.method === 'POST' && request.url === '/api/refresh') { await readBody(request); manager.schedule(); json(response, 202, { accepted: true }); return; }
        if (request.method === 'PATCH' && request.url === '/api/settings') { json(response, 200, await settings(await readBody(request))); return; }
        if (request.method === 'POST' && request.url === '/api/pairing') { const body = await readBody(request); json(response, 200, await pairing.start(body.address ?? body.listen_address)); return; }
        if (request.method === 'POST' && request.url === '/api/pairing/abort') { const body = await readBody(request); await pairing.abort(body.session_id); json(response, 200, { ok: true }); return; }
        if (['POST', 'DELETE'].includes(request.method) && ['/api/pairing/stop', '/api/pairing'].includes(request.url) && !(request.method === 'POST' && request.url === '/api/pairing')) { await pairing.stop(); json(response, 200, { ok: true }); return; }
        const match = request.url.match(/^\/api\/accounts\/([a-f0-9]{32})(?:\/(login|refresh|launch|login-code|api-key))?$/);
        if (match) {
          const [, id, action] = match;
          if (request.method === 'DELETE' && !action) { await manager.remove(id); json(response, 200, { ok: true }); return; }
          if (request.method === 'POST' && action === 'api-key') { const body = await readBody(request, 8192); json(response, 200, await manager.updateDeepSeekApiKey(id, body)); return; }
          if (request.method === 'POST' && action === 'login') { await readBody(request); json(response, 200, { account_id: id, login: await manager.login(id) }); return; }
          if (request.method === 'POST' && action === 'refresh') { await readBody(request); manager.account(id); manager.schedule(id); json(response, 202, { accepted: true }); return; }
          if (request.method === 'POST' && action === 'login-code') { const body = await readBody(request); manager.loginCode(id, body.code); json(response, 202, { accepted: true }); return; }
          if (request.method === 'GET' && action === 'launch') { json(response, 200, await manager.launchCommand(id)); return; }
        }
        json(response, 404, { error: 'not_found' }); return;
      }
      if (request.method !== 'GET' && request.method !== 'HEAD') { json(response, 405, { error: 'method_not_allowed' }); return; }
      let relative; try { relative = decodeURIComponent(new URL(request.url, 'http://localhost').pathname); } catch { json(response, 400, { error: 'invalid_path' }); return; }
      const filename = path.resolve(frontend, `.${relative === '/' ? '/index.html' : relative}`);
      if (!filename.startsWith(`${frontend}${path.sep}`)) { json(response, 403, { error: 'invalid_path' }); return; }
      let content; try { content = await readFile(filename); } catch { json(response, 404, { error: 'not_found' }); return; }
      const extension = path.extname(filename);
      const types = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8', '.svg': 'image/svg+xml', '.png': 'image/png' };
      response.writeHead(200, { 'Content-Type': types[extension] ?? 'application/octet-stream', 'Content-Length': content.length, 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'no-referrer', 'Content-Security-Policy': "default-src 'self'; style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; font-src 'self' https://fonts.gstatic.com; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'self'; form-action 'self'", 'Permissions-Policy': 'serial=(self)' }); response.end(request.method === 'HEAD' ? undefined : content);
    } catch (error) { json(response, errorStatus(error), { error: Object.hasOwn(errorStatuses, error.message) ? error.message : 'local_service_error' }); }
  });
  admin.requestTimeout = 30000; admin.headersTimeout = 10000;
  beginTimer();
  return {
    admin, store, manager, pairing,
    async listen() { await new Promise((resolve, reject) => { admin.once('error', reject); admin.listen(adminPort, '127.0.0.1', resolve); }); for (const account of store.state.accounts) manager.schedule(account.id); return admin.address(); },
    async close() { clearInterval(refreshTimer); manager.close(); await pairing.close(); if (admin.listening) { admin.closeAllConnections(); await new Promise(resolve => admin.close(resolve)); } await store.pendingWrite.catch(() => {}); },
  };
}
async function main() {
  const application = await createApplication();
  const address = await application.listen();
  console.log(`AI 额度本地设置页：http://127.0.0.1:${address.port}/`);
  const stop = async () => { await application.close(); process.exit(0); };
  process.once('SIGINT', stop); process.once('SIGTERM', stop);
}
if (process.argv[1] && pathToFileURL(path.resolve(process.argv[1])).href === import.meta.url) {
  main().catch(() => { console.error('AI 额度启动失败，请检查端口或本地数据目录。'); process.exitCode = 1; });
}
