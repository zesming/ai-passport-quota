import test from 'node:test';
import assert from 'node:assert/strict';
import { deepSeekRmbDisplay } from '../src/deepseek-display.mjs';

test('DeepSeek presentation selects only CNY and preserves precise amount strings', () => {
  const cny = { currency: 'CNY', total_balance: '128.50', granted_balance: '10.00', topped_up_balance: '118.50' };
  const balance = { is_available: true, balance_infos: [
    { currency: 'USD', total_balance: '17.25', granted_balance: '2.00', topped_up_balance: '15.25' },
    cny,
  ] };
  assert.deepEqual(deepSeekRmbDisplay(balance), { info: cny, availability: '余额可用', tone: 'available' });
});

test('DeepSeek presentation stays unknown when CNY is absent even if USD is available', () => {
  const balance = { is_available: true, balance_infos: [
    { currency: 'USD', total_balance: '17.25', granted_balance: '2.00', topped_up_balance: '15.25' },
  ] };
  assert.deepEqual(deepSeekRmbDisplay(balance), { info: null, availability: '尚无人民币余额', tone: 'unknown' });
  assert.deepEqual(deepSeekRmbDisplay({ is_available: false, balance_infos: [] }), { info: null, availability: '尚无人民币余额', tone: 'unknown' });
});
