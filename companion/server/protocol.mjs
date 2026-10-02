import { timingSafeEqual } from 'node:crypto';
import { normalizeDeepSeekBalance } from './deepseek.mjs';

export const MAX_ACCOUNTS = 8;
export const MAX_SNAPSHOT_BYTES = 8192;
export const INTERVALS = [60, 300, 900, 1800];
export const SCREEN_TIMEOUT_SECONDS = [0, 30, 60, 120, 300, 600];
export const DEFAULT_SCREEN_TIMEOUT_SECONDS = 120;
export const epoch = () => Math.floor(Date.now() / 1000);
export const validId = value => typeof value === 'string' && /^[a-f0-9]{32}$/.test(value);
export const privateIPv4 = value => typeof value === 'string' && /^(10|192\.168|172\.(1[6-9]|2\d|3[01]))\./.test(value) && value.split('.').length === 4 && value.split('.').every(part => /^\d{1,3}$/.test(part) && Number(part) <= 255);
export function safeText(value, limit) {
  if (typeof value !== 'string') return '';
  let result = '';
  for (const character of value.replace(/[\u0000-\u001f\u007f]/g, '')) {
    if (Buffer.byteLength(result + character) > limit) break;
    result += character;
  }
  return result;
}
export function validSettings(value) {
  return value && typeof value === 'object' && !Array.isArray(value)
    && INTERVALS.includes(value.refresh_seconds) && typeof value.auto_refresh === 'boolean'
    && (!Object.hasOwn(value, 'screen_timeout_seconds') || SCREEN_TIMEOUT_SECONDS.includes(value.screen_timeout_seconds));
}
export function settingsWithDefaults(value) {
  return {
    refresh_seconds: value.refresh_seconds,
    auto_refresh: value.auto_refresh,
    screen_timeout_seconds: value.screen_timeout_seconds ?? DEFAULT_SCREEN_TIMEOUT_SECONDS,
  };
}
export function percentWindow(used, resets) {
  if (typeof used !== 'number' || !Number.isFinite(used)) return null;
  return { remaining_percent: Math.min(100, Math.max(0, Math.round(100 - used))), resets_at: Number.isInteger(resets) && resets > 0 ? resets : null };
}
export function codexWindows(response) {
  const map = response?.rateLimitsByLimitId;
  const candidates = map && typeof map === 'object' ? Object.entries(map) : [];
  const preferred = candidates.find(([id, bucket]) => id === 'codex' || bucket?.limitId === 'codex');
  const bucket = preferred?.[1] ?? (candidates.length === 1 ? candidates[0][1] : response?.rateLimits);
  const result = { five_hour: null, seven_day: null };
  for (const window of [bucket?.primary, bucket?.secondary]) {
    if (window?.windowDurationMins === 300) result.five_hour = percentWindow(window.usedPercent, window.resetsAt);
    if (window?.windowDurationMins === 10080) result.seven_day = percentWindow(window.usedPercent, window.resetsAt);
  }
  return result;
}
export function claudeWindows(payload) {
  const five = payload?.rate_limits?.five_hour;
  const week = payload?.rate_limits?.seven_day;
  return { five_hour: percentWindow(five?.used_percentage, five?.resets_at), seven_day: percentWindow(week?.used_percentage, week?.resets_at) };
}
export function constantToken(actual, expected) {
  if (typeof actual !== 'string' || typeof expected !== 'string') return false;
  const a = Buffer.from(actual); const b = Buffer.from(expected);
  return a.length === b.length && timingSafeEqual(a, b);
}
export function publicAccount(account) {
  const result = {
    id: account.id, provider: account.provider, email: safeText(account.email, 128), plan: safeText(account.plan, 32),
    status: ['ok', 'waiting', 'expired', 'error', 'unsupported'].includes(account.status) ? account.status : 'error',
    observed_at: Number.isSafeInteger(account.observed_at) && account.observed_at > 0 ? account.observed_at : null,
    five_hour: publicWindow(account.five_hour), seven_day: publicWindow(account.seven_day),
  };
  if (account.provider === 'deepseek') {
    result.email = '';
    result.plan = 'API';
    result.label = normalizeDeepSeekLabel(account.label);
    result.balance = null;
    result.five_hour = null;
    result.seven_day = null;
    try { if (account.balance) result.balance = normalizeDeepSeekBalance(account.balance); } catch {}
  }
  return result;
}
export function normalizeDeepSeekLabel(value) {
  if (value === undefined || value === null || value === '') return 'DeepSeek API';
  if (typeof value !== 'string' || Buffer.byteLength(value) > 32 || /[\u0000-\u001f\u007f]/.test(value)) throw new Error('invalid_account_label');
  return value;
}
export function publicWindow(window) {
  if (!window || !Number.isInteger(window.remaining_percent) || window.remaining_percent < 0 || window.remaining_percent > 100) return null;
  return { remaining_percent: window.remaining_percent, resets_at: Number.isSafeInteger(window.resets_at) && window.resets_at > 0 ? window.resets_at : null };
}
export function makeSnapshot(state, now = epoch()) {
  const result = { v: 1, server_time: now, revision: state.revision, settings: settingsWithDefaults(state.settings), accounts: state.accounts.filter(account => account.authenticated).map(publicAccount) };
  if (result.accounts.length > MAX_ACCOUNTS || Buffer.byteLength(JSON.stringify(result)) > MAX_SNAPSHOT_BYTES) throw new Error('snapshot_bounds');
  return result;
}
