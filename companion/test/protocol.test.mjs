import test from 'node:test';
import assert from 'node:assert/strict';
import { codexWindows, claudeWindows, makeSnapshot, safeText, privateIPv4, constantToken, validSettings, SCREEN_TIMEOUT_SECONDS, DEFAULT_SCREEN_TIMEOUT_SECONDS } from '../server/protocol.mjs';

test('Codex maps duration, not primary/secondary position, and chooses Codex bucket', () => {
  const result = codexWindows({ rateLimits: { primary: { usedPercent: 99, windowDurationMins: 300 } }, rateLimitsByLimitId: { other: { primary: { usedPercent: 99, windowDurationMins: 300 } }, codex: { primary: { usedPercent: 58, windowDurationMins: 10080, resetsAt: 2000 }, secondary: { usedPercent: 32, windowDurationMins: 300, resetsAt: 1000 } } } });
  assert.deepEqual(result, { five_hour: { remaining_percent: 68, resets_at: 1000 }, seven_day: { remaining_percent: 42, resets_at: 2000 }, credits: null, banked_reset: null });
});
test('missing duration and malformed values remain unknown; zero is real exhaustion', () => {
  assert.deepEqual(codexWindows({ rateLimits: { primary: { usedPercent: 1 }, secondary: { usedPercent: 'bad', windowDurationMins: 300 } } }), { five_hour: null, seven_day: null, credits: null, banked_reset: null });
  assert.deepEqual(claudeWindows({ rate_limits: { five_hour: { used_percentage: 100, resets_at: 2000 } } }), { five_hour: { remaining_percent: 0, resets_at: 2000 }, seven_day: null });
});
test('Codex extras use the selected bucket and authoritative reset count without inventing a missing window', () => {
  const response = {
    rateLimits: { credits: { hasCredits: true, unlimited: false, balance: '999' } },
    rateLimitsByLimitId: {
      other: { credits: { hasCredits: true, unlimited: false, balance: '888' } },
      codex: { secondary: { usedPercent: 100, windowDurationMins: 10080, resetsAt: 2000 }, credits: { hasCredits: true, unlimited: false, balance: '0.00000000000000000100' } },
    },
    rateLimitResetCredits: { availableCount: 3, credits: [] },
  };
  assert.deepEqual(codexWindows(response), {
    five_hour: null, seven_day: { remaining_percent: 0, resets_at: 2000 },
    credits: { has_credits: true, unlimited: false, balance: '0.00000000000000000100' }, banked_reset: { available_count: 3, next_expires_at: null },
  });
  assert.deepEqual(codexWindows({ rateLimits: {}, rateLimitResetCredits: { availableCount: 0, credits: null } }).banked_reset, { available_count: 0, next_expires_at: null });
  for (const availableCount of [null, -1, 1.5, '2', Number.MAX_SAFE_INTEGER + 1]) {
    assert.equal(codexWindows({ rateLimitResetCredits: { availableCount } }).banked_reset, null);
  }
});
test('banked reset expiry uses only complete, known available Codex reset details', () => {
  const reset = rateLimitResetCredits => codexWindows({ rateLimitResetCredits }).banked_reset;
  const available = (expiresAt, extra = {}) => ({ status: 'available', resetType: 'codexRateLimits', expiresAt, ...extra });
  const complete = reset({ availableCount: 3, credits: [
    available(1900, { id: 'PRIVATE_RESET_ID_1', title: 'Private title', description: 'Private description' }),
    available(null, { id: 'PRIVATE_RESET_ID_3' }),
    available(1700, { id: 'PRIVATE_RESET_ID_4' }),
  ] });
  assert.deepEqual(complete, { available_count: 3, next_expires_at: 1700 });
  assert.equal(JSON.stringify(complete).includes('PRIVATE_'), false);
  assert.deepEqual(reset({ availableCount: 2, credits: [available(1700), available(null)] }), { available_count: 2, next_expires_at: 1700 });
  assert.deepEqual(reset({ availableCount: 2, credits: [available(null), available(null)] }), { available_count: 2, next_expires_at: null });
  assert.deepEqual(reset({ availableCount: 1, credits: [available(1500)] }), { available_count: 1, next_expires_at: 1500 });

  // Count-only, capped, missing or ambiguous detail rows preserve the count but never guess a deadline.
  for (const [availableCount, details] of [
    [1, null],
    [1, []],
    [2, [available(1800)]],
    [1, [available(1800), available(1900)]],
    [1, [{ status: 'redeemed', resetType: 'codexRateLimits', expiresAt: 1800 }]],
    [1, [{ status: 'unknown', resetType: 'codexRateLimits', expiresAt: 1800 }]],
    [1, [{ resetType: 'codexRateLimits', expiresAt: 1800 }]],
    [1, [available(1800, { resetType: 'unknown' })]],
    [1, [available(undefined)]],
    [1, [available(0)]],
    [1, [available(-1)]],
    [1, [available(1.5)]],
    [1, [available(Number.MAX_SAFE_INTEGER + 1)]],
    [1, [null]],
  ]) {
    assert.deepEqual(reset({ availableCount, credits: details }), { available_count: availableCount, next_expires_at: null });
  }
});
test('Codex credits require available flags and preserve only complete bounded display strings', () => {
  const read = credits => codexWindows({ rateLimits: { credits } }).credits;
  assert.equal(read({ hasCredits: false, unlimited: false, balance: '0' }), null);
  assert.equal(read({ hasCredits: true, balance: '1' }), null);
  assert.equal(read({ hasCredits: 'true', unlimited: false, balance: '1' }), null);
  assert.deepEqual(read({ hasCredits: false, unlimited: true }), { has_credits: false, unlimited: true, balance: null });
  const amount = '1'.repeat(32);
  assert.equal(read({ hasCredits: true, unlimited: false, balance: amount }).balance, amount);
  assert.equal(read({ hasCredits: true, unlimited: false, balance: '中'.repeat(10) }).balance, '中'.repeat(10));
  for (const balance of [null, 123, '', '1\n2', '1'.repeat(33), '中'.repeat(11)]) {
    assert.equal(read({ hasCredits: true, unlimited: false, balance }).balance, null);
  }
});
test('screen timeout accepts only supported values and stays optional for legacy settings', () => {
  const legacy = { refresh_seconds: 300, auto_refresh: true };
  assert.equal(validSettings(legacy), true);
  for (const value of SCREEN_TIMEOUT_SECONDS) assert.equal(validSettings({ ...legacy, screen_timeout_seconds: value }), true);
  assert.equal(validSettings({ ...legacy, screen_timeout_seconds: 90 }), false);
  assert.equal(validSettings({ ...legacy, screen_timeout_seconds: null }), false);
  assert.equal(validSettings({ refresh_seconds: 300, screen_timeout_seconds: 120 }), false);
  assert.equal(DEFAULT_SCREEN_TIMEOUT_SECONDS, 120);
});
test('device snapshots are whitelisted and exclude pending identities and internal metadata', () => {
  const state = { revision: 3, settings: { refresh_seconds: 300, auto_refresh: true }, accounts: [{ id: 'a'.repeat(32), provider: 'codex', email: 'one@example.com', authenticated: true, plan: 'Plus', status: 'ok', observed_at: 2000, token: 'SECRET_NEVER_SENT', profile: '/private/path', five_hour: null, seven_day: null }, { id: 'b'.repeat(32), provider: 'claude', authenticated: false }] };
  const snapshot = makeSnapshot(state, 3000);
  assert.equal(snapshot.accounts.length, 1); assert.equal(snapshot.server_time, 3000);
  assert.deepEqual(snapshot.settings, { refresh_seconds: 300, auto_refresh: true, screen_timeout_seconds: DEFAULT_SCREEN_TIMEOUT_SECONDS });
  assert.equal(JSON.stringify(snapshot).includes('SECRET'), false); assert.equal(JSON.stringify(snapshot).includes('/private/path'), false);
});
test('DeepSeek public snapshots keep the local alias and raw currency balances without API keys or quota windows', () => {
  const balance = { is_available: true, balance_infos: [
    { currency: 'CNY', total_balance: '-0.50', granted_balance: '0', topped_up_balance: '128.50' },
    { currency: 'USD', total_balance: '17.25', granted_balance: '2.00', topped_up_balance: '15.25' },
  ] };
  const snapshot = makeSnapshot({ revision: 7, settings: { refresh_seconds: 300, auto_refresh: true }, accounts: [{
    id: 'c'.repeat(32), provider: 'deepseek', email: 'must-not-leak@example.com', plan: 'unknown', label: '工作 API', status: 'ok', observed_at: 2000,
    authenticated: true, balance, five_hour: { remaining_percent: 30 }, seven_day: { remaining_percent: 20 }, api_key: 'SECRET_API_KEY',
  }] }, 3000);
  assert.deepEqual(snapshot.accounts[0], {
    id: 'c'.repeat(32), provider: 'deepseek', email: '', plan: 'API', status: 'ok', observed_at: 2000,
    five_hour: null, seven_day: null, label: '工作 API', balance,
  });
  assert.equal(JSON.stringify(snapshot).includes('SECRET_API_KEY'), false);
  assert.equal(JSON.stringify(snapshot).includes('must-not-leak'), false);
});
test('UTF-8 bounds preserve complete characters and remove control text', () => {
  assert.equal(safeText('中中\nabc', 7), '中中a');
  assert.equal(Buffer.byteLength(safeText('😀'.repeat(40), 128)), 128);
});
test('pairing accepts private IPv4 and exact tokens only', () => {
  for (const value of ['10.0.1.2', '172.16.0.1', '172.31.255.1', '192.168.1.2']) assert.equal(privateIPv4(value), true);
  for (const value of ['127.0.0.1', '8.8.8.8', '172.32.0.1', '192.168.1.256', 'localhost', '192.168.1.2:4318']) assert.equal(privateIPv4(value), false);
  assert.equal(constantToken('Bearer abc', 'Bearer abc'), true); assert.equal(constantToken('Bearer abc', 'Bearer abd'), false); assert.equal(constantToken(undefined, 'secret'), false);
});
