import { spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { rm } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { CodexClient, claudeCommand, profileEnvironment, shellQuote, officialLoginURL } from './clients.mjs';
import { privateDirectory, atomicJSON, readJSON } from './storage.mjs';
import { MAX_ACCOUNTS, codexWindows, publicAccount, publicWindow, safeText, epoch } from './protocol.mjs';

const feedScript = fileURLToPath(new URL('./claude-feed.mjs', import.meta.url));
const sessionScript = fileURLToPath(new URL('./claude-session.mjs', import.meta.url));
export class AccountManager {
  constructor(store, executables, { codexFactory = (executable, directory) => new CodexClient(executable, directory), claudeRun = claudeCommand, loginTimeout = 5 * 60 * 1000 } = {}) {
    Object.assign(this, { store, executables, codexFactory, claudeRun, loginTimeout });
    this.clients = new Map(); this.clientOpenings = new Map(); this.jobs = new Map(); this.refreshes = new Map(); this.claudeLogins = new Map(); this.loginStarts = new Map(); this.closed = false;
  }
  live(account) { return !this.closed && this.store.state.accounts.includes(account); }
  account(id) { const account = this.store.state.accounts.find(item => item.id === id); if (!account || this.closed) throw new Error('account_not_found'); return account; }
  async prepare(account) {
    const directory = path.join(this.store.profile(account.id), account.provider);
    await privateDirectory(directory);
    if (account.provider === 'claude') {
      await privateDirectory(path.join(directory, 'anthropic'));
      const settingsFile = path.join(directory, 'settings.json');
      const settings = await readJSON(settingsFile) ?? {};
      settings.statusLine = { type: 'command', command: `${shellQuote(process.execPath)} ${shellQuote(feedScript)} ${shellQuote(directory)}`, refreshInterval: 30 };
      await atomicJSON(settingsFile, settings);
    }
    return directory;
  }
  finishJob(id, job, status, error = null) {
    if (this.jobs.get(id) !== job) return;
    clearTimeout(job.timer); job.status = status; job.error = error; job.url = null; job.code = null; job.requires_code = false;
  }
  finishCodexLoginFromIdentity(id, job, email) {
    const initial = job?.initialIdentity;
    const verifiedEmail = safeText(email, 128);
    if (!initial || this.jobs.get(id) !== job || job.status !== 'pending' || job.provider !== 'codex' || !verifiedEmail) return;
    const isNewIdentity = initial.authenticated !== true || (initial.email && initial.email !== verifiedEmail);
    if (isNewIdentity) this.finishJob(id, job, 'complete');
  }
  async codex(account) {
    if (!this.executables.codex) throw new Error('cli_unavailable');
    if (!this.live(account)) throw new Error('account_not_found');
    if (!this.clients.has(account.id) && !this.clientOpenings.has(account.id)) {
      const opening = (async () => {
        const directory = await this.prepare(account);
        if (!this.live(account)) throw new Error('account_not_found');
        const client = this.codexFactory(this.executables.codex, directory);
        client.on('notification', (method, params) => {
          if (!this.live(account) || this.clients.get(account.id) !== client) return;
          if (method === 'account/login/completed') {
            const job = this.jobs.get(account.id);
            if (job?.status === 'pending' && (!params.loginId || params.loginId === job.loginId)) {
              this.finishJob(account.id, job, params.success ? 'complete' : 'error', params.success ? null : 'login_failed');
              if (params.success) this.schedule(account.id);
            }
          }
          // Rate-limit notifications are sparse; reread the complete schema.
          if (['account/rateLimits/updated', 'account/updated'].includes(method) && account.authenticated && epoch() - (account.observed_at ?? 0) >= 10) this.schedule(account.id);
        });
        client.on('disconnected', () => {
          if (!this.live(account) || this.clients.get(account.id) !== client) return;
          const job = this.jobs.get(account.id);
          if (job?.status === 'pending') this.finishJob(account.id, job, 'error', 'login_failed');
        });
        this.clients.set(account.id, client);
        return client;
      })().finally(() => this.clientOpenings.delete(account.id));
      this.clientOpenings.set(account.id, opening);
    }
    const client = this.clients.get(account.id) ?? await this.clientOpenings.get(account.id);
    if (!this.live(account)) throw new Error('account_not_found');
    await client.open();
    if (!this.live(account)) throw new Error('account_not_found');
    return client;
  }
  async add(provider) {
    if (!['codex', 'claude'].includes(provider)) throw new Error('invalid_provider');
    if (!this.executables[provider]) throw new Error('cli_unavailable');
    if (this.store.state.accounts.length >= MAX_ACCOUNTS) throw new Error('account_limit');
    const account = { id: randomBytes(16).toString('hex'), provider, email: '', plan: '', status: 'waiting', observed_at: null, five_hour: null, seven_day: null, authenticated: false };
    this.store.state.accounts.push(account); await this.store.changed();
    await this.login(account.id);
    return { account_id: account.id, login: this.publicJob(account.id) };
  }
  publicJob(id) {
    const job = this.jobs.get(id); if (!job) return null;
    return { account_id: id, provider: job.provider, status: job.status, url: job.url ?? null, code: job.code ?? null, error: job.error ?? null, requires_code: job.requires_code === true };
  }
  login(id) {
    const account = this.account(id);
    if (this.loginStarts.has(id)) return this.loginStarts.get(id);
    if (this.jobs.get(id)?.status === 'pending') return Promise.resolve(this.publicJob(id));
    const start = this.startLogin(account).finally(() => this.loginStarts.delete(id));
    this.loginStarts.set(id, start); return start;
  }
  async startLogin(account) {
    const id = account.id; const job = { provider: account.provider, status: 'pending', url: null, code: null, error: null, initialIdentity: { authenticated: account.authenticated === true, email: safeText(account.email, 128) } };
    this.jobs.set(id, job);
    job.timer = setTimeout(() => {
      if (job.status !== 'pending') return;
      this.finishJob(id, job, 'error', 'login_timeout');
      this.claudeLogins.get(id)?.kill('SIGTERM');
      if (job.loginId) this.clients.get(id)?.request('account/login/cancel', { loginId: job.loginId }).catch(() => {});
    }, this.loginTimeout); job.timer.unref();
    try {
      if (account.provider === 'codex') {
        const client = await this.codex(account);
        const result = await client.request('account/login/start', { type: 'chatgptDeviceCode' });
        if (!this.live(account) || job.status !== 'pending') return this.publicJob(id);
        job.loginId = result.loginId; job.url = officialLoginURL(result.verificationUrl, 'codex'); job.code = safeText(result.userCode, 64);
        if (!job.url) throw new Error('login_url_invalid');
      } else {
        if (!this.executables.claude) throw new Error('cli_unavailable');
        const directory = await this.prepare(account);
        if (!this.live(account)) return null;
        const child = spawn(this.executables.claude, ['auth', 'login', '--claudeai'], { cwd: directory, env: profileEnvironment('claude', directory), stdio: ['pipe', 'pipe', 'pipe'] });
        this.claudeLogins.set(id, child); let buffer = '';
        const capture = chunk => {
          if (!this.live(account) || job.status !== 'pending') return;
          buffer = (buffer + chunk.toString('utf8')).slice(-32768);
          const plain = buffer.replace(/\x1b\[[0-9;]*m/g, '');
          for (const candidate of plain.match(/https:\/\/[^\s<>]+/g) ?? []) { const url = officialLoginURL(candidate, 'claude'); if (url) job.url = url; }
          if (/paste.*code|enter.*code/i.test(plain)) job.requires_code = true;
        };
        child.stdout.on('data', capture); child.stderr.on('data', capture);
        const finish = code => {
          buffer = ''; if (this.claudeLogins.get(id) === child) this.claudeLogins.delete(id);
          if (!this.live(account) || this.jobs.get(id) !== job || job.status !== 'pending') return;
          this.finishJob(id, job, code === 0 ? 'complete' : 'error', code === 0 ? null : 'login_failed');
          if (code === 0) this.schedule(id);
        };
        child.once('error', () => finish(-1)); child.once('exit', finish);
      }
    } catch { this.finishJob(id, job, 'error', 'login_failed'); }
    return this.publicJob(id);
  }
  loginCode(id, code) {
    this.account(id); const job = this.jobs.get(id); const child = this.claudeLogins.get(id);
    if (!job || job.status !== 'pending' || !child || typeof code !== 'string' || code.length < 1 || code.length > 4096 || /[\r\n]/.test(code)) throw new Error('invalid_login_code');
    child.stdin.write(`${code}\n`);
  }
  schedule(id) {
    if (this.closed) return;
    const ids = id ? [id] : this.store.state.accounts.filter(account => account.authenticated).map(account => account.id);
    for (const accountId of ids) this.refresh(accountId).catch(() => {});
  }
  refresh(id) {
    if (this.refreshes.has(id)) return this.refreshes.get(id);
    const refresh = this.readQuota(id).finally(() => this.refreshes.delete(id));
    this.refreshes.set(id, refresh); return refresh;
  }
  async setIdentity(account, identity, directory) {
    const email = safeText(identity.email, 128);
    const changed = account.authenticated && account.email !== email;
    if (changed) {
      account.five_hour = null; account.seven_day = null; account.observed_at = null;
      if (account.provider === 'claude') await rm(path.join(directory, 'quota-snapshot.json'), { force: true });
    }
    if (account.provider === 'claude') await atomicJSON(path.join(directory, 'quota-identity.json'), { v: 1, email });
    if (!this.live(account)) return false;
    account.authenticated = true; account.email = email; account.plan = safeText(identity.planType ?? identity.subscriptionType, 32);
    return true;
  }
  async readQuota(id) {
    const account = this.account(id);
    try {
      if (account.provider === 'codex') {
        const client = await this.codex(account);
        const loginJob = this.jobs.get(id);
        const identity = await client.request('account/read', { refreshToken: false });
        if (!this.live(account)) return;
        if (!identity.account || identity.account.type !== 'chatgpt') account.status = identity.account ? 'unsupported' : 'expired';
        else {
          if (!await this.setIdentity(account, identity.account)) return;
          this.finishCodexLoginFromIdentity(id, loginJob, identity.account.email);
          const result = await client.request('account/rateLimits/read', { excludeResetCreditDetails: true });
          if (!this.live(account)) return;
          Object.assign(account, codexWindows(result), { status: 'ok', observed_at: epoch() });
        }
      } else {
        if (!this.executables.claude) throw new Error('cli_unavailable');
        const directory = await this.prepare(account);
        if (!this.live(account)) return;
        const identity = await this.claudeRun(this.executables.claude, directory, ['auth', 'status', '--json']);
        if (!this.live(account)) return;
        if (!identity.loggedIn || identity.authMethod !== 'claude.ai') account.status = identity.loggedIn ? 'unsupported' : 'expired';
        else {
          if (!await this.setIdentity(account, identity, directory)) return;
          const cached = await readJSON(path.join(directory, 'quota-snapshot.json'));
          if (!this.live(account)) return;
          if (cached?.v === 1 && cached.identity === account.email && Number.isSafeInteger(cached.observed_at) && cached.observed_at > 0) {
            account.five_hour = publicWindow(cached.five_hour); account.seven_day = publicWindow(cached.seven_day);
            account.observed_at = cached.observed_at; account.status = 'ok';
          } else account.status = 'waiting';
        }
      }
    } catch { if (this.live(account)) account.status = this.executables[account.provider] ? 'error' : 'unsupported'; }
    if (this.live(account)) await this.store.changed();
  }
  async remove(id) {
    const account = this.account(id); const job = this.jobs.get(id);
    this.store.state.accounts = this.store.state.accounts.filter(item => item !== account);
    clearTimeout(job?.timer); this.jobs.delete(id); this.claudeLogins.get(id)?.kill('SIGTERM');
    await this.store.changed();
    await Promise.allSettled([this.loginStarts.get(id), this.refreshes.get(id), this.clientOpenings.get(id)].filter(Boolean));
    const client = this.clients.get(id);
    if (client) {
      if (job?.loginId && job.status === 'pending') await client.request('account/login/cancel', { loginId: job.loginId }).catch(() => {});
      await client.request('account/logout').catch(() => {}); client.close(); this.clients.delete(id);
    }
    if (account.provider === 'claude' && this.executables.claude) {
      const directory = path.join(this.store.profile(id), 'claude');
      await this.claudeRun(this.executables.claude, directory, ['auth', 'logout']).catch(() => {});
    }
    // Detached profiles are retained; official logout is attempted without deleting files.
  }
  async launchCommand(id) {
    const account = this.account(id);
    if (account.provider !== 'claude' || !this.executables.claude) throw new Error('unsupported_account');
    const directory = await this.prepare(account);
    if (!this.live(account)) throw new Error('account_not_found');
    return { command: `${shellQuote(process.execPath)} ${shellQuote(sessionScript)} ${shellQuote(this.executables.claude)} ${shellQuote(directory)}` };
  }
  publicAccounts() { return this.store.state.accounts.map(account => ({ ...publicAccount(account), authenticated: account.authenticated === true })); }
  publicJobs() { return [...this.jobs.keys()].map(id => this.publicJob(id)); }
  close() {
    this.closed = true; for (const job of this.jobs.values()) clearTimeout(job.timer);
    for (const client of this.clients.values()) client.close(); for (const child of this.claudeLogins.values()) child.kill('SIGTERM');
  }
}
