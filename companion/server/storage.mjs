import { mkdir, readFile, writeFile, rename, chmod } from 'node:fs/promises';
import path from 'node:path';
import { randomBytes } from 'node:crypto';
import { MAX_ACCOUNTS, validId, validSettings, publicAccount } from './protocol.mjs';

export async function privateDirectory(directory) {
  await mkdir(directory, { recursive: true, mode: 0o700 });
  await chmod(directory, 0o700);
}
export async function atomicJSON(filename, data) {
  await privateDirectory(path.dirname(filename));
  const temporary = `${filename}.${randomBytes(6).toString('hex')}.tmp`;
  await writeFile(temporary, `${JSON.stringify(data)}\n`, { mode: 0o600, flag: 'wx' });
  await rename(temporary, filename);
  await chmod(filename, 0o600);
}
export async function readJSON(filename) {
  try { return JSON.parse(await readFile(filename, 'utf8')); }
  catch (error) { if (error.code === 'ENOENT') return null; throw new Error('local_state_invalid'); }
}
export async function loadState(directory) {
  await privateDirectory(directory);
  const saved = await readJSON(path.join(directory, 'accounts.json'));
  const state = saved ?? { v: 1, revision: 1, settings: { refresh_seconds: 300, auto_refresh: true }, accounts: [] };
  if (state.v !== 1 || !validSettings(state.settings) || !Array.isArray(state.accounts) || state.accounts.length > MAX_ACCOUNTS || !Number.isSafeInteger(state.revision) || state.accounts.some(account => !validId(account.id) || !['codex', 'claude'].includes(account.provider)) || new Set(state.accounts.map(account => account.id)).size !== state.accounts.length) throw new Error('local_state_invalid');
  state.accounts = state.accounts.map(account => ({ ...publicAccount(account), authenticated: account.authenticated === true }));
  return state;
}
export class StateStore {
  constructor(directory, state) { this.directory = directory; this.state = state; this.pendingWrite = Promise.resolve(); }
  async save() {
    const copy = JSON.parse(JSON.stringify(this.state));
    this.pendingWrite = this.pendingWrite.catch(() => {}).then(() => atomicJSON(path.join(this.directory, 'accounts.json'), copy));
    await this.pendingWrite;
  }
  async changed() { this.state.revision += 1; await this.save(); }
  profile(id) { if (!validId(id)) throw new Error('invalid_account'); return path.join(this.directory, 'profiles', id); }
}
