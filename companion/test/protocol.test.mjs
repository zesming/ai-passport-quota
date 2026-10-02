import test from 'node:test';
import assert from 'node:assert/strict';
import { codexWindows, claudeWindows, makeSnapshot, safeText, privateIPv4, constantToken } from '../server/protocol.mjs';

test('Codex maps duration, not primary/secondary position, and chooses Codex bucket', () => {
  const result = codexWindows({ rateLimits: { primary: { usedPercent: 99, windowDurationMins: 300 } }, rateLimitsByLimitId: { other: { primary: { usedPercent: 99, windowDurationMins: 300 } }, codex: { primary: { usedPercent: 58, windowDurationMins: 10080, resetsAt: 2000 }, secondary: { usedPercent: 32, windowDurationMins: 300, resetsAt: 1000 } } } });
  assert.deepEqual(result, { five_hour: { remaining_percent: 68, resets_at: 1000 }, seven_day: { remaining_percent: 42, resets_at: 2000 } });
});
test('missing duration and malformed values remain unknown; zero is real exhaustion', () => {
  assert.deepEqual(codexWindows({ rateLimits: { primary: { usedPercent: 1 }, secondary: { usedPercent: 'bad', windowDurationMins: 300 } } }), { five_hour: null, seven_day: null });
  assert.deepEqual(claudeWindows({ rate_limits: { five_hour: { used_percentage: 100, resets_at: 2000 } } }), { five_hour: { remaining_percent: 0, resets_at: 2000 }, seven_day: null });
});
test('device snapshots are whitelisted and exclude pending identities and internal metadata', () => {
  const state = { revision: 3, settings: { refresh_seconds: 300, auto_refresh: true }, accounts: [{ id: 'a'.repeat(32), provider: 'codex', email: 'one@example.com', authenticated: true, plan: 'Plus', status: 'ok', observed_at: 2000, token: 'SECRET_NEVER_SENT', profile: '/private/path', five_hour: null, seven_day: null }, { id: 'b'.repeat(32), provider: 'claude', authenticated: false }] };
  const snapshot = makeSnapshot(state, 3000);
  assert.equal(snapshot.accounts.length, 1); assert.equal(snapshot.server_time, 3000);
  assert.equal(JSON.stringify(snapshot).includes('SECRET'), false); assert.equal(JSON.stringify(snapshot).includes('/private/path'), false);
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
