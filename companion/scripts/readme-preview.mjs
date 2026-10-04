// Documentation-only preview: synthetic accounts and device indicators; no real providers, profiles, USB or LAN listener.
import { mkdtemp, rm } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { createApplication } from '../server/index.mjs';

const now = Math.floor(Date.now() / 1000);
const accounts = [
  { id: '1'.repeat(32), provider: 'codex', email: 'codex@example.com', plan: 'Plus', authenticated: true, status: 'ok', observed_at: now, five_hour: { remaining_percent: 16, resets_at: now + 7200 }, seven_day: { remaining_percent: 42, resets_at: now + 172800 }, banked_reset: { available_count: 2, next_expires_at: now + 442800 }, credits: { has_credits: true, unlimited: false, balance: '45000' } },
  { id: '2'.repeat(32), provider: 'claude', email: 'claude@example.com', plan: 'Pro', authenticated: true, status: 'ok', observed_at: now, five_hour: { remaining_percent: 68, resets_at: now + 10800 }, seven_day: { remaining_percent: 72, resets_at: now + 259200 } },
  { id: '3'.repeat(32), provider: 'deepseek', email: '', plan: 'API', label: 'DeepSeek API', authenticated: true, status: 'ok', observed_at: now, balance: { is_available: true, balance_infos: [{ currency: 'CNY', total_balance: '128.50', granted_balance: '10.00', topped_up_balance: '118.50' }] }, five_hour: null, seven_day: null },
  { id: '4'.repeat(32), provider: 'codex', email: 'pro@example.com', plan: 'pro', authenticated: true, status: 'ok', observed_at: now, five_hour: null, seven_day: { remaining_percent: 82, resets_at: now + 172800 }, banked_reset: { available_count: 3, next_expires_at: now + 442800 }, credits: { has_credits: true, unlimited: false, balance: '12500' } },
];
const directory = await mkdtemp(path.join(os.tmpdir(), 'aiq-readme-preview-'));
const application = await createApplication({
  directory, adminPort: 4327, autoRestore: false, interfaces: () => [],
  previewStatus: { wifi_connected: true, battery_percent: 76 },
  executables: { codex: 'documentation-only', claude: 'documentation-only' },
  managerFactory: () => ({
    publicAccounts: () => accounts, publicJobs: () => [], schedule() {}, close() {},
    add() { throw new Error('unsupported_account'); },
    login() { throw new Error('unsupported_account'); },
    loginCode() { throw new Error('unsupported_account'); },
    remove() { throw new Error('unsupported_account'); },
    account() { throw new Error('unsupported_account'); },
    updateDeepSeekApiKey() { throw new Error('unsupported_account'); },
    launchCommand() { throw new Error('unsupported_account'); },
  }),
});
let stopping = false;
const stop = async () => {
  if (stopping) return;
  stopping = true;
  await application.close();
  await rm(directory, { recursive: true, force: true });
};
process.once('SIGINT', () => stop().then(() => process.exit(0)));
process.once('SIGTERM', () => stop().then(() => process.exit(0)));
try {
  await application.listen();
  console.log('Documentation preview (synthetic accounts only): http://127.0.0.1:4327/');
} catch (error) {
  await stop();
  throw error;
}
