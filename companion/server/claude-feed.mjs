import { createHash } from 'node:crypto';
import { mkdir, stat, rm } from 'node:fs/promises';
import { atomicJSON, readJSON, privateDirectory } from './storage.mjs';
import { claudeWindows, epoch } from './protocol.mjs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

async function withFeedLock(directory, operation) {
  const lock = path.join(directory, '.quota-feed-lock');
  for (let attempt = 0; attempt < 100; attempt++) {
    try { await mkdir(lock, { mode: 0o700 }); break; }
    catch (error) {
      if (error.code !== 'EEXIST') throw error;
      // Recover only a lock abandoned by a crashed callback.
      try { if (Date.now() - (await stat(lock)).mtimeMs > 30000) await rm(lock, { recursive: true, force: true }); } catch { /* retry */ }
      if (attempt === 99) return;
      await new Promise(resolve => setTimeout(resolve, 20));
    }
  }
  try { return await operation(); }
  finally { await rm(lock, { recursive: true, force: true }); }
}
export async function recordClaudeFeed(directory, payload, now = epoch(), identity = process.env.AIQ_FEED_EMAIL ?? '') {
  const windows = claudeWindows(payload);
  if (!windows.five_hour && !windows.seven_day) return;
  await privateDirectory(directory);
  const evidence = JSON.stringify({ identity, windows, api_duration: payload.cost?.total_api_duration_ms ?? null, cost: payload.cost?.total_cost_usd ?? null });
  const fingerprint = createHash('sha256').update(evidence).digest('hex');
  // Sessions keep separate evidence so alternating idle callbacks never refresh
  // each other's cached observation or overwrite a more recent source reading.
  const sessionKey = createHash('sha256').update(JSON.stringify([identity, payload.session_id ?? 'unknown'])).digest('hex');
  const sessionFile = path.join(directory, 'quota-sessions', `${sessionKey}.json`);
  const filename = path.join(directory, 'quota-snapshot.json');
  await withFeedLock(directory, async () => {
    const verified = await readJSON(path.join(directory, 'quota-identity.json'));
    if (verified && verified.email !== identity) return;
    const previous = await readJSON(sessionFile);
    if (previous?.fingerprint === fingerprint) return;
    const observation = { v: 1, identity, ...windows, observed_at: now, fingerprint };
    await atomicJSON(sessionFile, observation);
    const current = await readJSON(filename);
    if (!current || current.identity !== identity || observation.observed_at >= current.observed_at) await atomicJSON(filename, observation);
  });
}
async function main() {
  const directory = process.argv[2];
  if (!directory || !path.isAbsolute(directory)) return;
  let input = '';
  for await (const chunk of process.stdin) {
    input += chunk.toString('utf8'); if (Buffer.byteLength(input) > 128 * 1024) return;
  }
  try { await recordClaudeFeed(directory, JSON.parse(input)); process.stdout.write('AI 额度已同步'); }
  catch { process.stdout.write('AI 额度等待同步'); }
}
if (process.argv[1] && pathToFileURL(path.resolve(process.argv[1])).href === import.meta.url) await main();
