import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { loadState, StateStore } from '../server/storage.mjs';
import { makeSnapshot } from '../server/protocol.mjs';

async function fixture(t) {
  const directory = await mkdtemp(path.join(os.tmpdir(), 'aiq-settings-migration-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  return directory;
}

test('missing and legacy screen timeout settings load with the two-minute default', async t => {
  const freshDirectory = await fixture(t);
  const fresh = await loadState(freshDirectory);
  assert.deepEqual(fresh.settings, { refresh_seconds: 300, auto_refresh: true, screen_timeout_seconds: 120 });

  const legacyDirectory = await fixture(t);
  await writeFile(path.join(legacyDirectory, 'accounts.json'), JSON.stringify({
    v: 1,
    revision: 4,
    settings: { refresh_seconds: 900, auto_refresh: false },
    accounts: [],
  }));
  const legacy = await loadState(legacyDirectory);
  assert.deepEqual(legacy.settings, { refresh_seconds: 900, auto_refresh: false, screen_timeout_seconds: 120 });
});

test('persisted screen timeout rejects unsupported values', async t => {
  const directory = await fixture(t);
  await writeFile(path.join(directory, 'accounts.json'), JSON.stringify({
    v: 1,
    revision: 1,
    settings: { refresh_seconds: 300, auto_refresh: true, screen_timeout_seconds: 90 },
    accounts: [],
  }));
  await assert.rejects(loadState(directory), /local_state_invalid/);
});


test('DeepSeek state reload accepts a balance and alias without persisting credentials', async t => {
  const directory = await fixture(t);
  const balance = { is_available: true, balance_infos: [{ currency: 'CNY', total_balance: '0', granted_balance: '0', topped_up_balance: '0' }] };
  await writeFile(path.join(directory, 'accounts.json'), JSON.stringify({
    v: 1, revision: 2, settings: { refresh_seconds: 300, auto_refresh: true },
    accounts: [{ id: 'd'.repeat(32), provider: 'deepseek', email: '', plan: 'API', label: '备用 API', balance, status: 'ok', observed_at: 1700000000, authenticated: true }],
  }));
  const state = await loadState(directory);
  assert.equal(state.accounts[0].provider, 'deepseek');
  assert.equal(state.accounts[0].label, '备用 API');
  assert.deepEqual(state.accounts[0].balance, balance);
  assert.equal(state.accounts[0].authenticated, true);
  assert.equal(Object.hasOwn(state.accounts[0], 'api_key'), false);
});
test('Codex extras survive state reload with only display fields and unknown legacy defaults', async t => {
  const directory = await fixture(t);
  const credits = { has_credits: true, unlimited: false, balance: '0.0000000000000100', token: 'PRIVATE_CREDIT_TOKEN' };
  const banked_reset = { available_count: 0, credits: [{ id: 'PRIVATE_RESET_ID' }] };
  const base = { email: 'fixture@example.com', plan: 'Plus', status: 'ok', observed_at: 1700000000, authenticated: true };
  const store = new StateStore(directory, { v: 1, revision: 2, settings: { refresh_seconds: 300, auto_refresh: true }, accounts: [
    { ...base, id: 'a'.repeat(32), provider: 'codex', credits, banked_reset, token: 'PRIVATE_ACCOUNT_TOKEN' },
    { ...base, id: 'b'.repeat(32), provider: 'codex' },
    { ...base, id: 'c'.repeat(32), provider: 'claude', credits, banked_reset },
  ] });
  await store.save();
  const state = await loadState(directory);
  assert.deepEqual(state.accounts[0].credits, { has_credits: true, unlimited: false, balance: '0.0000000000000100' });
  assert.deepEqual(state.accounts[0].banked_reset, { available_count: 0 });
  assert.equal(state.accounts[1].credits, null); assert.equal(state.accounts[1].banked_reset, null);
  assert.equal(Object.hasOwn(state.accounts[2], 'credits'), false); assert.equal(Object.hasOwn(state.accounts[2], 'banked_reset'), false);
  assert.equal(JSON.stringify(state).includes('PRIVATE_'), false);
  assert.equal(JSON.stringify(makeSnapshot(state, 1700000010)).includes('PRIVATE_'), false);

  store.state.accounts[0].credits.balance = '1'.repeat(33);
  store.state.accounts[0].banked_reset.available_count = -1;
  await store.save();
  const invalid = await loadState(directory);
  assert.equal(invalid.accounts[0].credits.balance, null); assert.equal(invalid.accounts[0].banked_reset, null);
});
