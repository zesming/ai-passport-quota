import test from 'node:test';
import assert from 'node:assert/strict';
import { startDeviceSerial, serialErrorMessage } from '../src/serial.mjs';

const encoder = new TextEncoder();
const requestId = '0123abcd';
const ack = (id = requestId, ok = true, error = null) => `@AIQ:${JSON.stringify({ v: 1, op: 'result', request_id: id, ok, error })}\r\n`;
const frame = encoder.encode('@AIQ:{"v":1,"op":"configure"}\n');

function fixture(t, onWrite = () => {}) {
  let input;
  const events = [];
  const port = {
    readable: new ReadableStream({
      start(controller) { input = controller; },
      pull() { events.push('read'); },
      cancel() { events.push('cancel'); },
    }, { highWaterMark: 0 }),
    writable: new WritableStream({
      write(bytes) { events.push('write'); return onWrite(bytes, input, events); },
      abort() { events.push('abort'); },
    }),
  };
  const session = startDeviceSerial(port, requestId);
  t.after(() => session.close());
  return { port, session, input, events };
}

test('USB receives before pairing preparation and retains an ACK arriving during the write', async t => {
  const { session, input, events, port } = fixture(t, async (_, controller) => {
    controller.enqueue(encoder.encode(ack()));
    await new Promise(resolve => setTimeout(resolve, 5));
  });
  input.enqueue(encoder.encode('I (705) ai_quota: ready\r\n'));
  await new Promise(resolve => setTimeout(resolve, 5));
  assert.equal(events[0], 'read');
  const result = await session.send(frame, { timeoutMs: 100, bootWaitMs: 20 });
  assert.equal(result.ok, true);
  assert.equal(events.indexOf('read') < events.indexOf('write'), true);
  await session.close();
  assert.equal(port.readable.locked, false);
  assert.equal(port.writable.locked, false);
});

test('USB parser handles split UTF-8, CRLF, malformed data and a wrong request ID', async t => {
  const { session } = fixture(t, (_, input) => {
    const bytes = encoder.encode(`中文日志\r\n@AIQ:broken\n${ack('ffffffff')}${ack(requestId, false, 'pairing_closed')}`);
    for (let offset = 0; offset < bytes.length; offset += 2) input.enqueue(bytes.slice(offset, offset + 2));
  });
  const result = await session.send(frame, { timeoutMs: 100, bootWaitMs: 0 });
  assert.equal(result.ok, false);
  assert.equal(result.error, 'pairing_closed');
});

test('USB parser discards an oversized line and recovers at the following newline', async t => {
  const { session } = fixture(t, (_, input) => {
    input.enqueue(encoder.encode('x'.repeat(4100)));
    input.enqueue(encoder.encode(`tail\n${ack()}`));
  });
  assert.equal((await session.send(frame, { timeoutMs: 100, bootWaitMs: 0 })).ok, true);
});

test('USB timeout starts after preparation and the bounded boot grace', async t => {
  const { session } = fixture(t, (_, input) => input.enqueue(encoder.encode(ack())));
  await new Promise(resolve => setTimeout(resolve, 20));
  assert.equal((await session.send(frame, { timeoutMs: 10, bootWaitMs: 20 })).ok, true);
});

test('USB timeout distinguishes no input, startup logs and an unmatched ACK without retaining input', async t => {
  for (const [message, expected] of [
    ['', { received_data: false, ready_seen: false, result_seen: false, matching_result_seen: false }],
    ['I (705) ai_quota: ready\nsecret-log-content\n', { received_data: true, ready_seen: true, result_seen: false, matching_result_seen: false }],
    [ack('ffffffff'), { received_data: true, ready_seen: false, result_seen: true, matching_result_seen: false }],
  ]) {
    const { session } = fixture(t, (_, input) => { if (message) input.enqueue(encoder.encode(message)); });
    await assert.rejects(session.send(frame, { timeoutMs: 10, bootWaitMs: 0 }), error => {
      assert.equal(error.code, 'serial_timeout');
      assert.deepEqual(error.diagnostics, expected);
      assert.equal(JSON.stringify(error).includes('secret-log-content'), false);
      assert.equal(typeof serialErrorMessage(error), 'string');
      return true;
    });
    await session.close();
  }
});

test('a read failure before sending is handled and stream locks can be released', async t => {
  const { session, input, port, events } = fixture(t);
  input.error(new Error('hardware disconnected'));
  await new Promise(resolve => setTimeout(resolve, 5));
  await assert.rejects(session.send(frame, { timeoutMs: 100, bootWaitMs: 0 }), error => error.code === 'serial_read_error');
  assert.equal(events.includes('write'), false);
  await session.close();
  assert.equal(port.readable.locked, false);
  assert.equal(port.writable.locked, false);
});

test('oversized outbound configuration is rejected before USB transmission', async t => {
  const { session, events } = fixture(t);
  await assert.rejects(session.send(new Uint8Array(4097)), error => error.code === 'frame_too_long');
  assert.equal(events.includes('write'), false);
});
