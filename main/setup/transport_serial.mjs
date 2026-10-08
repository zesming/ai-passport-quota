const encoder = new TextEncoder();
const REQUEST_ID = /^[a-f0-9]{8}$/;
const SESSION_ID = /^[a-f0-9]{32}$/;
const DEFAULT_LINE_BYTES = 32768;
// The setup protocol this page speaks, and the USB identity of a Passport.
export const SERIAL_PROTOCOL = 3;
export const SESSION_OPEN_TRY_MS = 1500;
const STATE_TRY_MS = 5000;
export const PASSPORT_USB_FILTER = { usbVendorId: 0x303a, usbProductId: 0x1001 };

export class DeviceSerialError extends Error {
  // `device` carries the protocol and firmware a rejecting Passport reported, if it did.
  constructor(code, diagnostics = {}, device = null) {
    super(code);
    this.name = 'DeviceSerialError';
    this.code = code;
    this.diagnostics = { ...diagnostics };
    this.device = device;
  }
}

function deviceError(result, diagnostics = {}) {
  return new DeviceSerialError(
    `device_${result.error_code ?? result.error ?? 'rejected'}`,
    diagnostics,
    {
      protocol: result.protocol,
      firmware: result.firmware,
    },
  );
}

export function makeUsbRequestId() {
  const bytes = new Uint8Array(4);
  if (!globalThis.crypto?.getRandomValues) throw new DeviceSerialError('serial_crypto_unavailable');
  crypto.getRandomValues(bytes);
  return Array.from(bytes, (value) => value.toString(16).padStart(2, '0')).join('');
}

function startReader(port, { maxLineBytes = DEFAULT_LINE_BYTES } = {}) {
  if (!port.readable || !port.writable) throw new DeviceSerialError('serial_unavailable');
  const reader = port.readable.getReader();
  let writer;
  try {
    writer = port.writable.getWriter();
  } catch (error) {
    reader.releaseLock();
    throw error;
  }

  const diagnostics = {
    received_data: false,
    result_seen: false,
    matching_result_seen: false,
  };
  const pending = new Map();
  let stopped = false;
  let terminalError = null;

  const fail = (code) => {
    terminalError ??= new DeviceSerialError(code, diagnostics);
    for (const request of pending.values()) {
      clearTimeout(request.timer);
      request.reject(terminalError);
    }
    pending.clear();
  };

  const consumeLine = (line) => {
    const normalized = line.endsWith('\r') ? line.slice(0, -1) : line;
    if (!normalized.startsWith('@AIQ:')) return;
    let frame;
    try {
      frame = JSON.parse(normalized.slice(5));
    } catch {
      return;
    }
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
      if (done) {
        if (!stopped) fail('serial_closed');
        return;
      }
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
            if (ended) {
              consumeLine(line);
              line = '';
            }
          }
        }
        if (ended) discarding = false;
        start = index + 1;
      }
    }
  })().catch(() => {
    if (!stopped) fail('serial_read_error');
  });

  return {
    async send(
      frameBytes,
      requestId,
      { timeoutMs = 15000, onWritten, fatalWriteTimeout = true } = {},
    ) {
      if (!(frameBytes instanceof Uint8Array)) frameBytes = new Uint8Array(frameBytes);
      if (frameBytes.byteLength > 4096) throw new DeviceSerialError('frame_too_long', diagnostics);
      if (!REQUEST_ID.test(requestId ?? ''))
        throw new DeviceSerialError('serial_invalid_request_id', diagnostics);
      if (terminalError) throw terminalError;
      if (pending.has(requestId)) throw new DeviceSerialError('serial_request_busy', diagnostics);

      let resolve;
      let reject;
      const response = new Promise((res, rej) => {
        resolve = res;
        reject = rej;
      });
      const current = { resolve, reject, timer: null, writePending: true };
      pending.set(requestId, current);

      current.timer = setTimeout(() => {
        pending.delete(requestId);
        const error = new DeviceSerialError(
          current.writePending ? 'serial_write_timeout' : 'serial_timeout',
          diagnostics,
        );
        // A write that stays blocked is fatal unless the caller can live with it: the Passport
        // does not read the port while its USB setting is closed, so it is not a broken link.
        if (current.writePending && fatalWriteTimeout) terminalError ??= error;
        current.timedOut = true;
        reject(error);
      }, timeoutMs);
      const write = writer.write(frameBytes).then(
        () => {
          current.writePending = false;
          onWritten?.();
          return current.timedOut ? undefined : response;
        },
        () => {
          current.writePending = false;
          pending.delete(requestId);
          clearTimeout(current.timer);
          throw new DeviceSerialError('serial_write_error', diagnostics);
        },
      );
      return Promise.race([write, response]);
    },
    async close() {
      stopped = true;
      fail('serial_closed');
      try {
        await reader.cancel();
      } catch {
        /* the device may have disconnected */
      }
      await pump;
      try {
        reader.releaseLock();
      } catch {
        /* already released */
      }
      try {
        await Promise.race([writer.abort(), new Promise((resolve) => setTimeout(resolve, 250))]);
      } catch {
        /* the device may have disconnected */
      }
      try {
        writer.releaseLock();
      } catch {
        /* already released */
      }
    },
  };
}

async function openPort(port) {
  let readerSession;
  let opened = false;
  let closePromise;
  const close = () =>
    (closePromise ??= (async () => {
      await readerSession?.close();
      if (opened) {
        opened = false;
        try {
          await port.close();
        } catch {
          /* disconnected already */
        }
      }
    })());
  try {
    await port.open({ baudRate: 115200, bufferSize: 4096, flowControl: 'none' });
    opened = true;
    readerSession = startReader(port);
    await port.setSignals({ dataTerminalReady: false, requestToSend: false });
    return {
      async send(frameBytes, requestId, options) {
        try {
          return await readerSession.send(frameBytes, requestId, options);
        } catch (error) {
          if (error?.code === 'serial_write_timeout' && options?.fatalWriteTimeout !== false)
            void close();
          throw error;
        }
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
  return {
    send: (frameBytes, options) => session.send(frameBytes, requestId, options),
    close: session.close,
  };
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

  // Forget the session. With `sameOpener` the next session_open carries the same request id: a
  // Passport that did not restart gives the same session back, one that did has no opener yet.
  resetSession({ sameOpener = false } = {}) {
    if (this.closed) throw new DeviceSerialError('serial_closed');
    if (!sameOpener) this.openerId = this.requestId();
    this.sessionId = '';
    this.sessionExpiresAt = 0;
    this.limits = null;
    this.pendingMutation = null;
  }

  async exchange(
    frame,
    {
      timeoutMs = 15000,
      retryOnce = false,
      maxFrameBytes = 4096,
      onWritten,
      fatalWriteTimeout = true,
    } = {},
  ) {
    const encoded = encoder.encode(`@AIQ:${JSON.stringify(frame)}\n`);
    if (encoded.byteLength > maxFrameBytes) throw new DeviceSerialError('frame_too_long');
    const send = () =>
      this.serial.send(encoded, frame.request_id, { timeoutMs, onWritten, fatalWriteTimeout });
    try {
      return await send();
    } catch (error) {
      if (!retryOnce || error?.code !== 'serial_timeout' || this.closed) throw error;
      return send();
    }
  }

  // The Passport answers only while its USB setting is open, so a try that gets no answer within
  // `timeoutMs` just means "not open yet"; the caller tries again.
  async openSession({ timeoutMs = SESSION_OPEN_TRY_MS } = {}) {
    if (this.closed) throw new DeviceSerialError('serial_closed');
    if (this.sessionId) return { session_id: this.sessionId, ...this.limits };
    // A write that is still blocked has not reached the Passport: do not queue another behind it.
    if (this.openWriting) throw new DeviceSerialError('serial_timeout', { write_pending: true });
    this.openWriting = true;
    const result = await this.exchange(
      { v: SERIAL_PROTOCOL, op: 'session_open', request_id: this.openerId },
      {
        timeoutMs,
        fatalWriteTimeout: false,
        onWritten: () => {
          this.openWriting = false;
        },
      },
    ).catch((error) => {
      if (error?.code === 'serial_write_error') this.openWriting = false;
      throw error;
    });
    if (!result.ok) throw deviceError(result);
    if (
      !SESSION_ID.test(result.session_id ?? '') ||
      !Number.isInteger(result.remaining_seconds) ||
      result.remaining_seconds <= 0 ||
      !Number.isInteger(result.max_command_bytes) ||
      !Number.isInteger(result.max_frame_bytes) ||
      !Number.isInteger(result.max_state_bytes)
    ) {
      throw new DeviceSerialError('usb_invalid_session_response');
    }
    this.sessionId = result.session_id;
    this.protocol = result.protocol;
    this.firmware = result.firmware;
    this.sessionExpiresAt = this.now() + result.remaining_seconds * 1000;
    this.limits = {
      remaining_seconds: result.remaining_seconds,
      max_command_bytes: Math.min(result.max_command_bytes, 2048),
      max_frame_bytes: Math.min(result.max_frame_bytes, 4096),
      max_state_bytes: Math.min(result.max_state_bytes, 16384),
    };
    return {
      session_id: this.sessionId,
      protocol: this.protocol,
      firmware: this.firmware,
      ...this.limits,
    };
  }

  assertSession(result) {
    if (result.session_id !== this.sessionId)
      throw new DeviceSerialError('usb_invalid_session_response');
  }

  assertSessionNotExpired() {
    if (this.sessionId && this.now() >= this.sessionExpiresAt)
      throw new DeviceSerialError('device_session_expired');
  }

  async stateGet() {
    if (!this.sessionId) throw new DeviceSerialError('usb_session_not_open');
    this.assertSessionNotExpired();
    // A busy Passport reads late: a write that is still blocked is not repeated behind itself, and
    // a blocked write is not a broken link.
    if (this.stateWriting) throw new DeviceSerialError('serial_timeout', { write_pending: true });
    this.stateWriting = true;
    const result = await this.exchange(
      {
        v: SERIAL_PROTOCOL,
        op: 'state_get',
        request_id: this.requestId(),
        session_id: this.sessionId,
      },
      {
        timeoutMs: STATE_TRY_MS,
        maxFrameBytes: this.limits.max_frame_bytes,
        fatalWriteTimeout: false,
        onWritten: () => {
          this.stateWriting = false;
        },
      },
    ).catch((error) => {
      if (error?.code === 'serial_write_error') this.stateWriting = false;
      throw error;
    });
    if (!result.ok) throw deviceError(result);
    this.assertSession(result);
    if (!result.state || typeof result.state !== 'object' || Array.isArray(result.state))
      throw new DeviceSerialError('usb_invalid_state');
    const stateBytes = encoder.encode(JSON.stringify(result.state)).byteLength;
    if (stateBytes > this.limits.max_state_bytes)
      throw new DeviceSerialError('usb_state_too_large');
    const seconds = result.state.session?.remaining_seconds;
    if (Number.isInteger(seconds) && seconds >= 0)
      this.sessionExpiresAt = this.now() + seconds * 1000;
    if (
      this.pendingMutation &&
      this.jobProvesAdmission(result.state, this.pendingMutation.requestId)
    )
      this.pendingMutation = null;
    return result.state;
  }

  jobProvesAdmission(state, requestId) {
    return (
      (state.jobs ?? []).some((job) => job.request_id === requestId) ||
      state.operation?.request_id === requestId
    );
  }

  async mutation(frame) {
    if (this.closed) throw new DeviceSerialError('serial_closed');
    if (!this.sessionId) throw new DeviceSerialError('usb_session_not_open');
    this.assertSessionNotExpired();
    if (this.pendingMutation) throw new DeviceSerialError('usb_mutation_unresolved');
    if (!REQUEST_ID.test(frame.request_id ?? ''))
      throw new DeviceSerialError('serial_invalid_request_id');
    const bodyBytes = encoder.encode(JSON.stringify(frame.body)).byteLength;
    if (bodyBytes > this.limits.max_command_bytes)
      throw new DeviceSerialError('usb_command_too_large');
    const packet = {
      v: SERIAL_PROTOCOL,
      op: frame.op,
      request_id: frame.request_id,
      session_id: this.sessionId,
      body: frame.body,
    };
    const pending = { requestId: frame.request_id, sessionId: this.sessionId };
    this.pendingMutation = pending;
    const remainingMs = () => Math.max(0, this.sessionExpiresAt - this.now());
    // onTransmit fires only once a frame has been written to the port; a rejection before that
    // never reached the device.
    const send = (timeoutMs) =>
      this.exchange(packet, {
        timeoutMs,
        maxFrameBytes: this.limits.max_frame_bytes,
        onWritten: frame.onTransmit,
      });
    let result;
    let retried = false;
    try {
      result = await send(Math.min(90000, remainingMs()));
    } catch (error) {
      if (
        error?.code !== 'serial_timeout' ||
        this.closed ||
        pending.sessionId !== this.sessionId ||
        remainingMs() <= 0
      ) {
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
      // retry_used: a rejection after a resend may answer the resend while the first write was
      // already processed.
      throw deviceError(result, { retry_used: retried });
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
  if (
    ['serial_timeout', 'usb_uncertain_receipt', 'usb_mutation_unresolved'].includes(error?.code)
  ) {
    if (error.code === 'serial_timeout') {
      const flags = error.diagnostics;
      if (!flags?.received_data)
        return '没有收到 Passport 的回应。请确认 USB 线已连接，并在 Passport 上打开了 USB 设置。';
      if (flags.result_seen) return 'Passport 回复了，但没有匹配的确认。请重新读取状态。';
      return '收到了 USB 信息，但没有确认。请重新读取状态。';
    }
    return (
      'Passport 是否收到这次操作还不确定。页面不会再发送新的修改；请重新读取状态，' +
      '或断开 USB 后重新打开 USB 设置。'
    );
  }
  if (['serial_read_error', 'serial_write_error', 'serial_closed'].includes(error?.code))
    return 'USB 连接中断。请关闭占用设备的串口工具，重新插拔 USB 线后重试。';
  const rejected = {
    frame_too_long: '配置内容超过设备协议允许的大小。',
    session_busy: '另一个设置页面刚刚连接了 Passport。请关闭另一个页面，或等待数秒后重试。',
    invalid_session: '已在另一个页面继续设置，或 USB 设置已重新打开。请重新连接。',
    session_expired:
      'USB 设置还没有打开，或设置时间已到。' +
      '请在 Passport 上：长按 OK，按下键，再按 OK，然后重新连接。',
  };
  if (error?.code?.startsWith('device_'))
    return (
      rejected[error.code.slice(7)] ?? '设备收到配置，但拒绝了此次请求。请重新打开 USB 设置后重试。'
    );
  return null;
}

// The USB transport of the setup page. Opening the port may restart the Passport, so the page opens
// it first and the user opens the Passport's USB setting afterwards; the session is then tried
// again and again until the Passport answers. The port stays open while the setting comes and goes.
export function createSerialTransport(nav = globalThis.navigator) {
  let session = null;
  return {
    kind: 'serial',
    supported: Boolean(nav?.serial?.requestPort),
    hasPort: () => session !== null,
    hasSession: () => Boolean(session?.sessionId),
    async openPort() {
      if (session) return;
      const port = await nav.serial.requestPort({ filters: [PASSPORT_USB_FILTER] });
      session = await openUsbDeviceSession(port);
    },
    // One try: it fails with serial_timeout or device_session_expired while the setting is closed.
    openSession: () => session.openSession(),
    // The Passport ended the session, restarted, or another page took over: keep the port.
    sessionLost({ sameOpener = false } = {}) {
      session?.resetSession?.({ sameOpener });
    },
    stateGet: () => session.stateGet(),
    command: (body) => session.command(body),
    jobProvesAdmission: (state, requestId) => session?.jobProvesAdmission(state, requestId),
    async close() {
      const closing = session;
      session = null;
      if (closing) await closing.close();
    },
  };
}
