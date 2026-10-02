import { spawn } from 'node:child_process';
import path from 'node:path';
import { profileEnvironment, claudeCommand } from './clients.mjs';
const [executable, directory, ...arguments_] = process.argv.slice(2);
if (!executable || !directory || !path.isAbsolute(executable) || !path.isAbsolute(directory)) process.exit(1);
// Apply exactly the same credential isolation as the companion, in the user's terminal.
const environment = profileEnvironment('claude', directory);
try {
  const identity = await claudeCommand(executable, directory, ['auth', 'status', '--json']);
  if (!identity.loggedIn || identity.authMethod !== 'claude.ai' || typeof identity.email !== 'string') throw new Error('login_required');
  environment.AIQ_FEED_EMAIL = identity.email;
} catch { console.error('请先在本地设置页完成 Claude 订阅账户登录'); process.exit(1); }
const child = spawn(executable, arguments_, { cwd: process.cwd(), env: environment, stdio: 'inherit' });
child.once('error', () => { console.error('Claude 启动失败'); process.exitCode = 1; });
child.once('exit', code => { process.exitCode = code ?? 1; });
