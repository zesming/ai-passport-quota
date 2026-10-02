import test from 'node:test';
import assert from 'node:assert/strict';
import {
  DEEPSEEK_BALANCE_URL, MAX_DEEPSEEK_RESPONSE_BYTES, normalizeDeepSeekBalance,
  requestDeepSeekBalance, validateDeepSeekApiKey,
} from '../server/deepseek.mjs';

const sample = {
  is_available: true,
  balance_infos: [
    { currency: 'CNY', total_balance: '-0.0000000000000001', granted_balance: '0', topped_up_balance: '12345678901234567890' },
    { currency: 'USD', total_balance: '0.0000000000000001', granted_balance: '2.50', topped_up_balance: '17.25' },
  ],
};

test('DeepSeek balance keeps CNY and USD decimal strings exact and never invents reset windows', () => {
  assert.deepEqual(normalizeDeepSeekBalance(sample), sample);
  assert.deepEqual(normalizeDeepSeekBalance({ is_available: false, balance_infos: [] }), { is_available: false, balance_infos: [] });
});

test('DeepSeek balance rejects malformed currencies, amounts, duplicates, and oversized schemas', () => {
  const mutate = (changes, infos = sample.balance_infos) => ({ ...sample, ...changes, balance_infos: infos });
  for (const value of [
    null,
    [],
    { ...sample, is_available: 1 },
    { ...sample, balance_infos: null },
    mutate({}, [{ ...sample.balance_infos[0], currency: 'EUR' }]),
    mutate({}, [sample.balance_infos[0], sample.balance_infos[0]]),
    mutate({}, [...sample.balance_infos, sample.balance_infos[0]]),
    mutate({}, [{ ...sample.balance_infos[0], total_balance: 0 }]),
    mutate({}, [{ ...sample.balance_infos[0], granted_balance: '1e3' }]),
    mutate({}, [{ ...sample.balance_infos[0], topped_up_balance: '+1' }]),
    mutate({}, [{ ...sample.balance_infos[0], total_balance: '123456789012345678901' }]),
  ]) assert.throws(() => normalizeDeepSeekBalance(value), /deepseek_invalid_response/);
});

test('DeepSeek keys reject empty, non-visible, and oversized input', () => {
  assert.equal(validateDeepSeekApiKey('sk-test_01'), 'sk-test_01');
  for (const key of ['', 'has space', 'has\nlinebreak', 'x'.repeat(2049)]) assert.throws(() => validateDeepSeekApiKey(key), /invalid_api_key/);
});

test('balance request uses only the official GET endpoint with bearer auth and rejects unsafe bodies', async () => {
  let call;
  const result = await requestDeepSeekBalance('sk-unit-test', {
    fetchImpl: async (url, options) => {
      call = { url, options };
      return new Response(JSON.stringify(sample), { status: 200, headers: { 'content-type': 'application/json' } });
    },
  });
  assert.deepEqual(result, sample);
  assert.equal(call.url, DEEPSEEK_BALANCE_URL);
  assert.equal(call.options.method, 'GET');
  assert.equal(call.options.headers.Authorization, 'Bearer sk-unit-test');
  assert.equal(call.options.redirect, 'error');
  assert.ok(call.options.signal instanceof AbortSignal);

  await assert.rejects(requestDeepSeekBalance('sk-unit-test', {
    fetchImpl: async () => new Response('PRIVATE_UPSTREAM_BODY', { status: 403 }),
  }), error => error.message === 'deepseek_unauthorized' && !error.message.includes('PRIVATE_UPSTREAM_BODY'));
  await assert.rejects(requestDeepSeekBalance('sk-unit-test', {
    fetchImpl: async () => new Response('x'.repeat(MAX_DEEPSEEK_RESPONSE_BYTES + 1), { status: 200 }),
  }), /deepseek_invalid_response/);
});

test('balance request returns safe status and timeout errors without reading provider error bodies', async () => {
  await assert.rejects(requestDeepSeekBalance('sk-unit-test', {
    fetchImpl: async () => new Response('PRIVATE_UPSTREAM_BODY', { status: 500 }),
  }), error => error.message === 'deepseek_unavailable' && !error.message.includes('PRIVATE_UPSTREAM_BODY'));

  const fetchUntilAborted = (_url, { signal }) => new Promise((_, reject) => {
    signal.addEventListener('abort', () => reject(Object.assign(new Error('aborted'), { name: 'AbortError' })), { once: true });
  });
  await assert.rejects(requestDeepSeekBalance('sk-unit-test', { fetchImpl: fetchUntilAborted, timeoutMs: 5 }), /deepseek_timeout/);
});
