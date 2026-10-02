const encoder = new TextEncoder();

export class DeviceSerialError extends Error {
  constructor(code, diagnostics) {
    super(code);
    this.name = 'DeviceSerialError';
    this.code = code;
    this.diagnostics = { ...diagnostics };
  }
}

// Start receiving as soon as the port opens, including while pairing HTTP and
// USB writes are pending. Only diagnostic flags survive; raw input is discarded.
export function startDeviceSerial(port, requestId) {
  if (!port.readable || !port.writable) throw new DeviceSerialError('serial_unavailable', {});
  const reader = port.readable.getReader();
  let writer;
  try { writer = port.writable.getWriter(); } catch (error) { reader.releaseLock(); throw error; }
  const diagnostics = { received_data: false, ready_seen: false, result_seen: false, matching_result_seen: false };
  let resolveResult; let rejectResult; let resolveReady;
  let stopped = false; let settled = false; let terminalError = null;
  const result = new Promise((resolve, reject) => { resolveResult = resolve; rejectResult = reject; });
  // A read failure can occur before the caller finishes preparing its frame.
  result.catch(() => {});
  const ready = new Promise(resolve => { resolveReady = resolve; });
  const fail = code => {
    terminalError ??= new DeviceSerialError(code, diagnostics);
    if (!settled) { settled = true; rejectResult(terminalError); }
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
    if (frame?.op !== 'result' || typeof frame.ok !== 'boolean') return;
    diagnostics.result_seen = true;
    if (frame.request_id !== requestId || settled) return;
    diagnostics.matching_result_seen = true;
    settled = true;
    resolveResult(frame);
  };
  const pump = (async () => {
    const decoder = new TextDecoder();
    let line = ''; let discarding = false;
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
          if (encoder.encode(line + fragment).byteLength > 4096) {
            line = ''; discarding = true;
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
    async send(frame, { timeoutMs = 15000, bootWaitMs = 1500 } = {}) {
      if (frame.byteLength > 4096) throw new DeviceSerialError('frame_too_long', diagnostics);
      if (terminalError) throw terminalError;
      let bootTimer;
      try {
        await Promise.race([ready, new Promise(resolve => { bootTimer = setTimeout(resolve, bootWaitMs); })]);
      } finally { clearTimeout(bootTimer); }
      if (terminalError) throw terminalError;
      let responseTimer;
      try {
        return await Promise.race([
          (async () => {
            try { await writer.write(frame); } catch { throw new DeviceSerialError('serial_write_error', diagnostics); }
            return await result;
          })(),
          new Promise((_, reject) => {
            responseTimer = setTimeout(() => reject(new DeviceSerialError('serial_timeout', diagnostics)), timeoutMs);
          }),
        ]);
      } finally { clearTimeout(responseTimer); }
    },
    async close() {
      stopped = true;
      fail('serial_closed');
      try { await reader.cancel(); } catch { /* the device may have disconnected */ }
      await pump;
      try { reader.releaseLock(); } catch { /* already released */ }
      try { await writer.abort(); } catch { /* the device may have disconnected */ }
      try { writer.releaseLock(); } catch { /* already released */ }
    },
  };
}

export function serialErrorMessage(error) {
  if (error?.code === 'serial_timeout') {
    const flags = error.diagnostics;
    if (!flags?.received_data) return '未收到设备的 USB 回应。请重新插拔 USB 线后重试，并选择 USB JTAG/serial debug unit。';
    if (flags.result_seen) return '已收到设备回复，但配置请求未得到对应确认。请刷新此网页并重新配对。';
    if (flags.ready_seen) return '设备已启动，但没有确认配置。请重新打开小屏配对窗口后重试。';
    return '已收到 USB 信息，但设备未完成配置。请重新插拔 USB 线，等待小屏启动后重试。';
  }
  if (['serial_read_error', 'serial_write_error', 'serial_closed'].includes(error?.code)) return 'USB 连接中断。请关闭占用设备的串口工具，重新插拔 USB 线后重试。';
  const rejected = {
    pairing_closed: '小屏配对窗口已关闭。请在小屏重新打开配对窗口后重试。',
    invalid_config: '设备收到配置，但信息无效。请检查 Wi‑Fi 名称和本机地址后重试。',
    storage_error: '设备收到配置，但保存失败。请重启小屏后重试。',
    frame_too_long: '配置内容超过设备协议允许的大小。',
    unsupported_version: '网页与设备固件版本不匹配。请更新到配套版本。',
  };
  if (error?.code?.startsWith('device_')) return rejected[error.code.slice(7)] ?? '设备收到配置，但拒绝了此次请求。请重新配对。';
  return null;
}
