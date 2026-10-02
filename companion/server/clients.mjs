import { spawn, execFile } from 'node:child_process';
import { EventEmitter } from 'node:events';
import { access } from 'node:fs/promises';
import { constants } from 'node:fs';
import path from 'node:path';

export function profileEnvironment(provider, directory) {
  const environment = { ...process.env };
  for (const name of Object.keys(environment)) {
    if (/^(OPENAI_|CODEX_|ANTHROPIC_|CLAUDE_)/.test(name)) delete environment[name];
  }
  if (provider === 'codex') environment.CODEX_HOME = directory;
  else { environment.CLAUDE_CONFIG_DIR = directory; environment.ANTHROPIC_CONFIG_DIR = path.join(directory, 'anthropic'); }
  return environment;
}
export function shellQuote(value) { return `'${value.replace(/'/g, `'"'"'`)}'`; }
export async function findCLI(name) {
  const explicit = process.env[`AIQ_${name.toUpperCase()}_BIN`];
  const alternatives = (process.env.PATH ?? '').split(path.delimiter).map(directory => path.join(directory, name));
  if (name === 'codex') alternatives.push('/Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex');
  if (name === 'claude') alternatives.push(path.join(process.env.HOME ?? '', '.local/bin/claude'));
  for (const filename of explicit ? [explicit] : alternatives) {
    try { await access(filename, constants.X_OK); return filename; } catch { /* Try the next normal install path. */ }
  }
  return null;
}
export class CodexClient extends EventEmitter {
  constructor(executable, directory) { super(); this.executable = executable; this.directory = directory; this.pending = new Map(); this.nextId = 1; this.buffer = ''; this.process = null; this.opening = null; }
  open() {
    if (this.opening) return this.opening;
    const opening = this.start().catch(error => { if (this.opening === opening) this.opening = null; throw error; });
    this.opening = opening;
    return this.opening;
  }
  async start() {
    const child = spawn(this.executable, ['app-server', '-c', 'cli_auth_credentials_store="file"', '--listen', 'stdio://'], { cwd: this.directory, env: profileEnvironment('codex', this.directory), stdio: ['pipe', 'pipe', 'pipe'] });
    this.process = child; this.buffer = '';
    child.stdout.setEncoding('utf8');
    child.stdout.on('data', chunk => {
      if (this.process !== child) return;
      this.buffer += chunk;
      if (Buffer.byteLength(this.buffer) > 1024 * 1024) { child.kill('SIGTERM'); this.fail(child); return; }
      let newline;
      while ((newline = this.buffer.indexOf('\n')) >= 0) {
        const line = this.buffer.slice(0, newline); this.buffer = this.buffer.slice(newline + 1);
        let message; try { message = JSON.parse(line); } catch { continue; }
        if (message.id !== undefined && this.pending.has(message.id)) {
          const request = this.pending.get(message.id); this.pending.delete(message.id); clearTimeout(request.timer);
          if (message.error) request.reject(new Error('codex_request_failed')); else request.resolve(message.result);
        } else if (message.method) this.emit('notification', message.method, message.params ?? {});
      }
    });
    child.stderr.resume();
    child.on('error', () => this.fail(child));
    child.on('exit', () => this.fail(child));
    try { await this.request('initialize', { clientInfo: { name: 'ai-passport-quota', version: '1.0.0' } }); }
    catch (error) { this.fail(child); child.kill('SIGTERM'); throw error; }
    if (this.process !== child) throw new Error('codex_unavailable');
    child.stdin.write(`${JSON.stringify({ method: 'initialized', params: {} })}\n`);
    return this;
  }
  request(method, params, timeout = 20000) {
    if (!this.process || this.process.exitCode !== null || this.process.killed) return Promise.reject(new Error('codex_unavailable'));
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.pending.delete(id); reject(new Error('codex_timeout')); }, timeout);
      this.pending.set(id, { resolve, reject, timer });
      const payload = { id, method, ...(params === undefined ? {} : { params }) };
      this.process.stdin.write(`${JSON.stringify(payload)}\n`, error => { if (error && this.pending.has(id)) { clearTimeout(timer); this.pending.delete(id); reject(new Error('codex_unavailable')); } });
    });
  }
  fail(child = this.process, notify = true) {
    if (child && this.process !== child) return;
    for (const request of this.pending.values()) { clearTimeout(request.timer); request.reject(new Error('codex_unavailable')); }
    this.pending.clear(); this.opening = null; this.buffer = ''; this.process = null;
    if (child && notify) this.emit('disconnected');
  }
  close() { const child = this.process; this.fail(child, false); child?.kill('SIGTERM'); }
}
export function claudeCommand(executable, directory, arguments_, timeout = 20000) {
  return new Promise((resolve, reject) => {
    execFile(executable, arguments_, { cwd: directory, env: profileEnvironment('claude', directory), timeout, maxBuffer: 64 * 1024 }, (error, stdout) => {
      if (error && error.code !== 1) { reject(new Error('claude_unavailable')); return; }
      let result; try { result = JSON.parse(stdout); } catch { reject(new Error('claude_status_invalid')); return; }
      resolve(result);
    });
  });
}
export function officialLoginURL(value, provider) {
  try {
    const url = new URL(value);
    const allowed = provider === 'codex' ? ['openai.com', 'chatgpt.com'] : ['claude.ai', 'anthropic.com', 'claude.com'];
    return url.protocol === 'https:' && !url.username && !url.password && allowed.some(domain => url.hostname === domain || url.hostname.endsWith(`.${domain}`)) ? url.href : null;
  } catch { return null; }
}
