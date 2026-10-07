const encoder = new TextEncoder();
const REQUEST_ID = /^[a-f0-9]{8}$/;
const SESSION_ID = /^[a-f0-9]{32}$/;
const DEFAULT_LINE_BYTES = 32768;

export class DeviceSerialError extends Error {
  constructor(code, diagnostics = {}) {
    super(code);
    this.name = 'DeviceSerialError';
    this.code = code;
    this.diagnostics = { ...diagnostics };
  }
}

export function makeUsbRequestId() {
  const bytes = new Uint8Array(4);
  if (!globalThis.crypto?.getRandomValues) throw new DeviceSerialError('serial_crypto_unavailable');
  crypto.getRandomValues(bytes);
  return Array.from(bytes, value => value.toString(16).padStart(2, '0')).join('');
}

function startReader(port, { maxLineBytes = DEFAULT_LINE_BYTES } = {}) {
  if (!port.readable || !port.writable) throw new DeviceSerialError('serial_unavailable');
  const reader = port.readable.getReader();
  let writer;
  try { writer = port.writable.getWriter(); } catch (error) { reader.releaseLock(); throw error; }

  const diagnostics = { received_data: false, ready_seen: false, result_seen: false, matching_result_seen: false };
  const pending = new Map();
  let resolveReady;
  let stopped = false;
  let terminalError = null;
  const ready = new Promise(resolve => { resolveReady = resolve; });

  const fail = code => {
    terminalError ??= new DeviceSerialError(code, diagnostics);
    for (const request of pending.values()) {
      clearTimeout(request.timer);
      request.reject(terminalError);
    }
    pending.clear();
  };

  const consumeLine = line => {
    const normalized = line.endsWith('\r') ? line.slice(0, -1) : line;
    if (normalized.includes('ai_quota: ready')) {
      diagnostics.ready_seen = true;
      resolveReady();
    }
    if (!normalized.startsWith('@AIQ:')) return;
    let frame;
    try { frame = JSON.parse(normalized.slice(5)); } catch { return; }
    if (!['result', 'state'].includes(frame?.op) || typeof frame.ok !== 'boolean') return;
    diagnostics.result_seen = true;
    const request = pending.get(frame.request_id);
    if (!request) return;
    diagnostics.matching_result_seen = true;
    pending.delete(frame.request_id);
    clearTimeout(request.timer);
    request.resolve(frame);
  };

  const pump = (async () => {
    const decoder = new TextDecoder();
    let line = '';
    let discarding = false;
    while (!stopped) {
      const { value, done } = await reader.read();
      if (done) { if (!stopped) fail('serial_closed'); return; }
      if (value?.byteLength) diagnostics.received_data = true;
      const text = decoder.decode(value, { stream: true });
      let start = 0;
      for (let index = 0; index <= text.length; index += 1) {
        if (index !== text.length && text[index] !== '\n') continue;
        const ended = index < text.length;
        if (!discarding) {
          const fragment = text.slice(start, index);
          if (encoder.encode(line + fragment).byteLength > maxLineBytes) {
            line = '';
            discarding = true;
          } else {
            line += fragment;
            if (ended) { consumeLine(line); line = ''; }
          }
        }
        if (ended) discarding = false;
        start = index + 1;
      }
    }
  })().catch(() => { if (!stopped) fail('serial_read_error'); });

  return {
    async send(frameBytes, requestId, { timeoutMs = 15000, bootWaitMs = 1500, onWritten } = {}) {
      if (!(frameBytes instanceof Uint8Array)) frameBytes = new Uint8Array(frameBytes);
      if (frameBytes.byteLength > 4096) throw new DeviceSerialError('frame_too_long', diagnostics);
      if (!REQUEST_ID.test(requestId ?? '')) throw new DeviceSerialError('serial_invalid_request_id', diagnostics);
      if (terminalError) throw terminalError;
      if (pending.has(requestId)) throw new DeviceSerialError('serial_request_busy', diagnostics);

      let resolve;
      let reject;
      const response = new Promise((res, rej) => { resolve = res; reject = rej; });
      const current = { resolve, reject, timer: null, writePending: true };
      pending.set(requestId, current);

      let bootTimer;
      try {
        await Promise.race([ready, new Promise(done => { bootTimer = setTimeout(done, bootWaitMs); })]);
      } finally { clearTimeout(bootTimer); }
      if (terminalError) { pending.delete(requestId); throw terminalError; }

      current.timer = setTimeout(() => {
        pending.delete(requestId);
        const error = new DeviceSerialError(current.writePending ? 'serial_write_timeout' : 'serial_timeout', diagnostics);
        if (current.writePending) terminalError ??= error;
        reject(error);
      }, timeoutMs);
      const write = writer.write(frameBytes).then(() => {
        current.writePending = false;
        onWritten?.();
        return response;
      }, () => {
        current.writePending = false;
        pending.delete(requestId);
        clearTimeout(current.timer);
        throw new DeviceSerialError('serial_write_error', diagnostics);
      });
      return Promise.race([write, response]);
    },
    async close() {
      stopped = true;
      fail('serial_closed');
      try { await reader.cancel(); } catch { /* the device may have disconnected */ }
      await pump;
      try { reader.releaseLock(); } catch { /* already released */ }
      try {
        await Promise.race([writer.abort(), new Promise(resolve => setTimeout(resolve, 250))]);
      } catch { /* the device may have disconnected */ }
      try { writer.releaseLock(); } catch { /* already released */ }
    },
  };
}

async function openPort(port) {
  let readerSession;
  let opened = false;
  let closePromise;
  const close = () => closePromise ??= (async () => {
    await readerSession?.close();
    if (opened) {
      opened = false;
      try { await port.close(); } catch { /* disconnected already */ }
    }
  })();
  try {
    await port.open({ baudRate: 115200, bufferSize: 4096, flowControl: 'none' });
    opened = true;
    readerSession = startReader(port);
    await port.setSignals({ dataTerminalReady: false, requestToSend: false });
    return {
      async send(frameBytes, requestId, options) {
        try { return await readerSession.send(frameBytes, requestId, options); }
        catch (error) { if (error?.code === 'serial_write_timeout') void close(); throw error; }
      },
      close,
    };
  } catch (error) {
    await close();
    throw error;
  }
}

export function startDeviceSerial(port, requestId) {
  if (!REQUEST_ID.test(requestId ?? '')) throw new DeviceSerialError('serial_invalid_request_id');
  const session = startReader(port);
  return { send: (frameBytes, options) => session.send(frameBytes, requestId, options), close: session.close };
}

// The primary settings page opens this once before the user starts the device's
// physical USB Settings window. The same port remains open for session_open.
export async function openUsbDeviceSession(port, options = {}) {
  const serial = await openPort(port);
  return new UsbDeviceSession(serial, options);
}

export class UsbDeviceSession {
  constructor(serial, { requestId = makeUsbRequestId, now = () => Date.now() } = {}) {
    this.serial = serial;
    this.requestId = requestId;
    this.now = now;
    this.openerId = this.requestId();
    this.sessionId = '';
    this.sessionExpiresAt = 0;
    this.limits = null;
    this.pendingMutation = null;
    this.closed = false;
  }

  resetSession() {
    if (this.closed) throw new DeviceSerialError('serial_closed');
    this.openerId = this.requestId();
    this.sessionId = '';
    this.sessionExpiresAt = 0;
    this.limits = null;
    this.pendingMutation = null;
  }

  async exchange(frame, { timeoutMs = 15000, retryOnce = false, maxFrameBytes = 4096, onWritten } = {}) {
    const encoded = encoder.encode(`@AIQ:${JSON.stringify(frame)}\n`);
    if (encoded.byteLength > maxFrameBytes) throw new DeviceSerialError('frame_too_long');
    const send = () => this.serial.send(encoded, frame.request_id, { timeoutMs, bootWaitMs: 0, onWritten });
    try { return await send(); } catch (error) {
      if (!retryOnce || error?.code !== 'serial_timeout' || this.closed) throw error;
      return send();
    }
  }

  async openSession() {
    if (this.closed) throw new DeviceSerialError('serial_closed');
    if (this.sessionId) return { session_id: this.sessionId, ...this.limits };
    const result = await this.exchange({ v: 2, op: 'session_open', request_id: this.openerId }, { retryOnce: true });
    if (!result.ok) throw new DeviceSerialError(`device_${result.error_code ?? result.error ?? 'rejected'}`);
    if (!SESSION_ID.test(result.session_id ?? '')
      || !Number.isInteger(result.remaining_seconds) || result.remaining_seconds <= 0
      || !Number.isInteger(result.max_command_bytes) || !Number.isInteger(result.max_frame_bytes)
      || !Number.isInteger(result.max_state_bytes)) {
      throw new DeviceSerialError('usb_invalid_session_response');
    }
    this.sessionId = result.session_id;
    this.sessionExpiresAt = this.now() + result.remaining_seconds * 1000;
    this.limits = {
      remaining_seconds: result.remaining_seconds,
      max_command_bytes: Math.min(result.max_command_bytes, 2048),
      max_frame_bytes: Math.min(result.max_frame_bytes, 4096),
      max_state_bytes: Math.min(result.max_state_bytes, 16384),
    };
    return { session_id: this.sessionId, ...this.limits };
  }

  assertSession(result) {
    if (result.session_id !== this.sessionId) throw new DeviceSerialError('usb_invalid_session_response');
  }

  assertSessionNotExpired() {
    if (this.sessionId && this.now() >= this.sessionExpiresAt) throw new DeviceSerialError('device_session_expired');
  }

  async stateGet() {
    if (!this.sessionId) throw new DeviceSerialError('usb_session_not_open');
    this.assertSessionNotExpired();
    const result = await this.exchange({ v: 2, op: 'state_get', request_id: this.requestId(), session_id: this.sessionId }, {
      timeoutMs: 15000, maxFrameBytes: this.limits.max_frame_bytes,
    });
    if (!result.ok) throw new DeviceSerialError(`device_${result.error_code ?? result.error ?? 'rejected'}`);
    this.assertSession(result);
    if (!result.state || typeof result.state !== 'object' || Array.isArray(result.state)) throw new DeviceSerialError('usb_invalid_state');
    const stateBytes = encoder.encode(JSON.stringify(result.state)).byteLength;
    if (stateBytes > this.limits.max_state_bytes) throw new DeviceSerialError('usb_state_too_large');
    const seconds = result.state.session?.remaining_seconds;
    if (Number.isInteger(seconds) && seconds >= 0) this.sessionExpiresAt = this.now() + seconds * 1000;
    if (this.pendingMutation && this.jobProvesAdmission(result.state, this.pendingMutation.requestId)) this.pendingMutation = null;
    return result.state;
  }

  jobProvesAdmission(state, requestId) {
    return (state.jobs ?? []).some(job => job.request_id === requestId)
      || state.operation?.request_id === requestId;
  }

  async mutation(frame) {
    if (this.closed) throw new DeviceSerialError('serial_closed');
    if (!this.sessionId) throw new DeviceSerialError('usb_session_not_open');
    this.assertSessionNotExpired();
    if (this.pendingMutation) throw new DeviceSerialError('usb_mutation_unresolved');
    if (!REQUEST_ID.test(frame.request_id ?? '')) throw new DeviceSerialError('serial_invalid_request_id');
    const bodyBytes = encoder.encode(JSON.stringify(frame.body)).byteLength;
    if (bodyBytes > this.limits.max_command_bytes) throw new DeviceSerialError('usb_command_too_large');
    const packet = { v: 2, op: frame.op, request_id: frame.request_id, session_id: this.sessionId, body: frame.body };
    const pending = { requestId: frame.request_id, sessionId: this.sessionId };
    this.pendingMutation = pending;
    const remainingMs = () => Math.max(0, this.sessionExpiresAt - this.now());
    // onTransmit fires only once a frame has been written to the port; a rejection before that never reached the device.
    const send = timeoutMs => this.exchange(packet, { timeoutMs, maxFrameBytes: this.limits.max_frame_bytes, onWritten: frame.onTransmit });
    let result;
    let retried = false;
    try {
      result = await send(Math.min(90000, remainingMs()));
    } catch (error) {
      if (error?.code !== 'serial_timeout' || this.closed || pending.sessionId !== this.sessionId || remainingMs() <= 0) {
        throw error;
      }
      retried = true;
      try {
        result = await send(Math.min(90000, remainingMs()));
      } catch {
        throw new DeviceSerialError('usb_uncertain_receipt', { retry_used: true });
      }
    }
    if (!result.ok) {
      this.pendingMutation = null;
      // retry_used: a rejection after a resend may answer the resend while the first write was already processed.
      throw new DeviceSerialError(`device_${result.error_code ?? result.error ?? 'rejected'}`, { retry_used: retried });
    }
    this.assertSession(result);
    if (result.accepted !== true || result.request_id !== frame.request_id) {
      throw new DeviceSerialError('usb_invalid_mutation_ack');
    }
    this.pendingMutation = null;
    return result;
  }

  command(body) {
    return this.mutation({ op: 'command', request_id: body.request_id, body });
  }

  async close() {
    if (this.closed) return;
    this.closed = true;
    this.sessionId = '';
    this.pendingMutation = null;
    await this.serial.close();
  }
}

export function serialErrorMessage(error) {
  if (['serial_timeout', 'usb_uncertain_receipt', 'usb_mutation_unresolved'].includes(error?.code)) {
    if (error.code === 'serial_timeout') {
      const flags = error.diagnostics;
      if (!flags?.received_data) return '未收到设备回应。请确认 USB 已连接，再打开「设备设置 → USB 设置」窗口后重试。';
      if (flags.result_seen) return '已收到设备回复，但没有匹配的确认。请重新读取设备状态。';
      if (flags.ready_seen) return '设备已启动，但没有确认操作。请检查 USB 设置窗口是否仍打开。';
      return '已收到 USB 信息，但没有确认。请重新读取设备状态。';
    }
    return '设备是否收到这次操作尚未确认。页面不会再次发送新的修改；请重新读取设备状态，或断开 USB 后重新打开「USB 设置」窗口。';
  }
  if (['serial_read_error', 'serial_write_error', 'serial_closed'].includes(error?.code)) return 'USB 连接中断。请关闭占用设备的串口工具，重新插拔 USB 线后重试。';
  const rejected = {
    frame_too_long: '配置内容超过设备协议允许的大小。',
    unsupported_version: '网页与设备固件版本不匹配。请更新到配套版本。',
    session_busy: '设备刚刚由另一个 USB 设置页面连接。请关闭另一个页面，或等待数秒后重试。',
    invalid_session: '已在另一个页面继续设置，或设备窗口已重新打开。如需在此页面继续，请重新连接设备状态。',
    session_expired: 'USB 设置窗口已过期。请重新打开窗口并连接设备状态。',
  };
  if (error?.code?.startsWith('device_')) return rejected[error.code.slice(7)] ?? '设备收到配置，但拒绝了此次请求。请重新打开 USB 设置后重试。';
  return null;
}
