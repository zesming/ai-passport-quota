import test from 'node:test';
import assert from 'node:assert/strict';
import { DeviceSerialError, UsbDeviceSession, openDeviceSerial, openUsbDeviceSession, startDeviceSerial, serialErrorMessage } from '../src/serial.mjs';

const encoder = new TextEncoder();
const requestId = '0123abcd';
const ack = (id = requestId, ok = true, error = null) => `@AIQ:${JSON.stringify({ v: 1, op: 'result', request_id: id, ok, error })}\r\n`;
const frame = encoder.encode('@AIQ:{"v":1,"op":"configure"}\n');

function fixture(t, onWrite = () => {}, start = true) {
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
  const session = start ? startDeviceSerial(port, requestId) : null;
  t.after(() => session?.close());
  return { port, session, input, events };
}

test('opening USB prepares control signals but waits for an explicit send on the same port', async t => {
  const { port, events } = fixture(t, (_, input) => input.enqueue(encoder.encode(ack())), false);
  let opens = 0, closes = 0;
  port.open = async options => {
    assert.deepEqual(options, { baudRate: 115200, bufferSize: 4096, flowControl: 'none' });
    opens++;
  };
  port.setSignals = async signals => {
    assert.deepEqual(signals, { dataTerminalReady: false, requestToSend: false });
    assert.equal(port.readable.locked, true);
    events.push('signals');
  };
  port.close = async () => {
    assert.equal(port.readable.locked || port.writable.locked, false);
    closes++;
  };
  const session = await openDeviceSerial(port, requestId);
  t.after(() => session.close());
  assert.equal(events.includes('write'), false);
  assert.equal((await session.send(frame, { timeoutMs: 100, bootWaitMs: 0 })).ok, true);
  assert.equal(events.indexOf('signals') < events.indexOf('write'), true);
  await session.close();
  assert.equal(opens, 1);
  assert.equal(closes, 1);
});

test('a failed control-signal setup releases stream locks and the opened port', async t => {
  const { port, events } = fixture(t, () => {}, false);
  let closes = 0;
  port.open = async () => {};
  port.setSignals = async () => { throw new Error('signals unavailable'); };
  port.close = async () => {
    assert.equal(port.readable.locked || port.writable.locked, false);
    closes++;
  };
  await assert.rejects(openDeviceSerial(port, requestId), /signals unavailable/);
  assert.equal(events.includes('write'), false);
  assert.equal(closes, 1);
});

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
    input.enqueue(encoder.encode('x'.repeat(33000)));
    input.enqueue(encoder.encode(`tail\n${ack()}`));
  });
  assert.equal((await session.send(frame, { timeoutMs: 100, bootWaitMs: 0 })).ok, true);
});

test('USB v2 state_get matches its ID, accepts fragmented UTF-8 and validates the physical-window nonce', async t => {
  const sessionId = 'a'.repeat(32);
  const { port, input } = fixture(t, (_, controller) => {}, false);
  port.open = async () => {};
  port.setSignals = async () => {};
  port.close = async () => {};
  const client = await openUsbDeviceSession(port, { requestId: () => requestId });
  t.after(() => client.close());
  let phase = 0;
  const oldWrite = client.serial.send;
  client.serial.send = async (bytes, id, options) => {
    const frameText = new TextDecoder().decode(bytes);
    const outgoing = JSON.parse(frameText.slice(5));
    if (outgoing.op === 'session_open') {
      input.enqueue(encoder.encode(`@AIQ:${JSON.stringify({ op: 'result', request_id: id, ok: true, session_id: sessionId, remaining_seconds: 110, max_command_bytes: 2048, max_frame_bytes: 4096, max_state_bytes: 16384 })}\n`));
    } else {
      phase++;
      const state = { network: { ssid: '测试网络' }, jobs: [] };
      if (phase === 1) input.enqueue(encoder.encode(`@AIQ:${JSON.stringify({ op: 'state', request_id: 'ffffffff', ok: true, session_id: sessionId, state })}\n`));
      const responseSession = phase === 2 ? 'b'.repeat(32) : sessionId;
      const line = `@AIQ:${JSON.stringify({ op: 'state', request_id: id, ok: true, session_id: responseSession, state })}\n`;
      const bytes = encoder.encode(line);
      for (let offset = 0; offset < bytes.length; offset += 3) input.enqueue(bytes.slice(offset, offset + 3));
    }
    return oldWrite.call(client.serial, bytes, id, options);
  };
  await client.openSession();
  const state = await client.stateGet();
  assert.equal(state.network.ssid, '测试网络');
  await assert.rejects(client.stateGet(), error => error.code === 'usb_invalid_session_response');
});

test('USB v2 rejects state above negotiated byte limit', async t => {
  const sessionId = 'a'.repeat(32);
  let input;
  const port = {
    readable: new ReadableStream({ start(controller) { input = controller; }, pull() {}, cancel() {} }, { highWaterMark: 0 }),
    writable: new WritableStream({ write(bytes) { const outgoing = JSON.parse(new TextDecoder().decode(bytes).slice(5)); const result = outgoing.op === 'session_open'
      ? { op: 'result', request_id: outgoing.request_id, ok: true, session_id: sessionId, remaining_seconds: 100, max_command_bytes: 2048, max_frame_bytes: 4096, max_state_bytes: 8 }
      : { op: 'state', request_id: outgoing.request_id, ok: true, session_id: sessionId, state: { long: 'oversize state' } }; input.enqueue(encoder.encode(`@AIQ:${JSON.stringify(result)}\n`)); }, abort() {} }),
    open: async () => {}, setSignals: async () => {}, close: async () => {},
  };
  const client = await openUsbDeviceSession(port, { requestId: () => requestId });
  t.after(() => client.close());
  await client.openSession();
  await assert.rejects(client.stateGet(), error => error.code === 'usb_state_too_large');
});

test('USB v2 resets session identity without closing the physical serial port', async () => {
  const openerIds = ['0123abcd', '87654321'];
  const writes = [];
  let closes = 0;
  const serial = {
    async send(bytes) {
      const frame = JSON.parse(new TextDecoder().decode(bytes).slice(5));
      writes.push(frame);
      return { op: 'result', request_id: frame.request_id, ok: true, session_id: 'b'.repeat(32), remaining_seconds: 90, max_command_bytes: 2048, max_frame_bytes: 4096, max_state_bytes: 16384 };
    },
    async close() { closes++; },
  };
  const client = new UsbDeviceSession(serial, { requestId: () => openerIds.shift() });
  client.sessionId = 'a'.repeat(32);
  client.sessionExpiresAt = 1;
  client.limits = { max_command_bytes: 2048, max_frame_bytes: 4096, max_state_bytes: 16384 };
  client.pendingMutation = { requestId, sessionId: client.sessionId };
  client.resetSession();
  assert.equal(client.openerId, '87654321');
  assert.equal(client.sessionId, '');
  assert.equal(client.pendingMutation, null);
  assert.equal(client.limits, null);
  assert.equal(closes, 0);
  await client.openSession();
  assert.equal(writes[0].request_id, '87654321');
  assert.equal(closes, 0);
  await client.close();
  assert.equal(closes, 1);
});

test('USB v2 detects local physical-window expiry before state reads or mutation writes', async t => {
  const writes = [];
  const client = new UsbDeviceSession({ async send(bytes) { writes.push(bytes); }, async close() {} }, {
    requestId: () => requestId,
    now: () => 1000,
  });
  t.after(() => client.close());
  client.sessionId = 'd'.repeat(32);
  client.sessionExpiresAt = 999;
  client.limits = { max_command_bytes: 2048, max_frame_bytes: 4096, max_state_bytes: 16384 };
  await assert.rejects(client.stateGet(), error => error.code === 'device_session_expired');
  await assert.rejects(client.command({ v: 1, request_id: requestId, op: 'settings_save' }), error => error.code === 'device_session_expired');
  assert.equal(writes.length, 0, 'expired-window operations do not touch the serial port');
});

test('USB v2 permits one identical in-session mutation retry and blocks later writes until a state job proves admission', async () => {
  const writes = [];
  let stateReads = 0;
  const serial = {
    async send(bytes) {
      const frame = JSON.parse(new TextDecoder().decode(bytes).slice(5));
      writes.push(JSON.stringify(frame));
      if (frame.op === 'command' && writes.filter(item => JSON.parse(item).op === 'command').length <= 2) {
        throw new DeviceSerialError('serial_timeout');
      }
      if (frame.op === 'state_get') {
        stateReads++;
        return { op: 'state', request_id: frame.request_id, ok: true, session_id: frame.session_id, state: { jobs: [{ request_id: requestId, op: 'settings_save', status: 'running' }] } };
      }
      return { op: 'result', request_id: frame.request_id, ok: true, session_id: frame.session_id, accepted: true };
    },
    async close() {},
  };
  const client = new UsbDeviceSession(serial, { requestId: () => '87654321', now: () => 1000 });
  client.sessionId = 'c'.repeat(32);
  client.sessionExpiresAt = 100000;
  client.limits = { max_command_bytes: 2048, max_frame_bytes: 4096, max_state_bytes: 16384 };
  const body = { v: 1, request_id: requestId, op: 'settings_save', phone_utc: 1800000000 };
  await assert.rejects(client.command(body), error => error.code === 'usb_uncertain_receipt');
  const commandWrites = writes.filter(item => JSON.parse(item).op === 'command');
  assert.equal(commandWrites.length, 2);
  assert.equal(commandWrites[0], commandWrites[1]);
  await assert.rejects(client.command({ ...body, request_id: '87654321' }), error => error.code === 'usb_mutation_unresolved');
  await client.stateGet();
  assert.equal(stateReads, 1);
  assert.equal((await client.command({ ...body, request_id: '87654321' })).accepted, true);
});

test('a genuinely pending serial write times out promptly, forbids replay and closes the opened port', async t => {
  let closes = 0;
  let writes = 0;
  const { port } = fixture(t, () => { writes++; return new Promise(() => {}); }, false);
  port.open = async () => {};
  port.setSignals = async () => {};
  port.close = async () => { closes++; };
  const client = await openUsbDeviceSession(port, { requestId: () => requestId });
  await assert.rejects(client.serial.send(frame, requestId, { timeoutMs: 15, bootWaitMs: 0 }), error => error.code === 'serial_write_timeout');
  await assert.rejects(client.serial.send(frame, requestId, { timeoutMs: 15, bootWaitMs: 0 }), error => error.code === 'serial_write_timeout');
  await new Promise(resolve => setTimeout(resolve, 300));
  assert.equal(writes, 1);
  assert.equal(closes, 1);
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

test('partial-save mode switch failure tells the user the pairing config was saved', () => {
  assert.equal(
    serialErrorMessage({ code: 'device_mode_switch_failed' }),
    '电脑采集配置已保存，但采集器来源启用失败。请重新打开 USB 设置。',
  );
});
