// User-level macOS service; no administrator access or account credentials in the plist.
import { access, chmod, mkdir, rm, writeFile } from 'node:fs/promises';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';
import os from 'node:os';
import path from 'node:path';

const execute = promisify(execFile);
const label = 'cn.folotoy.ai-passport-quota';
const appDirectory = fileURLToPath(new URL('../', import.meta.url));
const plist = path.join(os.homedir(), 'Library/LaunchAgents', `${label}.plist`);
const domain = `gui/${process.getuid?.()}`;
const target = `${domain}/${label}`;
const xml = value => String(value).replace(/[&<>"']/g, character => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&apos;' })[character]);

try {
  if (process.platform !== 'darwin' || !['install', 'remove'].includes(process.argv[2])) {
    throw new Error('Use service:install or service:remove on macOS.');
  }
  const loaded = await execute('/bin/launchctl', ['print', target]).then(() => true, () => false);
  if (process.argv[2] === 'remove') {
    if (loaded) await execute('/bin/launchctl', ['bootout', target]);
    await rm(plist, { force: true });
    console.log('Login service removed. Accounts and pairing are retained.');
  } else {
    await access(path.join(appDirectory, 'dist/client/index.html'));
    const running = await fetch('http://127.0.0.1:4317/', { signal: AbortSignal.timeout(2000) }).then(response => response.ok, () => false);
    if (running && !loaded) throw new Error('Stop the manually started companion before installing the service.');
    const previous = await access(plist).then(async () => JSON.parse((await execute('/usr/bin/plutil', ['-convert', 'json', '-o', '-', plist])).stdout), error => { if (error.code === 'ENOENT') return null; throw error; });
    const stateDirectory = path.resolve(process.env.AIQ_STATE_DIR ?? previous?.EnvironmentVariables?.AIQ_STATE_DIR ?? path.join(os.homedir(), '.local/share/ai-passport-quota'));
    const logs = path.join(stateDirectory, 'logs');
    const log = path.join(logs, 'companion.log');
    await mkdir(path.dirname(plist), { recursive: true });
    await mkdir(logs, { recursive: true, mode: 0o700 });
    await chmod(logs, 0o700);
    await writeFile(log, '', { flag: 'a', mode: 0o600 });
    await chmod(log, 0o600);
    const searchPaths = [process.env.PATH ?? '', previous?.EnvironmentVariables?.PATH ?? '', `/opt/homebrew/bin:/usr/local/bin:${path.join(os.homedir(), '.local/bin')}:/usr/bin:/bin:/usr/sbin:/sbin`];
    const environment = { PATH: [...new Set(searchPaths.join(':').split(':').filter(Boolean))].join(':'), AIQ_STATE_DIR: stateDirectory };
    for (const key of ['AIQ_CODEX_BIN', 'AIQ_CLAUDE_BIN']) {
      const value = process.env[key] ?? previous?.EnvironmentVariables?.[key];
      if (value) environment[key] = path.resolve(value);
    }
    const content = `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>${label}</string>
<key>ProgramArguments</key><array><string>/usr/bin/env</string><string>node</string><string>${xml(path.join(appDirectory, 'server/index.mjs'))}</string></array>
<key>WorkingDirectory</key><string>${xml(appDirectory)}</string>
<key>EnvironmentVariables</key><dict>${Object.entries(environment).map(([key, value]) => `<key>${key}</key><string>${xml(value)}</string>`).join('')}</dict>
<key>RunAtLoad</key><true/>
<key>KeepAlive</key><true/>
<key>ThrottleInterval</key><integer>15</integer>
<key>StandardOutPath</key><string>${xml(log)}</string>
<key>StandardErrorPath</key><string>${xml(log)}</string>
</dict></plist>
`;
    await writeFile(plist, content, { mode: 0o600 });
    await chmod(plist, 0o600);
    await execute('/usr/bin/plutil', ['-lint', plist]);
    if (loaded) await execute('/bin/launchctl', ['bootout', target]);
    const registerDeadline = Date.now() + 15000;
    for (;;) {
      try { await execute('/bin/launchctl', ['bootstrap', domain, plist]); break; }
      catch (error) {
        // bootout may return before launchd finishes unregistering the old job.
        if (!loaded || error.code !== 5 || Date.now() >= registerDeadline) throw error;
        await new Promise(resolve => setTimeout(resolve, 250));
      }
    }
    const deadline = Date.now() + 15000;
    let ready = false;
    while (!ready && Date.now() < deadline) {
      const alive = await execute('/bin/launchctl', ['print', target]).then(result => /\bstate = running\b/.test(result.stdout), () => false);
      ready = alive && await fetch('http://127.0.0.1:4317/api/state', { signal: AbortSignal.timeout(2000) }).then(response => response.ok, () => false);
      if (!ready) await new Promise(resolve => setTimeout(resolve, 250));
    }
    if (!ready) throw new Error(`Service did not become ready; check ${log}.`);
    console.log('Login service installed. Open http://127.0.0.1:4317/.');
  }
} catch (error) {
  console.error(error.message);
  process.exitCode = 1;
}
