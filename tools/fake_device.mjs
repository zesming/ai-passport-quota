// A software Passport that speaks setup protocol v3 over both transports, or pretends to be a
// v2 firmware. The page tests and tools/preview_portable.mjs run the real setup page against it.
//
// Time is virtual: call device.advance(ms) to let validation make progress. Outcomes follow the
// data: a Wi-Fi password or DeepSeek key that contains "bad" is rejected; a ChatGPT authorization
// finishes when the test calls device.approveLogin().
//
// This file must stay free of Node-only APIs: the preview tool runs it inside a browser too.

const FRAME_PREFIX = '@AIQ:';
const BASE_MS = { usb: 120000, hotspot: 600000 };
const TOPUP_MS = 300000;
const CAP_MS = 1200000;
const MUTATIONS = [
  'network_save',
  'network_remove',
  'deepseek_save',
  'codex_queue',
  'account_remove',
  'settings_save',
  'validate',
];

export function createSimDevice(options = {}) {
  const config = {
    protocol: 3,
    firmware: '3.0.0-sim',
    code: 'K7QM-2X9D-PA4T-Z8RW',
    wifiMs: 800,
    keyMs: 600,
    loginMs: 900000,
    ...options,
  };
  let now = 0;
  let counter = 0;
  const device = {
    config,
    networks: [], // { ssid, password, validation, error_code }
    selected: 0,
    accounts: [], // { id, provider, label, email, validation, error_code, key, five_hour, ... }
    settings: { auto_refresh: true, refresh_seconds: 300, screen_timeout_seconds: 120 },
    jobs: [],
    queue: null, // { id, label, is_new, job }
    work: [],
    validating: false,
    step: '',
    stepLeft: 0,
    loginLeft: 0,
    loginError: '',
    loginAccount: '',
    session: null, // { kind, opened, deadline, id, opener }
    failures: 0,
    log: [], // every command the device took, for tests
    validateRuns: 0,
    failNext: '', // the next change fails with this error code
    bootingUntil: 0,
    stallJobs: false,
    stalledUntil: 0,
  };
  // 32 lower-case hexadecimal digits, like the Passport's own identifiers
  const make = (prefix) =>
    `${prefix}${(counter += 1).toString(16).padStart(8, '0')}`.padEnd(32, '0');
  const rejectEarly = (code, extra = {}) => ({ ok: false, error_code: code, ...extra });
  const ident = () => ({ protocol: config.protocol, firmware: config.firmware });

  function sessionActive() {
    return device.session !== null && now < device.session.deadline;
  }

  function topUp() {
    const s = device.session;
    s.deadline = Math.min(Math.max(s.deadline, now + TOPUP_MS), s.opened + CAP_MS);
  }

  function finishJob(id, error = '') {
    const job = device.jobs.find((item) => item.request_id === id);
    if (job && job.status !== 'succeeded' && job.status !== 'failed') {
      job.status = error ? 'failed' : 'succeeded';
      job.error_code = error;
    }
  }

  function pendingWork() {
    const work = [];
    device.networks.forEach(
      (net, index) =>
        ['pending', 'failed'].includes(net.validation) && work.push({ kind: 'wifi', index }),
    );
    device.accounts.forEach(
      (item) =>
        item.provider === 'deepseek' &&
        item.validation !== 'ok' &&
        work.push({ kind: 'key', id: item.id }),
    );
    if (device.queue) work.push({ kind: 'login' });
    return work;
  }

  function startValidation(job) {
    device.validateRuns += 1;
    device.work = pendingWork();
    if (!device.work.length) {
      finishJob(job);
      return;
    }
    device.validating = true;
    device.validateJob = job;
    device.stepLeft = 0;
    next();
  }

  function next() {
    const item = device.work[0];
    if (!item) {
      device.validating = false;
      device.step = '';
      finishJob(device.validateJob);
      return;
    }
    device.step = { wifi: 'wifi', key: 'deepseek', login: 'chatgpt' }[item.kind];
    device.stepLeft = { wifi: config.wifiMs, key: config.keyMs, login: config.loginMs }[item.kind];
  }

  function complete(item) {
    if (item.kind === 'wifi') {
      const net = device.networks[item.index];
      if (!net) return;
      // New credentials of the network in use are only a candidate until they work.
      const candidate = net.staged ?? net;
      if (/bad/.test(candidate.password))
        Object.assign(net, { validation: 'failed', error_code: 'wifi_auth_failed' });
      else {
        Object.assign(net, { ...candidate, staged: undefined, validation: 'ok', error_code: '' });
        device.selected = item.index;
      }
    } else if (item.kind === 'key') {
      const account = device.accounts.find((entry) => entry.id === item.id);
      if (!account) return;
      if (/^bad/.test(account.key))
        Object.assign(account, { validation: 'failed', error_code: 'deepseek_invalid_key' });
      else Object.assign(account, { validation: 'ok', error_code: '', balance: '10.00' });
    } else if (item.kind === 'login') {
      device.loginError = 'codex_expired';
      if (device.queue) device.queue.error = 'codex_expired'; // it stays queued, failed
      device.loginAccount = '';
      finishJob(device.validateJob, 'codex_expired');
    }
  }

  device.advance = (ms) => {
    const stalled = now < device.stalledUntil;
    now += ms;
    if (stalled && now >= device.stalledUntil) device.readListeners.forEach((l) => l('resume'));
    let left = ms;
    while (device.validating && left > 0) {
      const spent = Math.min(left, device.stepLeft);
      device.stepLeft -= spent;
      left -= spent;
      if (device.stepLeft > 0) break;
      complete(device.work.shift());
      next();
    }
  };

  device.approveLogin = () => {
    if (!device.validating || device.work[0]?.kind !== 'login' || !device.queue) return false;
    const queued = device.queue;
    const existing = device.accounts.find((entry) => entry.id === queued.id);
    if (existing) Object.assign(existing, { validation: 'ok', error_code: '' });
    else
      device.accounts.push({
        id: queued.id,
        provider: 'codex',
        label: queued.label,
        email: 'a***@mail.com',
        validation: 'ok',
        error_code: '',
        five_hour: { present: true, remaining_percent: 73 },
        seven_day: { present: true, remaining_percent: 41 },
      });
    device.queue = null;
    device.loginError = '';
    device.work.shift();
    next();
    return true;
  };

  // Physical actions on the Passport.
  // Called with 'window' when the USB setting opens (the firmware drops what arrived before) and
  // with 'resume' when a busy firmware reads again (what waited in the FIFO is processed).
  device.readListeners = [];
  // The network task is busy (for example with a slow HTTPS request) for `ms`: nothing is read.
  device.stall = (ms) => {
    device.stalledUntil = now + ms;
  };
  // Whether the firmware is reading the serial port: only while the USB setting is open.
  device.reading = () =>
    now >= device.bootingUntil &&
    now >= device.stalledUntil &&
    sessionActive() &&
    device.session.kind === 'usb';
  device.openUsb = () => {
    device.session = {
      kind: 'usb',
      opened: now,
      deadline: now + BASE_MS.usb,
      id: make('5e'),
      opener: '',
    };
    device.readListeners.forEach((listener) => listener('window'));
  };
  device.openHotspot = () => {
    device.session = {
      kind: 'hotspot',
      opened: now,
      deadline: now + BASE_MS.hotspot,
      id: '',
      opener: '',
    };
    device.failures = 0;
  };
  // The Passport restarts: RAM state and the USB setting are gone, and it is silent while booting.
  device.reset = () => {
    device.session = null;
    device.queue = null;
    device.validating = false;
    device.work = [];
    device.bootingUntil = now + 3000;
    for (const net of device.networks) {
      net.staged = undefined;
      net.validation = 'saved';
    }
  };
  device.closeSession = () => {
    device.session = null;
  };
  device.remainingSeconds = () =>
    sessionActive() ? Math.ceil((device.session.deadline - now) / 1000) : 0;

  // ---------------------------------------------------------------------------------------

  function state(kind) {
    const accounts = device.accounts.map((item) => ({
      id: item.id,
      provider: item.provider,
      label: item.label,
      error_code:
        item.id === device.queue?.id && device.queue.error ? device.queue.error : item.error_code,
      status: item.validation === 'ok' ? 'ok' : 'waiting',
      auth_state: item.validation === 'ok' ? 'ready' : 'pending',
      validation:
        item.id === device.queue?.id
          ? device.queue.error
            ? 'failed'
            : 'pending'
          : item.validation,
      retry_after_seconds: 0,
      email: item.email ?? '',
      plan: '',
      five_hour: item.five_hour ?? { present: false },
      seven_day: item.seven_day ?? { present: false },
      balance: {
        balance_infos: item.balance ? [{ currency: 'CNY', total_balance: item.balance }] : [],
      },
    }));
    if (device.queue?.is_new && !accounts.some((item) => item.id === device.queue.id))
      accounts.push({
        id: device.queue.id,
        provider: 'codex',
        label: device.queue.label,
        error_code: device.queue.error ?? '',
        status: 'waiting',
        auth_state: 'pending',
        validation: device.queue.error ? 'failed' : 'pending',
        retry_after_seconds: 0,
      });
    const body = {
      storage_error: '',
      validating: device.validating,
      validation_step: device.validating ? device.step : '',
      session: { remaining_seconds: device.remainingSeconds() },
      network: {
        connected: true,
        state: 'connected',
        ssid: device.networks[device.selected]?.ssid ?? '',
        ip: '192.168.1.23',
        saved_networks: device.networks.map((net, index) => ({
          index,
          ssid: net.staged?.ssid ?? net.ssid,
          selected: index === device.selected,
          validation: net.validation,
          error_code: net.error_code,
        })),
      },
      clock: { epoch: 1800000000 + Math.floor(now / 1000), synchronized: true },
      settings: { ...device.settings },
      accounts,
      operation: {
        kind: device.validating && device.step === 'chatgpt' ? 'login' : 'none',
        request_id: device.validateJob ?? '',
        state: 'waiting',
        seconds_left: Math.ceil(device.stepLeft / 1000),
        cancelable: device.validating && device.step === 'chatgpt',
      },
      jobs: device.jobs.slice(-8).map((job) => ({ ...job })),
    };
    if (config.protocol >= 3) Object.assign(body, ident());
    if (kind === 'usb')
      body.login = {
        state: device.loginError ? 'failed' : 'idle',
        verification_url: '',
        user_code: '',
        error_code: device.loginError,
        account_id: device.queue?.id ?? '',
        seconds_left: 0,
      };
    return body;
  }
  device.state = state;

  function commandError(body) {
    const op = body.op;
    if (device.validating && !['operation_cancel', 'setup_close'].includes(op)) return 'busy';
    return '';
  }

  // Returns { error } when the command is rejected before it becomes a job.
  function command(body, kind) {
    if (device.jobs.some((job) => job.request_id === body.request_id)) return {};
    if (body.op === 'validate' && kind !== 'usb') return { error: 'invalid_command' };
    const early = commandError(body);
    if (early) return { error: early };
    device.log.push(body);
    device.jobs.push({
      request_id: body.request_id,
      op: body.op,
      status: 'running',
      error_code: '',
    });
    if (MUTATIONS.includes(body.op)) topUp();
    if (device.stallJobs) return {}; // the job never finishes
    const fail = (code) => finishJob(body.request_id, code);
    if (device.failNext && MUTATIONS.includes(body.op)) {
      const code = device.failNext;
      device.failNext = '';
      fail(code);
      return {};
    }
    switch (body.op) {
      case 'network_save': {
        let index = Number.isInteger(body.network_index)
          ? body.network_index
          : device.networks.findIndex((net) => net.ssid === body.ssid);
        if (index < 0) index = device.networks.length;
        if (index >= 3) return fail('network_limit');
        if (index === device.selected && device.networks[index]) {
          // The network in use keeps its working credentials until the new ones validate.
          Object.assign(device.networks[index], {
            staged: { ssid: body.ssid, password: body.password },
            validation: 'pending',
            error_code: '',
          });
          finishJob(body.request_id);
          break;
        }
        device.networks[index] = {
          ssid: body.ssid,
          password: body.password,
          validation: 'pending',
          error_code: '',
        };
        finishJob(body.request_id);
        break;
      }
      case 'network_remove':
        if (!device.networks[body.network_index]) return fail('invalid_request');
        device.networks.splice(body.network_index, 1);
        device.selected = 0;
        finishJob(body.request_id);
        break;
      case 'deepseek_save': {
        const existing = device.accounts.find((item) => item.id === body.account_id);
        if (!existing && device.accounts.length + (device.queue?.is_new ? 1 : 0) >= 8)
          return fail('account_limit');
        if (existing) {
          existing.label = body.label;
          if (body.api_key)
            Object.assign(existing, { key: body.api_key, validation: 'pending', error_code: '' });
        } else
          device.accounts.push({
            id: make('d0'),
            provider: 'deepseek',
            label: body.label,
            key: body.api_key,
            validation: 'pending',
            error_code: '',
          });
        finishJob(body.request_id);
        break;
      }
      case 'codex_queue': {
        if (device.queue) {
          const same = body.account_id ? body.account_id === device.queue.id : device.queue.is_new;
          if (!device.queue.error || !same) return fail('login_pending');
          device.queue.error = ''; // a failed one is armed again
          finishJob(body.request_id);
          break;
        }
        const existing = device.accounts.find((item) => item.id === body.account_id);
        if (!existing && device.accounts.length >= 8) return fail('account_limit');
        device.queue = {
          id: existing ? existing.id : make('c0'),
          label: body.label ?? '',
          is_new: !existing,
          job: body.request_id,
        };
        finishJob(body.request_id);
        break;
      }
      case 'account_remove': {
        if (device.queue?.id === body.account_id) device.queue = null;
        device.accounts = device.accounts.filter((item) => item.id !== body.account_id);
        finishJob(body.request_id);
        break;
      }
      case 'settings_save':
        device.settings = {
          auto_refresh: body.auto_refresh,
          refresh_seconds: body.refresh_seconds,
          screen_timeout_seconds: body.screen_timeout_seconds,
        };
        finishJob(body.request_id);
        break;
      case 'validate':
        startValidation(body.request_id);
        break;
      case 'setup_close':
        finishJob(body.request_id);
        if (kind === 'hotspot') {
          device.session = null; // the access point closes; the validation then runs on its own
          startValidation('');
        }
        break;
      case 'operation_cancel':
        if (device.validating && device.step === 'chatgpt') {
          device.queue.error = 'canceled'; // the account stays queued, failed
          device.work.shift();
          device.loginError = 'canceled';
          finishJob(device.validateJob, 'canceled');
          next();
        }
        finishJob(body.request_id);
        break;
      default:
        finishJob(body.request_id);
    }
    return {};
  }

  // ---------------------------------------------------------------------------------------
  // USB transport: one @AIQ frame in, one frame out.

  device.frame = (frame) => {
    const reply = (extra) => ({
      v: config.protocol,
      op: 'result',
      request_id: frame.request_id,
      ...extra,
    });
    const error = (code) =>
      reply({ ok: false, error_code: code, ...(config.protocol >= 3 ? ident() : {}) });
    // The real firmware reads the serial port only while its USB setting is open (and booted).
    if (now < device.bootingUntil || !sessionActive() || device.session.kind !== 'usb') return null;
    if (frame.v !== config.protocol) return error('unsupported_version');
    const session = device.session;
    if (frame.op === 'session_open') {
      if (session.opener && session.opener !== frame.request_id) {
        if (now - (session.lastSeen ?? 0) < 6000) return error('session_busy');
        session.id = make('5f');
      }
      session.opener = frame.request_id;
      session.lastSeen = now;
      return reply({
        ok: true,
        session_id: session.id,
        remaining_seconds: device.remainingSeconds(),
        max_command_bytes: 2048,
        max_frame_bytes: 4096,
        max_state_bytes: 16384,
        ...ident(),
      });
    }
    if (frame.session_id !== session.id) return error('invalid_session');
    session.lastSeen = now;
    if (frame.op === 'state_get')
      return {
        v: config.protocol,
        op: 'state',
        request_id: frame.request_id,
        session_id: session.id,
        ok: true,
        state: state('usb'),
      };
    if (frame.op === 'command') {
      const result = command(frame.body, 'usb');
      if (result.error) return error(result.error);
      return reply({ ok: true, session_id: session.id, accepted: true, ...ident() });
    }
    return error('unsupported_operation');
  };

  // ---------------------------------------------------------------------------------------
  // Hotspot transport.

  device.http = (method, path, headers = {}, body = null) => {
    const respond = (status, json) => ({ status, json });
    const reject = (status, code) =>
      respond(status, { ok: false, error_code: code, ...(config.protocol >= 3 ? ident() : {}) });
    if (!sessionActive() || device.session.kind !== 'hotspot')
      return reject(403, 'session_expired');
    if (device.failures >= 5) return reject(403, 'access_locked');
    if (headers['X-AIQ-Access'] !== config.code) {
      if (headers['X-AIQ-Access']) device.failures += 1;
      return reject(403, device.failures >= 5 ? 'access_locked' : 'unauthorized');
    }
    device.failures = 0; // five wrong codes in a row, not over the whole session
    if (method === 'GET' && path === '/api/state') return respond(200, state('hotspot'));
    if (method === 'POST' && path === '/api/command') {
      const result = command(body, 'hotspot');
      if (result.error === 'busy') return reject(503, 'busy');
      if (result.error) return reject(400, result.error);
      return respond(202, { request_id: body.request_id, accepted: true });
    }
    return reject(404, 'not_found');
  };

  return device;
}

// ---------------------------------------------------------------------------------------
// Web Serial stand-in: `navigator.serial.requestPort()` returns a port wired to the device.

// `resetsOnOpen` models a host that restarts the Passport when it opens the serial port.
// `backpressure` models a firmware that does not read the port (USB setting closed, or busy): a
// write that fits the 64-byte hardware FIFO waits there, a larger one blocks; with 'buffered' the
// host buffers every write instead. A frame that waited in the FIFO of a busy firmware is processed
// when it reads again; what arrived before the USB setting opened is dropped.
export function createSimSerial(device, { resetsOnOpen = false, backpressure = false } = {}) {
  const decoder = new TextDecoder();
  const encoder = new TextEncoder();
  const port = {
    opened: false,
    async open() {
      if (resetsOnOpen) device.reset();
      let controller;
      port.readable = new ReadableStream({
        start(c) {
          controller = c;
        },
      });
      let buffered = '';
      const deliver = (chunk) => {
        buffered += decoder.decode(chunk, { stream: true });
        let newline;
        while ((newline = buffered.indexOf('\n')) >= 0) {
          const line = buffered.slice(0, newline);
          buffered = buffered.slice(newline + 1);
          if (!line.startsWith(FRAME_PREFIX)) continue;
          const response = device.frame(JSON.parse(line.slice(FRAME_PREFIX.length)));
          if (response)
            controller.enqueue(encoder.encode(`${FRAME_PREFIX}${JSON.stringify(response)}\n`));
        }
      };
      let fifo = 0;
      const queued = [];
      const waiting = [];
      if (backpressure)
        device.readListeners.push((kind) => {
          if (kind === 'resume') queued.splice(0).forEach(deliver);
          else queued.length = 0;
          fifo = 0;
          waiting.splice(0).forEach((release) => release());
        });
      port.writable = new WritableStream({
        async write(chunk) {
          if (backpressure && !device.reading()) {
            if (backpressure === 'buffered' || fifo + chunk.byteLength <= 64) {
              fifo += chunk.byteLength;
              queued.push(chunk);
              return;
            }
            await new Promise((resolve) => waiting.push(resolve));
          }
          deliver(chunk);
        },
      });
      port.opened = true;
    },
    async close() {
      port.opened = false;
    },
    async setSignals() {},
  };
  return {
    requested: 0,
    async requestPort(options) {
      this.requested += 1;
      this.lastOptions = options;
      return port;
    },
    port,
  };
}
// ---------------------------------------------------------------------------------------
// Ready-made situations for the preview tool.

export const SCENARIOS = {
  default(device) {
    device.networks.push({
      ssid: 'Home Wi-Fi',
      password: 'password',
      validation: 'saved', // as after a restart
      error_code: '',
    });
    device.accounts.push(
      {
        id: 'c0' + '1'.repeat(30),
        provider: 'codex',
        label: 'Work',
        email: 'a***@mail.com',
        validation: 'ok',
        error_code: '',
        five_hour: { present: true, remaining_percent: 73 },
        seven_day: { present: true, remaining_percent: 41 },
      },
      {
        id: 'd0' + '1'.repeat(30),
        provider: 'deepseek',
        label: 'Notes',
        key: 'sk-good',
        validation: 'ok',
        error_code: '',
        balance: '123.45',
      },
    );
  },
  empty() {},
  pending(device) {
    SCENARIOS.default(device);
    device.networks.push({
      ssid: 'Office',
      password: 'password',
      validation: 'pending',
      error_code: '',
    });
    device.accounts.push({
      id: 'd0' + '2'.repeat(30),
      provider: 'deepseek',
      label: 'Research',
      key: 'sk-new',
      validation: 'pending',
      error_code: '',
    });
  },
  failed(device) {
    SCENARIOS.default(device);
    device.networks.push({
      ssid: 'Office',
      password: 'badpassword',
      validation: 'failed',
      error_code: 'wifi_auth_failed',
    });
    device.accounts.push({
      id: 'd0' + '3'.repeat(30),
      provider: 'deepseek',
      label: 'Research',
      key: 'bad-key',
      validation: 'failed',
      error_code: 'deepseek_invalid_key',
    });
  },
  full(device) {
    SCENARIOS.default(device);
    for (let i = 1; i < 3; i += 1)
      device.networks.push({
        ssid: `Wi-Fi ${i}`,
        password: 'password',
        validation: 'ok',
        error_code: '',
      });
    for (let i = 2; i < 8; i += 1)
      device.accounts.push({
        id: `d${i}` + '0'.repeat(30),
        provider: 'deepseek',
        label: `Account ${i + 1}`,
        key: 'sk-good',
        validation: 'ok',
        error_code: '',
        balance: '1.00',
      });
  },
};
