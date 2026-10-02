export const DEEPSEEK_BALANCE_URL = 'https://api.deepseek.com/user/balance';
export const DEEPSEEK_TIMEOUT_MS = 8000;
export const MAX_DEEPSEEK_RESPONSE_BYTES = 8192;

const CURRENCIES = new Set(['CNY', 'USD']);
const AMOUNT_PATTERN = /^-?[0-9]+(?:\.[0-9]+)?$/;

export function validateDeepSeekApiKey(value) {
  if (typeof value !== 'string' || Buffer.byteLength(value) < 1 || Buffer.byteLength(value) > 2048
    || !/^[\x21-\x7e]+$/.test(value)) throw new Error('invalid_api_key');
  return value;
}

export function normalizeDeepSeekBalance(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)
    || typeof value.is_available !== 'boolean'
    || !Array.isArray(value.balance_infos) || value.balance_infos.length > 2) {
    throw new Error('deepseek_invalid_response');
  }
  const seen = new Set();
  const balanceInfos = value.balance_infos.map(item => {
    if (!item || typeof item !== 'object' || Array.isArray(item)
      || !CURRENCIES.has(item.currency) || seen.has(item.currency)) {
      throw new Error('deepseek_invalid_response');
    }
    seen.add(item.currency);
    for (const key of ['total_balance', 'granted_balance', 'topped_up_balance']) {
      const amount = item[key];
      if (typeof amount !== 'string' || amount.length > 20 || !AMOUNT_PATTERN.test(amount)) {
        throw new Error('deepseek_invalid_response');
      }
    }
    return {
      currency: item.currency,
      total_balance: item.total_balance,
      granted_balance: item.granted_balance,
      topped_up_balance: item.topped_up_balance,
    };
  });
  return { is_available: value.is_available, balance_infos: balanceInfos };
}

async function readBoundedText(response) {
  const contentLength = response.headers?.get?.('content-length');
  if (contentLength && /^\d+$/.test(contentLength) && Number(contentLength) > MAX_DEEPSEEK_RESPONSE_BYTES) {
    throw new Error('deepseek_invalid_response');
  }
  if (typeof response.body?.getReader !== 'function') {
    if (typeof response.text !== 'function') throw new Error('deepseek_invalid_response');
    const text = await response.text();
    if (Buffer.byteLength(text) > MAX_DEEPSEEK_RESPONSE_BYTES) throw new Error('deepseek_invalid_response');
    return text;
  }
  const reader = response.body.getReader();
  const chunks = [];
  let size = 0;
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      size += value.byteLength;
      if (size > MAX_DEEPSEEK_RESPONSE_BYTES) {
        await reader.cancel().catch(() => {});
        throw new Error('deepseek_invalid_response');
      }
      chunks.push(Buffer.from(value));
    }
  } finally {
    reader.releaseLock();
  }
  return Buffer.concat(chunks).toString('utf8');
}

export async function requestDeepSeekBalance(apiKey, { fetchImpl = globalThis.fetch, timeoutMs = DEEPSEEK_TIMEOUT_MS } = {}) {
  const validatedKey = validateDeepSeekApiKey(apiKey);
  if (typeof fetchImpl !== 'function') throw new Error('deepseek_unavailable');
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetchImpl(DEEPSEEK_BALANCE_URL, {
      method: 'GET',
      headers: { Accept: 'application/json', Authorization: `Bearer ${validatedKey}` },
      redirect: 'error',
      signal: controller.signal,
    });
    if (response.status === 401 || response.status === 403) throw new Error('deepseek_unauthorized');
    if (response.status !== 200) throw new Error('deepseek_unavailable');
    const text = await readBoundedText(response);
    let payload;
    try { payload = JSON.parse(text); } catch { throw new Error('deepseek_invalid_response'); }
    return normalizeDeepSeekBalance(payload);
  } catch (error) {
    if (['deepseek_unauthorized', 'deepseek_unavailable', 'deepseek_invalid_response', 'deepseek_timeout'].includes(error.message)) throw error;
    if (error.name === 'AbortError') throw new Error('deepseek_timeout');
    throw new Error('deepseek_unavailable');
  } finally {
    clearTimeout(timeout);
  }
}
