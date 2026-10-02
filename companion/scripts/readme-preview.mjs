// Documentation-only preview: no real providers, profiles, USB or LAN listener.
import { mkdtemp, rm } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { createApplication } from '../server/index.mjs';

const now = Math.floor(Date.now() / 1000);
const accounts = [
  { id: '1'.repeat(32), provider: 'codex', email: 'codex@example.com', plan: 'Plus', authenticated: true, status: 'ok', observed_at: now, five_hour: { remaining_percent: 16, resets_at: now + 7200 }, seven_day: { remaining_percent: 42, resets_at: now + 172800 } },
  { id: '2'.repeat(32), provider: 'claude', email: 'claude@example.com', plan: 'Pro', authenticated: true, status: 'ok', observed_at: now, five_hour: { remaining_percent: 68, resets_at: now + 10800 }, seven_day: { remaining_percent: 72, resets_at: now + 259200 } },
];
const directory = await mkdtemp(path.join(os.tmpdir(), 'aiq-readme-preview-'));
const application = await createApplication({
  directory, adminPort: 4327, autoRestore: false, interfaces: () => [],
  executables: { codex: 'documentation-only', claude: 'documentation-only' },
  managerFactory: () => ({
    publicAccounts: () => accounts, publicJobs: () => [], schedule() {}, close() {},
    add() { throw new Error('unsupported_account'); },
    login() { throw new Error('unsupported_account'); },
    loginCode() { throw new Error('unsupported_account'); },
    remove() { throw new Error('unsupported_account'); },
    account() { throw new Error('unsupported_account'); },
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
