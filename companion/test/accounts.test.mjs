import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm, stat, readFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { EventEmitter } from 'node:events';
import { StateStore, loadState, readJSON } from '../server/storage.mjs';
import { AccountManager } from '../server/accounts.mjs';
import { recordClaudeFeed } from '../server/claude-feed.mjs';
import { profileEnvironment, officialLoginURL, shellQuote } from '../server/clients.mjs';

class FakeCodex extends EventEmitter {
  constructor(directory) { super(); this.directory = directory; this.calls = []; }
  async open() { return this; }
  async request(method, params) {
    this.calls.push({ method, params });
    if (method === 'account/login/start') return { loginId: 'test-login', userCode: 'TEST-CODE', verificationUrl: 'https://auth.openai.com/codex/device' };
    if (method === 'account/read') return { account: { type: 'chatgpt', email: 'test@example.com', planType: 'Plus' } };
    if (method === 'account/rateLimits/read') return { rateLimits: { primary: { windowDurationMins: 300, usedPercent: 25, resetsAt: 2000000000 }, secondary: { windowDurationMins: 10080, usedPercent: 38, resetsAt: 2000000500 } } };
    return {};
  }
  close() {}
}
async function fixture(t) {
  const directory = await mkdtemp(path.join(os.tmpdir(), 'aiq-account-test-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  return new StateStore(directory, await loadState(directory));
}
const cnyUsdBalance = {
  is_available: true,
  balance_infos: [
    { currency: 'CNY', total_balance: '128.50', granted_balance: '10.00', topped_up_balance: '118.50' },
    { currency: 'USD', total_balance: '17.25', granted_balance: '2.00', topped_up_balance: '15.25' },
  ],
};
const balanceResponse = value => new Response(JSON.stringify(value), { status: 200, headers: { 'content-type': 'application/json' } });

test('DeepSeek account keeps its key in a private profile and reloads only safe account fields', async t => {
  const store = await fixture(t); const calls = [];
  const manager = new AccountManager(store, { codex: null, claude: null }, { deepSeekFetch: async (url, options) => { calls.push({ url, options }); return balanceResponse(cnyUsdBalance); } });
  t.after(() => manager.close());
  const secret = 'sk-private-test-key';
  const { account_id: id } = await manager.add('deepseek', { api_key: secret, label: 'Team API' });
  const account = store.state.accounts[0]; const publicAccount = manager.publicAccounts()[0];
  assert.equal(account.id, id); assert.equal(account.provider, 'deepseek'); assert.equal(Object.hasOwn(account, 'api_key'), false);
  assert.equal(publicAccount.email, ''); assert.equal(publicAccount.plan, 'API'); assert.equal(publicAccount.label, 'Team API');
  assert.deepEqual(publicAccount.balance, cnyUsdBalance); assert.equal(publicAccount.five_hour, null); assert.equal(publicAccount.seven_day, null);
  assert.equal(publicAccount.authenticated, true); assert.equal(publicAccount.status, 'ok');
  assert.equal(calls[0].url, 'https://api.deepseek.com/user/balance'); assert.equal(calls[0].options.method, 'GET');
  assert.equal(calls[0].options.headers.Authorization, `Bearer ${secret}`); assert.equal(calls[0].options.redirect, 'error');

  const keyDirectory = path.join(store.profile(id), 'deepseek'); const keyFile = path.join(keyDirectory, 'api-key.json');
  assert.deepEqual(await readJSON(keyFile), { v: 1, api_key: secret });
  assert.equal(statMode(await stat(keyDirectory)), 0o700); assert.equal(statMode(await stat(keyFile)), 0o600);
  const saved = await readFile(path.join(store.directory, 'accounts.json'), 'utf8');
  assert.equal(saved.includes(secret), false); assert.equal(JSON.stringify(publicAccount).includes(secret), false);
  assert.equal(JSON.stringify(manager.publicJobs()).includes(secret), false);

  const restartedStore = new StateStore(store.directory, await loadState(store.directory));
  const restarted = new AccountManager(restartedStore, { codex: null, claude: null }, { deepSeekFetch: async (url, options) => { calls.push({ url, options }); return balanceResponse(cnyUsdBalance); } });
  t.after(() => restarted.close());
  assert.deepEqual(restarted.publicAccounts()[0].balance, cnyUsdBalance);
  await restarted.refresh(id);
  assert.equal(calls[1].options.headers.Authorization, `Bearer ${secret}`);
  assert.equal(JSON.stringify(restarted.publicAccounts()).includes(secret), false);

  await manager.remove(id);
  await assert.rejects(stat(keyFile), error => error.code === 'ENOENT');
});

function statMode(info) { return info.mode & 0o777; }

test('DeepSeek 401 retains cached balances, alias edits preserve them, and key replacement clears before verification', async t => {
  const store = await fixture(t); let requestCount = 0; let stateAtNewKeyRequest;
  const nextBalance = { is_available: false, balance_infos: [{ currency: 'USD', total_balance: '-0.25', granted_balance: '1.00', topped_up_balance: '0' }] };
  const manager = new AccountManager(store, { codex: null, claude: null }, { deepSeekFetch: async () => {
    requestCount += 1;
    if (requestCount === 1) return balanceResponse(cnyUsdBalance);
    if (requestCount === 2) return new Response('PRIVATE_PROVIDER_ERROR_BODY', { status: 403 });
    stateAtNewKeyRequest = { ...store.state.accounts[0], balance: store.state.accounts[0].balance && structuredClone(store.state.accounts[0].balance) };
    return balanceResponse(nextBalance);
  } });
  t.after(() => manager.close());
  const { account_id: id } = await manager.add('deepseek', { api_key: 'sk-old-key', label: 'Old wallet' });
  const account = store.state.accounts[0]; const previousObservedAt = account.observed_at;
  await manager.refresh(id);
  assert.equal(account.status, 'expired'); assert.equal(account.authenticated, true); assert.equal(account.observed_at, previousObservedAt);
  assert.deepEqual(account.balance, cnyUsdBalance);
  assert.equal(JSON.stringify(manager.publicAccounts()).includes('PRIVATE_PROVIDER_ERROR_BODY'), false);

  await manager.updateDeepSeekApiKey(id, { label: 'Renamed wallet' });
  assert.equal(requestCount, 2); assert.equal(account.authenticated, true); assert.equal(account.status, 'expired');
  assert.equal(account.observed_at, previousObservedAt); assert.deepEqual(account.balance, cnyUsdBalance);

  await manager.updateDeepSeekApiKey(id, { api_key: 'sk-new-key', label: 'New wallet' });
  assert.equal(stateAtNewKeyRequest.balance, null); assert.equal(stateAtNewKeyRequest.authenticated, false);
  assert.equal(stateAtNewKeyRequest.observed_at, null); assert.equal(stateAtNewKeyRequest.label, 'New wallet');
  assert.equal(account.status, 'ok'); assert.equal(account.authenticated, true); assert.deepEqual(account.balance, nextBalance);
  assert.equal(await readJSON(path.join(store.profile(id), 'deepseek', 'api-key.json')).then(file => file.api_key), 'sk-new-key');
  const saved = await readFile(path.join(store.directory, 'accounts.json'), 'utf8');
  assert.equal(saved.includes('sk-old-key'), false); assert.equal(saved.includes('sk-new-key'), false);
  assert.equal(JSON.stringify(manager.publicAccounts()).includes('sk-new-key'), false);
  await manager.remove(id);
});

test('DeepSeek first network failure recovers automatically and never starts Claude login', async t => {
  const store = await fixture(t); let online = false; let claudeRuns = 0;
  const manager = new AccountManager(store, { codex: null, claude: '/fake/claude' }, {
    claudeRun: async () => { claudeRuns += 1; throw new Error('must not be called'); },
    deepSeekFetch: async () => online ? balanceResponse(cnyUsdBalance) : new Response('upstream unavailable', { status: 503 }),
  });
  t.after(() => manager.close());
  const { account_id: id } = await manager.add('deepseek', { api_key: 'sk-recover-test' });
  assert.equal(manager.publicAccounts()[0].status, 'error'); assert.equal(manager.publicAccounts()[0].authenticated, false);
  await assert.rejects(manager.login(id), /unsupported_account/); assert.equal(claudeRuns, 0);

  online = true;
  manager.schedule();
  const retry = manager.refreshes.get(id);
  assert.ok(retry);
  await retry;
  assert.equal(manager.publicAccounts()[0].status, 'ok'); assert.equal(manager.publicAccounts()[0].authenticated, true);
  assert.deepEqual(manager.publicAccounts()[0].balance, cnyUsdBalance);
});

test('concurrent DeepSeek account creation cannot exceed the account cap', async t => {
  const store = await fixture(t);
  store.state.accounts.push(...Array.from({ length: 7 }, (_, index) => ({ id: String(index + 1).repeat(32), provider: 'codex', authenticated: false, status: 'waiting' })));
  const manager = new AccountManager(store, { codex: null, claude: null }, { deepSeekFetch: async () => balanceResponse(cnyUsdBalance) });
  t.after(() => manager.close());
  const results = await Promise.allSettled([
    manager.add('deepseek', { api_key: 'sk-concurrent-a' }),
    manager.add('deepseek', { api_key: 'sk-concurrent-b' }),
  ]);
  assert.equal(store.state.accounts.length, 8);
  assert.equal(results.filter(result => result.status === 'fulfilled').length, 1);
  assert.equal(results.filter(result => result.status === 'rejected' && result.reason.message === 'account_limit').length, 1);
});
test('subscription login uses unique owned profiles and device-code contract', async t => {
  const store = await fixture(t); const clients = [];
  const manager = new AccountManager(store, { codex: '/fake/codex', claude: null }, { codexFactory: (_, directory) => { const client = new FakeCodex(directory); clients.push(client); return client; } });
  t.after(() => manager.close());
  const first = await manager.add('codex'); const second = await manager.add('codex');
  assert.notEqual(first.account_id, second.account_id); assert.notEqual(clients[0].directory, clients[1].directory);
  assert.equal(clients[0].directory.startsWith(store.directory), true);
  assert.deepEqual(clients[0].calls[0], { method: 'account/login/start', params: { type: 'chatgptDeviceCode' } });
  await manager.refresh(first.account_id);
  assert.equal(manager.publicAccounts()[0].five_hour.remaining_percent, 75);
  assert.equal(manager.publicAccounts()[0].authenticated, true);
  assert.equal(JSON.stringify(manager.publicAccounts()).includes('/fake/codex'), false);
});
test('a verified account poll completes a new Codex login without a completion notification', async t => {
  const store = await fixture(t); const clients = [];
  const manager = new AccountManager(store, { codex: '/fake/codex' }, { codexFactory: (_, directory) => { const client = new FakeCodex(directory); clients.push(client); return client; } }); t.after(() => manager.close());
  const { account_id: id } = await manager.add('codex');
  assert.equal(manager.publicJob(id).status, 'pending');
  await manager.refresh(id);
  assert.equal(manager.publicJob(id).status, 'complete');
  assert.equal(clients[0].calls.some(call => call.method === 'account/read'), true);
});
test('an unchanged authenticated Codex identity cannot complete a re-login', async t => {
  const store = await fixture(t); const id = 'a'.repeat(32);
  store.state.accounts.push({ id, provider: 'codex', email: 'same@example.com', authenticated: true, status: 'ok', observed_at: 1000, five_hour: null, seven_day: null });
  const manager = new AccountManager(store, { codex: '/fake/codex' }, { codexFactory: (_, directory) => {
    const client = new FakeCodex(directory); const request = client.request.bind(client);
    client.request = (method, params) => method === 'account/read' ? { account: { type: 'chatgpt', email: 'same@example.com', planType: 'Plus' } } : request(method, params);
    return client;
  } }); t.after(() => manager.close());
  await manager.login(id); assert.equal(manager.publicJob(id).status, 'pending');
  await manager.refresh(id);
  assert.equal(manager.publicJob(id).status, 'pending');
});
test('a changed authenticated Codex identity completes a re-login', async t => {
  const store = await fixture(t); const id = 'b'.repeat(32);
  store.state.accounts.push({ id, provider: 'codex', email: 'old@example.com', authenticated: true, status: 'ok', observed_at: 1000, five_hour: null, seven_day: null });
  const manager = new AccountManager(store, { codex: '/fake/codex' }, { codexFactory: (_, directory) => {
    const client = new FakeCodex(directory); const request = client.request.bind(client);
    client.request = (method, params) => method === 'account/read' ? { account: { type: 'chatgpt', email: 'new@example.com', planType: 'Plus' } } : request(method, params);
    return client;
  } }); t.after(() => manager.close());
  await manager.login(id); assert.equal(manager.publicJob(id).status, 'pending');
  await manager.refresh(id);
  assert.equal(manager.publicJob(id).status, 'complete');
  assert.equal(manager.publicAccounts()[0].email, 'new@example.com');
});
test('Claude callback timer and companion polls do not manufacture fresh timestamps', async t => {
  const store = await fixture(t);
  const id = 'c'.repeat(32);
  store.state.accounts.push({ id, provider: 'claude', email: '', plan: '', status: 'waiting', authenticated: false, observed_at: null, five_hour: null, seven_day: null });
  const manager = new AccountManager(store, { codex: null, claude: '/fake/claude' }, { claudeRun: async () => ({ loggedIn: true, authMethod: 'claude.ai', email: 'claude@example.com', subscriptionType: 'Pro' }) });
  const directory = await manager.prepare(store.state.accounts[0]);
  const payload = { session_id: 'safe-session', cost: { total_api_duration_ms: 100, total_cost_usd: 1 }, rate_limits: { five_hour: { used_percentage: 20, resets_at: 2000000000 }, seven_day: { used_percentage: 40, resets_at: 2000000300 } } };
  await recordClaudeFeed(directory, payload, 1000, 'claude@example.com'); await recordClaudeFeed(directory, payload, 2000, 'claude@example.com');
  await manager.refresh(id); assert.equal(manager.publicAccounts()[0].observed_at, 1000);
  await manager.refresh(id); assert.equal(manager.publicAccounts()[0].observed_at, 1000);
  await recordClaudeFeed(directory, { ...payload, cost: { ...payload.cost, total_api_duration_ms: 200 } }, 3000, 'claude@example.com');
  assert.equal((await readJSON(path.join(directory, 'quota-snapshot.json'))).observed_at, 3000);
  const settings = await readJSON(path.join(directory, 'settings.json'));
  assert.equal(settings.statusLine.command.includes('claude-feed.mjs'), true);
});
test('provider auth environments are isolated without changing HOME', () => {
  const env = profileEnvironment('claude', '/owned/profile');
  assert.equal(env.CLAUDE_CONFIG_DIR, '/owned/profile'); assert.equal(env.ANTHROPIC_CONFIG_DIR, '/owned/profile/anthropic'); assert.equal(env.HOME, process.env.HOME);
  assert.equal(Object.hasOwn(env, 'ANTHROPIC_API_KEY'), false); assert.equal(Object.hasOwn(env, 'CLAUDE_CODE_OAUTH_TOKEN'), false);
  assert.equal(profileEnvironment('codex', '/owned/codex').CODEX_HOME, '/owned/codex');
  assert.equal(officialLoginURL('https://evil.example/claude.ai', 'claude'), null); assert.equal(officialLoginURL('https://claude.ai.evil.example/', 'claude'), null);
  assert.equal(officialLoginURL('https://claude.ai/oauth/authorize', 'claude'), 'https://claude.ai/oauth/authorize');
  assert.equal(shellQuote("a'b"), "'a'\"'\"'b'");
});
test('changed Codex identity clears old quota even when the new request fails', async t => {
  const store = await fixture(t); const id = 'd'.repeat(32);
  const account = { id, provider: 'codex', email: 'old@example.com', authenticated: true, status: 'ok', observed_at: 1000, five_hour: { remaining_percent: 75, resets_at: null }, seven_day: null }; store.state.accounts.push(account);
  const client = new FakeCodex(store.profile(id)); client.request = async method => { if (method === 'account/read') return { account: { type: 'chatgpt', email: 'new@example.com', planType: 'Plus' } }; throw new Error('offline'); };
  const manager = new AccountManager(store, { codex: '/fake/codex' }, { codexFactory: () => client }); t.after(() => manager.close()); await manager.refresh(id);
  assert.equal(account.email, 'new@example.com'); assert.equal(account.five_hour, null); assert.equal(account.observed_at, null); assert.equal(account.status, 'error');
});
test('Claude rejects an old running session feed after a different account logs in', async t => {
  const store = await fixture(t); const id = 'e'.repeat(32); const account = { id, provider: 'claude', email: 'old@example.com', authenticated: true, status: 'ok', observed_at: 1000, five_hour: { remaining_percent: 50, resets_at: null }, seven_day: null }; store.state.accounts.push(account);
  const manager = new AccountManager(store, { claude: '/fake/claude' }, { claudeRun: async () => ({ loggedIn: true, authMethod: 'claude.ai', email: 'new@example.com', subscriptionType: 'Pro' }) }); t.after(() => manager.close()); const directory = await manager.prepare(account);
  const payload = { rate_limits: { five_hour: { used_percentage: 50 } } };
  await recordClaudeFeed(directory, payload, 1000, 'old@example.com'); await manager.refresh(id);
  assert.equal(account.email, 'new@example.com'); assert.equal(account.five_hour, null); assert.equal(account.status, 'waiting');
  await recordClaudeFeed(directory, payload, 2000, 'old@example.com'); await manager.refresh(id);
  assert.equal(account.five_hour, null); assert.equal(account.observed_at, null); assert.equal((await manager.launchCommand(id)).command.includes('claude-session.mjs'), true);
});
test('login and refresh share a client; deletion detaches before pending refresh finishes', async t => {
  const store = await fixture(t); const id = 'f'.repeat(32); store.state.accounts.push({ id, provider: 'codex', email: '', authenticated: false, status: 'waiting' });
  let count = 0; let resolveIdentity; const identity = new Promise(resolve => { resolveIdentity = resolve; });
  const manager = new AccountManager(store, { codex: '/fake/codex' }, { codexFactory: (_, directory) => { count++; const client = new FakeCodex(directory); const original = client.request.bind(client); client.request = (method, params) => method === 'account/read' ? identity : original(method, params); return client; } }); t.after(() => manager.close());
  const login = manager.login(id); const refresh = manager.refresh(id); await login; assert.equal(count, 1);
  const removal = manager.remove(id); assert.throws(() => manager.account(id), /account_not_found/);
  resolveIdentity({ account: { type: 'chatgpt', email: 'late@example.com' } }); await Promise.all([refresh, removal]); assert.equal(manager.publicAccounts().length, 0); assert.equal(manager.clients.size, 0);
});
test('disconnected or timed out device-code login can retry', async t => {
  const store = await fixture(t); const clients = [];
  const manager = new AccountManager(store, { codex: '/fake/codex' }, { loginTimeout: 20, codexFactory: (_, directory) => { const client = new FakeCodex(directory); clients.push(client); return client; } }); t.after(() => manager.close());
  const { account_id: id } = await manager.add('codex'); clients[0].emit('disconnected'); assert.equal(manager.publicJob(id).status, 'error'); assert.equal(manager.publicJob(id).code, null);
  await manager.login(id); assert.equal(manager.publicJob(id).status, 'pending'); await new Promise(resolve => setTimeout(resolve, 35)); assert.equal(manager.publicJob(id).error, 'login_timeout'); await manager.login(id); assert.equal(manager.publicJob(id).status, 'pending');
});
test('alternating idle Claude sessions keep source times and cannot regress the latest quota', async t => {
  const store = await fixture(t); const directory = path.join(store.directory, 'feed');
  const first = { session_id: 'first', rate_limits: { five_hour: { used_percentage: 20 } }, cost: { total_api_duration_ms: 100 } };
  const second = { session_id: 'second', rate_limits: { five_hour: { used_percentage: 40 } }, cost: { total_api_duration_ms: 200 } };
  await recordClaudeFeed(directory, first, 1000, 'one@example.com'); await recordClaudeFeed(directory, second, 2000, 'one@example.com');
  await Promise.all([recordClaudeFeed(directory, first, 3000, 'one@example.com'), recordClaudeFeed(directory, second, 4000, 'one@example.com')]);
  const cached = await readJSON(path.join(directory, 'quota-snapshot.json'));
  assert.equal(cached.observed_at, 2000); assert.equal(cached.five_hour.remaining_percent, 60);
});
test('unchanged Claude callbacks cannot overwrite a second session observed in the same second', async t => {
  const store = await fixture(t); const directory = path.join(store.directory, 'feed');
  const first = { session_id: 'same-second-first', rate_limits: { five_hour: { used_percentage: 20 } } };
  const second = { session_id: 'same-second-second', rate_limits: { five_hour: { used_percentage: 40 } } };
  await recordClaudeFeed(directory, first, 1000, 'one@example.com'); await recordClaudeFeed(directory, second, 1000, 'one@example.com');
  await recordClaudeFeed(directory, first, 2000, 'one@example.com');
  const cached = await readJSON(path.join(directory, 'quota-snapshot.json')); assert.equal(cached.observed_at, 1000); assert.equal(cached.five_hour.remaining_percent, 60);
});
