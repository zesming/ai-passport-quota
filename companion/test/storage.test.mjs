import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { loadState } from '../server/storage.mjs';

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
