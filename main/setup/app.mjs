// The Passport setup page: one screen for accounts, Wi-Fi and device settings.
//
// Two transports reach the same device protocol (v3). The page chooses by how it was opened:
// from the Passport's own hotspot (http:) it talks HTTP; from GitHub Pages or a file (https:,
// file:) it talks over USB with Web Serial. Wi-Fi passwords and DeepSeek keys stay in page memory
// and go straight to the Passport; nothing is written to browser storage.
import { createHttpTransport } from './transport_http.mjs';
import {
  createSerialTransport,
  serialErrorMessage,
  SESSION_OPEN_TRY_MS,
} from './transport_serial.mjs';

const PROTOCOL = 3;
const README_URL = 'https://github.com/zesming/ai-passport-quota#readme';
const LIMITS = { accounts: 8, networks: 3 };
const REFRESH_MANUAL = 'manual';
const POLL_MS = 2000;
const SESSION_RETRY_MS = 1500;
const SESSION_BUSY_RETRY_MS = 6000; // the Passport frees an idle opener after about 6 seconds
// A Passport busy with a slow request stays silent for up to 15 seconds; only longer silence
// counts as a restart.
const SILENT_RESTART_MS = 20000;
const POLL_VALIDATING_MS = 900;
const JOB_WAIT_MS = 20000;
const REMOVE_CONFIRM_MS = 3000;

const textEncoder = new TextEncoder();
const textBytes = (value) => textEncoder.encode(value).length;
const byId = (id) => document.getElementById(id);
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const MESSAGES = {
  wifi_auth_failed: 'Wi‑Fi 密码错误或信号弱',
  wifi_not_found: '找不到这个 Wi‑Fi，请确认名称，并使用 2.4 GHz 网络',
  network_unavailable: 'Passport 没能联网',
  time_required: 'Passport 还没有校准时间，请稍后重试',
  deepseek_invalid_key: '密钥无效',
  codex_expired: '授权超时',
  auth_required: '授权已过期，需要重新授权',
  busy: 'Passport 正在验证，请稍候',
  login_pending: '已有一个 ChatGPT 账户在等待授权，请先点“完成设置”',
  session_expired: '设置时间已到',
  job_timeout: 'Passport 还没有确认这次操作。请看一下列表里的状态，再决定要不要重试',
  account_limit: '最多 8 个账户，请先移除一个',
  network_limit: '最多 3 个 Wi‑Fi，请先移除一个',
  network_ambiguous: '有同名的 Wi‑Fi，请先移除多余的',
  access_locked: '访问码错误次数过多，请在 Passport 上重新开启热点设置',
  unauthorized: '访问码不对，请对照 Passport 屏幕',
  rate_limited: '请求过多，请稍后重试',
  canceled: '已取消',
  configuration_changed: 'Passport 的设置刚刚变化，请再试一次',
  invalid_request: '输入不符合要求，请检查后重试',
  invalid_command: '输入不符合要求，请检查后重试',
  request_conflict: '这次操作的编号重复，请再试一次',
  no_memory: 'Passport 内存不足，请稍后重试',
  resource_error: 'Passport 资源不足，请稍后重试',
  tls_error: '安全连接失败，请稍后重试',
  provider_response_invalid: '服务返回了无法识别的内容，请稍后重试',
  response_too_large: '服务返回的内容过大',
  login_disabled: '请先在 ChatGPT 账户设置里开启设备码授权',
  storage_failed: 'Passport 没能保存，请稍后重试',
  storage_write_unknown: 'Passport 正在确认保存结果，请稍候',
  storage_busy: 'Passport 正在处理存储，请稍后重试',
  storage_invalid: 'Passport 的存储记录损坏，请保留数据并检查设备',
  storage_io_error: 'Passport 的存储暂时不可读，请稍后重试',
  recovery_conflict: 'Passport 的存储记录不一致，请保留数据并检查设备',
  generation_exhausted: '这个账户已保存太多次，请移除后重新添加',
  native_credential_limit: '凭证位置已满，请先移除一个账户',
  operation_expired: '操作超时，请重试',
  unsupported_operation: 'Passport 不支持这个操作',
  serial_busy: '串口被占用，请关闭其他设置页或串口工具',
  http_unreachable: '连不上 Passport，请确认手机仍连着 Passport 的热点',
  http_bad_response: 'Passport 返回了无法识别的内容，请重试',
};
// Row errors whose recovery is a button on the row itself.
const WIFI_RECOVERY = '点“修改”重新输入';
const HOTSPOT_WIFI_RECOVERY = '在 Passport 上重新开启热点设置后再修改';

const LOGOS = {
  codex:
    '<svg viewBox="0 0 24 24" width="28" height="28" fill="currentColor" aria-hidden="true">' +
    '<path d="M22.28 9.82a5.98 5.98 0 0 0-.52-4.91 6.05 6.05 0 0 0-6.51-2.9A6.07 6.07 0 0 0 ' +
    '4.98 4.18a5.98 5.98 0 0 0-4 2.9 6.05 6.05 0 0 0 .74 7.1 5.98 5.98 0 0 0 .51 4.91 6.05 ' +
    '6.05 0 0 0 6.51 2.9A5.98 5.98 0 0 0 13.26 24a6.06 6.06 0 0 0 5.77-4.21 5.99 5.99 0 0 0 ' +
    '4-2.9 6.06 6.06 0 0 0-.75-7.07zM13.26 22.43a4.48 4.48 0 0 1-2.88-1.04l.14-.08 4.78-2.76a' +
    '.8.8 0 0 0 .39-.68v-6.74l2.02 1.17a.07.07 0 0 1 .04.05v5.58a4.5 4.5 0 0 1-4.49 4.5zM3.6 ' +
    '18.3a4.47 4.47 0 0 1-.53-3.01l.14.09 4.78 2.76a.77.77 0 0 0 .78 0l5.84-3.37v2.33a.08.08 ' +
    '0 0 1-.03.06L9.74 19.95a4.5 4.5 0 0 1-6.14-1.65zM2.34 7.9a4.49 4.49 0 0 1 2.37-1.97V11.6' +
    'a.77.77 0 0 0 .39.68l5.81 3.35-2.02 1.17a.08.08 0 0 1-.07 0L3.99 14.1A4.5 4.5 0 0 1 2.34 ' +
    '7.87zm16.6 3.86-5.83-3.39L15.12 7.2a.08.08 0 0 1 .07 0l4.83 2.79a4.49 4.49 0 0 1-.68 ' +
    '8.1v-5.68a.79.79 0 0 0-.41-.67zm2.01-3.02-.14-.09-4.77-2.78a.78.78 0 0 0-.79 0L9.41 ' +
    '9.23V6.9a.07.07 0 0 1 .03-.06l4.83-2.79a4.5 4.5 0 0 1 6.68 4.66zM8.31 12.86l-2.02-1.16a' +
    '.08.08 0 0 1-.04-.06V6.07a4.5 4.5 0 0 1 7.38-3.45l-.14.08L8.7 5.46a.8.8 0 0 0-.39.68zm' +
    '1.1-2.37 2.6-1.5 2.6 1.5v3l-2.6 1.5-2.6-1.5z"/></svg>',
  deepseek:
    '<svg viewBox="0 0 24 24" width="28" height="28" fill="#4d6bfe" aria-hidden="true">' +
    '<path d="M23.75 4.48c-.25-.12-.36.11-.51.23l-.14.14c-.37.4-.8.66-1.37.63-.83-.05-1.54.21-' +
    '2.16.85-.13-.78-.58-1.25-1.25-1.55-.35-.16-.71-.31-.96-.65-.17-.24-.22-.51-.3-.77-.06-.' +
    '16-.11-.32-.29-.35-.2-.03-.28.14-.36.28-.31.57-.43 1.2-.42 1.84.03 1.44.63 2.58 1.84 ' +
    '3.39.14.09.17.19.13.32-.08.28-.18.55-.27.83-.05.18-.14.22-.33.14a5.5 5.5 0 0 1-1.74-1.18' +
    'c-.86-.83-1.63-1.74-2.6-2.46-.22-.17-.45-.33-.69-.47-.98-.96.13-1.74.39-1.84.27-.1.09-.' +
    '43-.78-.43-.87 0-1.67.3-2.69.68a3 3 0 0 1-.46.14 9.6 9.6 0 0 0-2.88-.1C5.9 6.96 4.4 7.' +
    '85 3.29 9.37.08 8.6-.23 10.68.15 12.85c.4 2.28 1.57 4.18 3.36 5.65 1.86 1.53 4 2.28 6.' +
    '44 2.14 1.48-.09 3.13-.28 4.99-1.86.47.23.96.33 1.78.4.63.06 1.24-.03 1.7-.13.74-.16.' +
    '68-.84.42-.96-2.16-1-1.68-.6-2.11-.93 1.1-1.3 2.75-2.64 3.39-7 .05-.35.01-.57 0-.85-.' +
    '01-.17.04-.24.23-.26a4.2 4.2 0 0 0 1.54-.48c1.4-.76 1.96-2.01 2.09-3.52.02-.23 0-.47-.' +
    '25-.59zM11.58 18c-2.09-1.64-3.1-2.18-3.52-2.16-.39.02-.32.47-.24.76.09.29.2.49.37.74.11.' +
    '17.19.42-.11.6-.67.42-1.84-.14-1.9-.17-1.36-.8-2.5-1.86-3.3-3.3-.77-1.4-1.22-2.89-1.3-' +
    '4.48-.02-.39.09-.52.48-.59a4.7 4.7 0 0 1 1.53-.04c2.13.31 3.95 1.27 5.47 2.77.87.86 1.' +
    '53 1.89 2.2 2.89.72 1.07 1.5 2.08 2.48 2.91.35.3.63.51.89.68-.8.09-2.14.11-3.05-.61z"/>' +
    '</svg>',
};

// ---------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------

const app = {
  transport: null,
  blocked: null, // 'firmware_old' | 'page_old' | 'unsupported_browser'
  blockedProtocol: 0,
  connected: false,
  connecting: false,
  waiting: false, // USB port open, waiting for the Passport's USB setting to be opened
  saving: null, // the row a change is being saved for: { type, id }
  closing: false, // the hotspot is about to close
  ended: false, // the session ended; a new one needs the Passport
  state: null,
  stateAt: 0,
  busy: false,
  polling: false,
  silentSince: 0, // since when the Passport has answered no state read
  lastPollAt: 0,
  validateJob: '',
  expanded: null, // { type: 'account' | 'network', id }
  removing: null, // { type, id }
  removeTimer: 0,
  wifiForm: null, // { index } while the Wi‑Fi form is open (index -1 adds)
  sheet: null, // 'codex' | 'deepseek' while the add-account sheet is open
  notice: null, // { text, kind, action }
  summary: '',
  drafts: new Map(), // what the user typed into the row editors
  inputs: new Map(), // the editor inputs of the current rows, by draft name
  focus: '',
  signature: '',
};

// ---------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------

function h(tag, props = {}, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props)) {
    if (value === undefined || value === null || value === false) continue;
    if (key === 'class') node.className = value;
    else if (key === 'text') node.textContent = value;
    else if (key === 'html') node.innerHTML = value;
    else if (key.startsWith('on')) node.addEventListener(key.slice(2), value);
    else if (['type', 'value', 'disabled', 'hidden', 'checked', 'id'].includes(key))
      node[key] = value;
    else node.setAttribute(key, value === true ? '' : String(value));
  }
  node.append(...children.flat().filter((child) => child !== null && child !== undefined));
  return node;
}

function newRequestId() {
  const data = new Uint8Array(4);
  if (globalThis.crypto?.getRandomValues) crypto.getRandomValues(data);
  else for (let i = 0; i < 4; i += 1) data[i] = Math.floor(Math.random() * 256);
  return Array.from(data, (x) => x.toString(16).padStart(2, '0')).join('');
}

// Crockford Base32: upper case, I and L read as 1, O as 0, no U.
function normalizeAccessCode(value) {
  const digits = String(value)
    .toUpperCase()
    .replace(/[IL]/g, '1')
    .replace(/O/g, '0')
    .replace(/[^0-9A-TV-Z]/g, '')
    .slice(0, 16);
  return digits.match(/.{1,4}/g)?.join('-') ?? '';
}

// The rules below are the Passport's own; the page says so before the Passport has to.
function textProblem(value, { min = 0, max, what }) {
  const length = textBytes(value);
  // eslint-disable-next-line no-control-regex
  if (/[\u0000-\u001f\u007f]/.test(value)) return `${what}不能包含控制字符`;
  if (length < min) return `${what}不能为空`;
  if (length > max) return `${what}最多 ${max} 个字节（一个汉字约 3 个字节）`;
  return '';
}
const labelProblem = (label, required) =>
  textProblem(label, { min: required ? 1 : 0, max: 32, what: '备注名' });
const ssidProblem = (ssid) => textProblem(ssid, { min: 1, max: 32, what: 'Wi‑Fi 名称' });
const keyProblem = (key) => textProblem(key, { min: 1, max: 511, what: '密钥' });
// 8 to 63 bytes, or exactly 64 hexadecimal digits (a raw WPA2 key); empty means an open network.
function wifiPasswordProblem(password) {
  if (password === '') return '';
  const length = textBytes(password);
  const control = textProblem(password, { max: 64, what: '密码' });
  if (control && !control.includes('最多')) return control;
  if (length < 8 || length > 64)
    return 'Wi‑Fi 密码须为 8 至 63 个字节（一个汉字约 3 个字节），或恰好 64 位十六进制密钥';
  if (length === 64 && !/^[0-9a-fA-F]{64}$/.test(password))
    return '64 个字节的密码必须是 64 位十六进制密钥；普通密码请用 8 至 63 个字节';
  return '';
}

function failureText(error) {
  const code = String(error?.code ?? '');
  const device = code.startsWith('device_') ? code.slice(7) : code;
  return (
    MESSAGES[device] ??
    serialErrorMessage(error) ??
    (error?.jobError ? '操作没有完成，请重试' : 'Passport 没有完成这次操作，请重试')
  );
}

// What a Passport that does not speak protocol 3 looks like: no `protocol`, or an older one.
function protocolProblem(device) {
  const protocol = device?.protocol;
  if (typeof protocol !== 'number' || protocol < PROTOCOL) return 'firmware_old';
  return protocol > PROTOCOL ? 'page_old' : null;
}

const PAGES_URL = 'https://zesming.github.io/ai-passport-quota/';

function matchingPageUrl(protocol) {
  if (location.protocol === 'file:') return `${PAGES_URL}p${protocol}/`;
  const base = location.pathname.replace(/(?:p\d+\/)?(?:index\.html)?$/, '');
  return `${location.origin}${base.endsWith('/') ? base : `${base}/`}p${protocol}/`;
}

// ---------------------------------------------------------------------------------------
// Derived values
// ---------------------------------------------------------------------------------------

const networksOf = (state) => state?.network?.saved_networks ?? [];
const accountsOf = (state) => state?.accounts ?? [];
const pendingOf = (item) => (item.validation ?? 'ok') === 'pending';
const failedOf = (item) => (item.validation ?? 'ok') === 'failed';

function jobOf(id) {
  return (app.state?.jobs ?? []).find((job) => job.request_id === id);
}
const jobDone = (job) => job?.status === 'succeeded' || job?.status === 'failed';

function validating() {
  if (app.state?.validating) return true;
  return Boolean(app.validateJob) && !jobDone(jobOf(app.validateJob));
}

function remainingSeconds() {
  const left = app.state?.session?.remaining_seconds;
  if (!Number.isFinite(left)) return null;
  return Math.max(0, Math.ceil(left - (Date.now() - app.stateAt) / 1000));
}

function countsOf(state) {
  const items = [...networksOf(state), ...accountsOf(state)];
  return { pending: items.filter(pendingOf).length, failed: items.filter(failedOf).length };
}

function interactive() {
  return app.connected && !app.blocked && !app.busy && !validating() && !app.closing;
}

// ---------------------------------------------------------------------------------------
// Notices
// ---------------------------------------------------------------------------------------

function say(text, kind = '', action = null) {
  app.notice = text ? { text, kind, action } : null;
  renderNotice();
}

function renderNotice() {
  const notice = app.notice;
  byId('notice').hidden = !notice;
  byId('notice').className = `notice ${notice?.kind ?? ''}`;
  byId('notice-text').textContent = notice?.text ?? '';
  const action = byId('notice-action');
  action.hidden = !notice?.action;
  action.textContent = notice?.action?.label ?? '';
  action.onclick = notice?.action?.run ?? null;
}

// ---------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------

function chip(validation) {
  const [text, kind] = {
    saved: ['已保存', 'saved'],
    ok: ['正常', 'ok'],
    pending: ['待验证', 'pending'],
  }[validation] ?? ['验证失败', 'failed'];
  return h('span', { class: `chip ${kind}`, text });
}

// While a change is being saved the row says so instead of showing its state.
function rowChip(type, id, validation) {
  const saving = app.saving?.type === type && app.saving.id === id;
  return saving ? h('span', { class: 'chip', text: '保存中…' }) : chip(validation);
}

function field(name, label, props = {}) {
  const input = h('input', {
    ...props,
    value: app.drafts.get(name) ?? props.value ?? '',
    oninput: (event) => app.drafts.set(name, event.target.value),
    onfocus: () => {
      app.focus = name;
    },
    onblur: () => {
      if (app.focus === name) app.focus = '';
    },
  });
  app.inputs.set(name, input);
  return h('label', { class: 'field' }, h('span', { text: label }), input);
}

function errorLine(item, kind) {
  if (!failedOf(item) || !item.error_code) return null;
  const known = MESSAGES[item.error_code] ?? '验证没有通过';
  let hint = '';
  if (kind === 'network')
    hint = app.transport.kind === 'http' ? `，${HOTSPOT_WIFI_RECOVERY}` : `，${WIFI_RECOVERY}`;
  return h('p', { class: 'row-error', text: known + hint });
}

function accountTitle(account) {
  return account.label || (account.provider === 'deepseek' ? 'DeepSeek' : 'ChatGPT');
}

function accountSub(account) {
  if (account.provider === 'deepseek') return 'DeepSeek';
  return account.email || 'ChatGPT';
}

function accountValue(account) {
  if (account.provider === 'deepseek') {
    const cny = account.balance?.balance_infos?.find((info) => info.currency === 'CNY');
    return cny ? `余额 ¥${cny.total_balance}` : '';
  }
  return [
    ['five_hour', '5 小时额度'],
    ['seven_day', '7 天额度'],
  ]
    .filter(([key]) => account[key]?.present && typeof account[key].remaining_percent === 'number')
    .map(([key, name]) => `${name} 剩余 ${account[key].remaining_percent}%`)
    .join(' · ');
}

function toggle(type, id) {
  const same = app.expanded?.type === type && app.expanded.id === id;
  app.expanded = same ? null : { type, id };
  app.removing = null;
  clearDrafts();
  render(true);
}

function removeButton(type, id, run) {
  const confirming = app.removing?.type === type && app.removing.id === id;
  return h('button', {
    class: `danger${confirming ? ' confirm' : ''}`,
    type: 'button',
    text: confirming ? '确认移除' : '移除',
    disabled: !interactive(),
    onclick: () => {
      if (!confirming) {
        app.removing = { type, id };
        clearTimeout(app.removeTimer);
        app.removeTimer = setTimeout(() => {
          app.removing = null;
          render(true);
        }, REMOVE_CONFIRM_MS);
        render(true);
        return;
      }
      clearTimeout(app.removeTimer);
      app.removing = null;
      void run();
    },
  });
}

function accountDetail(account) {
  const locked = !interactive();
  const pieces = [errorLine(account, 'account')];
  if (account.provider === 'deepseek') {
    pieces.push(
      field(`label:${account.id}`, '备注名', { value: account.label ?? '', maxlength: 32 }),
      field(`key:${account.id}`, '新密钥（不改密钥就留空）', {
        type: 'password',
        autocomplete: 'new-password',
        autocapitalize: 'none',
        spellcheck: 'false',
      }),
      h(
        'div',
        { class: 'actions' },
        h('button', {
          class: 'primary',
          type: 'button',
          text: '保存',
          disabled: locked,
          onclick: () => void saveDeepseek(account),
        }),
        removeButton('account', account.id, () => removeAccount(account)),
      ),
    );
  } else {
    const queued = account.validation === 'pending';
    pieces.push(
      queued ? h('p', { class: 'muted', text: '点“完成设置”后，在 Passport 上完成授权' }) : null,
      h(
        'div',
        { class: 'actions' },
        queued
          ? null
          : h('button', {
              type: 'button',
              text: '重新授权',
              disabled:
                locked ||
                accountsOf(app.state).some((item) => item.provider === 'codex' && pendingOf(item)),
              onclick: () => void queueCodex(account),
            }),
        removeButton('account', account.id, () => removeAccount(account)),
      ),
    );
  }
  return h('div', { class: 'detail' }, ...pieces);
}

function accountRow(account) {
  const open = app.expanded?.type === 'account' && app.expanded.id === account.id;
  const value = accountValue(account);
  return h(
    'li',
    { class: 'row' },
    h(
      'button',
      {
        class: 'row-main',
        type: 'button',
        'aria-expanded': open ? 'true' : 'false',
        onclick: () => toggle('account', account.id),
      },
      h('span', { class: 'logo', html: LOGOS[account.provider] ?? '' }),
      h(
        'span',
        { class: 'row-text' },
        h('span', { class: 'row-title', text: accountTitle(account) }),
        h('span', { class: 'row-sub', text: accountSub(account) }),
        value ? h('span', { class: 'row-sub', text: value }) : null,
      ),
      rowChip('account', account.id, account.validation),
      h('span', { class: 'chevron', text: open ? '⌄' : '›' }),
    ),
    open ? accountDetail(account) : null,
  );
}

function networkDetail(network) {
  return h(
    'div',
    { class: 'detail' },
    errorLine(network, 'network'),
    h(
      'div',
      { class: 'actions' },
      h('button', {
        type: 'button',
        text: '修改',
        disabled: !interactive(),
        onclick: () => openWifiForm(network),
      }),
      removeButton('network', String(network.index), () => removeNetwork(network)),
    ),
  );
}

function networkRow(network) {
  const open = app.expanded?.type === 'network' && app.expanded.id === String(network.index);
  return h(
    'li',
    { class: 'row' },
    h(
      'button',
      {
        class: 'row-main',
        type: 'button',
        'aria-expanded': open ? 'true' : 'false',
        onclick: () => toggle('network', String(network.index)),
      },
      h(
        'span',
        { class: 'row-text' },
        h('span', { class: 'row-title', text: network.ssid }),
        network.selected && ['ok', 'saved'].includes(network.validation)
          ? h('span', { class: 'row-sub', text: '正在使用' })
          : null,
      ),
      rowChip('network', String(network.index), network.validation),
      h('span', { class: 'chevron', text: open ? '⌄' : '›' }),
    ),
    open ? networkDetail(network) : null,
  );
}

function restoreFocus() {
  const target = app.inputs.get(app.focus);
  if (target) {
    target.focus();
    target.setSelectionRange?.(target.value.length, target.value.length);
  }
}

function renderConnection() {
  const transport = app.transport;
  const serial = transport.kind === 'serial';
  const dot = byId('conn-dot');
  const text = byId('conn-text');
  const help = byId('conn-help');
  dot.className = 'dot';
  help.hidden = true;
  byId('conn-steps').hidden = true;
  byId('code-form').hidden = true;
  byId('usb-connect').hidden = true;
  byId('usb-disconnect').hidden = true;
  if (app.blocked === 'unsupported_browser') {
    text.textContent = '请用桌面版 Chrome 或 Edge 打开';
    help.hidden = false;
    help.textContent = '其他浏览器暂时不能通过 USB 设置 Passport。';
  } else if (app.connected) {
    dot.className = 'dot on';
    text.textContent = serial ? '已连接（USB）' : '已连接（热点）';
    byId('usb-disconnect').hidden = !serial;
  } else if (app.waiting) {
    dot.className = 'dot spin';
    text.textContent = '请在 Passport 上打开 USB 设置';
    help.hidden = false;
    help.textContent = 'USB 线已连上。Passport 的 USB 设置一打开，页面会自动连接。';
    byId('usb-disconnect').hidden = false;
    byId('usb-disconnect').textContent = '取消';
  } else if (app.connecting) {
    dot.className = 'dot spin';
    text.textContent = '连接中…';
  } else if (app.closing) {
    text.textContent = '热点即将关闭';
  } else if (serial) {
    text.textContent = app.ended ? '设置时间已到' : '未连接';
    byId('conn-steps').hidden = false;
    byId('usb-connect').hidden = false;
    byId('usb-connect').textContent = transport.hasPort() ? '重新连接' : '连接 Passport';
  } else {
    text.textContent = app.ended ? '设置时间已到' : '未连接';
    byId('code-form').hidden = false;
  }
  if (app.connected) byId('usb-disconnect').textContent = '断开 USB';
}

// While connecting the lists show placeholders and nothing can be changed.
function renderSkeleton() {
  const placeholder = () =>
    h(
      'li',
      { class: 'row skeleton', 'aria-hidden': 'true' },
      h('span', { class: 'bar wide-bar' }),
      h('span', { class: 'bar' }),
    );
  byId('accounts-title').textContent = '账户';
  byId('wifi-title').textContent = 'Wi‑Fi';
  byId('accounts-hint').hidden = true;
  byId('accounts-empty').hidden = true;
  byId('wifi-empty').hidden = true;
  byId('wifi-form').hidden = true;
  byId('account-list').replaceChildren(placeholder(), placeholder());
  byId('wifi-list').replaceChildren(placeholder());
  for (const id of ['add-account', 'add-wifi', 'refresh-select', 'sleep-select'])
    byId(id).disabled = true;
}

function renderLists() {
  const state = app.state;
  const accounts = accountsOf(state);
  const networks = networksOf(state);
  const noWifi = networks.length === 0;
  const form = app.wifiForm !== null || noWifi;
  const accountsFull = accounts.length >= LIMITS.accounts;
  const networksFull = networks.length >= LIMITS.networks;
  byId('accounts-title').textContent = `账户 ${accounts.length}/${LIMITS.accounts}`;
  byId('wifi-title').textContent = `Wi‑Fi ${networks.length}/${LIMITS.networks}`;
  byId('accounts-section').className = `card${noWifi ? ' dim' : ''}`;
  byId('accounts-hint').hidden = !noWifi;
  byId('accounts-empty').hidden = accounts.length > 0 || noWifi;
  byId('accounts-empty').textContent = accountsFull ? '最多 8 个账户，请先移除一个' : '还没有账户';
  byId('wifi-empty').hidden = networks.length > 0;
  byId('add-account').disabled = !interactive() || noWifi || accountsFull;
  byId('add-account').title = accountsFull ? '最多 8 个账户，请先移除一个' : '';
  byId('add-wifi').disabled = !interactive() || networksFull;
  byId('add-wifi').title = networksFull ? '最多 3 个 Wi‑Fi，请先移除一个' : '';
  app.inputs.clear();
  byId('account-list').replaceChildren(...accounts.map(accountRow));
  byId('wifi-list').replaceChildren(...networks.map(networkRow));
  byId('wifi-form').hidden = !form;
  if (form) {
    const editing = app.wifiForm !== null && app.wifiForm.index >= 0;
    byId('wifi-cancel').hidden = noWifi && !editing;
    byId('wifi-save').disabled = !interactive();
    byId('wifi-ssid').disabled = !interactive();
    byId('wifi-password').disabled = !interactive();
  }
}

function renderSettings() {
  const settings = app.state?.settings ?? {};
  const refresh = byId('refresh-select');
  const screenOff = byId('sleep-select');
  refresh.value =
    settings.auto_refresh === false ? REFRESH_MANUAL : String(settings.refresh_seconds);
  screenOff.value = String(settings.screen_timeout_seconds ?? 120);
  refresh.disabled = screenOff.disabled = !interactive();
}

const STEP_TEXT = {
  wifi: '正在连接 Wi‑Fi…',
  deepseek: '正在验证 DeepSeek…',
  chatgpt: '请在 Passport 上完成 ChatGPT 授权',
};

function renderBar() {
  const serial = app.transport.kind === 'serial';
  const show = app.connected && !app.blocked && !app.closing;
  byId('bar').hidden = !show;
  if (!show) return;
  const { pending, failed } = countsOf(app.state);
  const running = validating();
  const authorizing = running && app.state?.validation_step === 'chatgpt';
  let text;
  if (running) text = STEP_TEXT[app.state?.validation_step] ?? '正在验证…';
  else if (pending || failed)
    text = [pending ? `有 ${pending} 项待验证` : '', failed ? `${failed} 项验证失败` : '']
      .filter(Boolean)
      .join('，');
  else text = '全部正常';
  if (!serial && !running) text += '。点“完成设置”后，Passport 会关闭热点并开始验证';
  byId('bar-text').textContent = text;
  const finish = byId('finish');
  finish.textContent = running ? '验证中…' : '完成设置';
  finish.disabled = !interactive() || (serial && pending + failed === 0);
  const cancel = byId('cancel-auth');
  cancel.hidden = !authorizing || !app.state?.operation?.cancelable;
  cancel.disabled = app.busy;
}

function renderSheet() {
  const kind = app.sheet;
  byId('sheet').hidden = !kind;
  byId('codex-form').hidden = kind !== 'codex';
  byId('deepseek-form').hidden = kind !== 'deepseek';
  byId('pick-codex').className = kind === 'codex' ? 'on' : '';
  byId('pick-deepseek').className = kind === 'deepseek' ? 'on' : '';
  const locked = !interactive();
  byId('codex-add').disabled = locked;
  byId('deepseek-add').disabled = locked;
}

function renderRemaining() {
  const left = remainingSeconds();
  const node = byId('remaining');
  node.hidden = left === null || !app.connected || app.closing;
  if (left !== null)
    node.textContent = `剩余 ${Math.floor(left / 60)}:${String(left % 60).padStart(2, '0')}`;
}

function render(force = false) {
  renderConnection();
  renderRemaining();
  renderNotice();
  const content = app.connected && !app.blocked && Boolean(app.state);
  const loading = !content && !app.blocked && (app.connecting || app.waiting);
  byId('content').hidden = !content && !loading;
  byId('content').className = loading ? 'loading' : '';
  if (!content) {
    byId('bar').hidden = true;
    if (loading) {
      app.signature = '';
      renderSkeleton();
    }
    renderSheet();
    return;
  }
  const signature = JSON.stringify([
    app.state.accounts,
    app.state.network,
    app.state.settings,
    app.state.validating,
    app.state.validation_step,
    app.expanded,
    app.removing,
    app.wifiForm,
    app.busy,
    app.saving,
    app.closing,
    validating(),
  ]);
  if (force || signature !== app.signature) {
    app.signature = signature;
    renderLists();
    renderSettings();
    restoreFocus();
  }
  renderBar();
  renderSheet();
}

// ---------------------------------------------------------------------------------------
// Device traffic
// ---------------------------------------------------------------------------------------

function clearSecrets() {
  clearDrafts();
  for (const id of ['wifi-password', 'deepseek-key']) byId(id).value = '';
}

function block(reason, device = null) {
  app.blocked = reason;
  app.blockedProtocol = device?.protocol ?? 0;
  app.waiting = false;
  app.connected = false;
  app.connecting = false;
  app.state = null;
  clearSecrets();
  void app.transport.close();
  if (reason === 'firmware_old')
    say('Passport 固件过旧，请先升级固件', 'error', {
      label: '查看升级方法',
      run: () => window.open(README_URL, '_blank', 'noopener,noreferrer'),
    });
  else if (reason === 'page_old') {
    const url = matchingPageUrl(app.blockedProtocol);
    say(
      '设置页版本过旧',
      'error',
      url ? { label: '打开匹配的页面', run: () => window.open(url, '_self') } : null,
    );
  }
  render(true);
}

function endSession(text, { retry = false, sameOpener = false } = {}) {
  app.silentSince = 0;
  app.waiting = false;
  app.connected = false;
  app.connecting = false;
  app.ended = true;
  app.state = null;
  app.validateJob = '';
  clearSecrets();
  app.transport.sessionLost({ sameOpener });
  say(text, 'warn');
  render(true);
  if (retry) void waitForWindow();
}

// A rejection that ends the session, or the page's use of this Passport.
function handleFailure(error) {
  const code = String(error?.code ?? '');
  if (code === 'device_unsupported_version')
    return block(protocolProblem(error.device), error.device);
  if (code === 'device_session_expired') {
    // The USB setting runs out: the port stays open and the page waits for it to be opened again.
    if (app.transport.kind === 'serial')
      return endSession('设置时间已到。请在 Passport 上重新打开 USB 设置', { retry: true });
    return endSession('设置时间已到。在 Passport 上重新开启热点设置，再输入新的访问码');
  }
  if (code === 'device_invalid_session')
    return endSession('另一个页面接管了 Passport 的 USB 设置。要在这里继续，请点“重新连接”');
  if (code === 'device_access_locked' || code === 'device_unauthorized') {
    app.transport.sessionLost();
    app.waiting = false;
    app.connected = app.connecting = false;
    app.state = null;
    say(failureText(error), 'error');
    return render(true);
  }
  if (['serial_closed', 'serial_read_error', 'serial_write_timeout'].includes(code)) {
    app.waiting = false;
    app.connected = false;
    app.state = null;
    void app.transport.close();
    say(failureText(error), 'error');
    return render(true);
  }
  say(failureText(error), 'error');
  return undefined;
}

function acceptState(state) {
  const problem = protocolProblem(state);
  if (problem) {
    block(problem, state);
    return false;
  }
  const wasValidating = validating();
  app.state = state;
  app.stateAt = Date.now();
  if (app.validateJob && jobDone(jobOf(app.validateJob))) {
    const job = jobOf(app.validateJob);
    app.validateJob = '';
    if (wasValidating) announceResult(job);
  } else if (wasValidating && !validating()) announceResult(null);
  if (state.storage_error) say(MESSAGES[state.storage_error] ?? 'Passport 暂时不能保存', 'error');
  return true;
}

function announceResult(job) {
  const { pending, failed } = countsOf(app.state);
  const login = app.state?.login;
  if (job?.status === 'failed' && job.error_code && !failed)
    say(MESSAGES[job.error_code] ?? '验证没有完成', 'error');
  else if (failed) say(`${failed} 项验证失败。修改后再点“完成设置”`, 'error');
  else if (login?.error_code && login.state === 'failed')
    say(`ChatGPT：${MESSAGES[login.error_code] ?? '授权没有完成'}。可以再添加一次`, 'error');
  else if (pending) say(`还有 ${pending} 项待验证`, 'warn');
  else say('全部正常，可以断开 USB 了', '');
}

async function poll() {
  if (app.polling || !app.transport.hasSession() || app.closing) return false;
  app.polling = true;
  app.lastPollAt = Date.now();
  try {
    const state = await app.transport.stateGet();
    if (!app.transport.hasSession()) return false;
    app.silentSince = 0;
    const accepted = acceptState(state);
    if (accepted) render();
    return accepted;
  } catch (error) {
    const silent = ['serial_timeout', 'serial_write_timeout'].includes(error?.code);
    if (app.connected && app.transport.kind === 'serial' && silent) {
      // No answer: a busy Passport reads late. Only long silence means it restarted, and then the
      // same opener asks again: a Passport that did not restart gives the same session back.
      app.silentSince ||= Date.now();
      if (Date.now() - app.silentSince >= SILENT_RESTART_MS)
        endSession('Passport 可能重启了，请在 Passport 上打开 USB 设置', {
          retry: true,
          sameOpener: true,
        });
    } else if (app.connected) handleFailure(error);
    return false;
  } finally {
    app.polling = false;
  }
}

// Wait for the Passport to finish what a command asked for: errors such as account_limit are job
// results, not part of the acknowledgement.
async function waitForJob(id) {
  const until = Date.now() + JOB_WAIT_MS;
  while (Date.now() < until && app.connected) {
    await poll();
    const job = jobOf(id);
    if (job?.status === 'succeeded') return;
    if (job?.status === 'failed') {
      const failure = new Error(job.error_code);
      failure.code = `device_${job.error_code}`;
      failure.jobError = true;
      throw failure;
    }
    await sleep(250);
  }
  // No answer yet: the change may or may not have taken effect, so do not report success.
  if (app.connected) {
    const failure = new Error('job_timeout');
    failure.code = 'device_job_timeout';
    throw failure;
  }
}

async function send(op, fields = {}, { wait = true, target = null } = {}) {
  if (app.busy || !app.connected || app.closing) return null;
  const body = {
    v: PROTOCOL,
    request_id: newRequestId(),
    op,
    ...fields,
    phone_utc: Math.floor(Date.now() / 1000),
  };
  app.busy = true;
  app.saving = target;
  render(true);
  try {
    try {
      await app.transport.command(body);
    } catch (error) {
      // The write was sent but not acknowledged: the Passport's job list says whether it arrived.
      if (error?.code !== 'usb_uncertain_receipt') throw error;
      await poll();
      if (!app.transport.jobProvesAdmission(app.state, body.request_id)) throw error;
    }
    if (wait) await waitForJob(body.request_id);
    return body;
  } catch (error) {
    handleFailure(error);
    return null;
  } finally {
    app.busy = false;
    app.saving = null;
    render(true);
  }
}

// ---------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------

function clearDrafts(prefix = '') {
  for (const name of [...app.drafts.keys()]) if (name.startsWith(prefix)) app.drafts.delete(name);
}

async function saveWifi(event) {
  event.preventDefault();
  const ssid = byId('wifi-ssid').value;
  const password = byId('wifi-password').value;
  const problem = ssidProblem(ssid) || wifiPasswordProblem(password);
  const error = byId('wifi-error');
  error.hidden = !problem;
  error.textContent = problem;
  if (problem) return;
  const index = app.wifiForm?.index ?? -1;
  const sent = await send(
    'network_save',
    {
      ssid,
      password,
      open_network: password === '',
      ...(index >= 0 ? { network_index: index } : {}),
    },
    index >= 0 ? { target: { type: 'network', id: String(index) } } : {},
  );
  if (!sent) return;
  byId('wifi-password').value = '';
  byId('wifi-ssid').value = '';
  app.wifiForm = null;
  app.expanded = null;
  say('Wi‑Fi 已保存，待验证', '');
  render(true);
}

function openWifiForm(network = null) {
  app.wifiForm = { index: network ? network.index : -1 };
  byId('wifi-ssid').value = network?.ssid ?? '';
  byId('wifi-password').value = '';
  byId('wifi-error').hidden = true;
  render(true);
  byId('wifi-ssid').focus?.();
}

async function removeNetwork(network) {
  const sent = await send(
    'network_remove',
    { network_index: network.index },
    { target: { type: 'network', id: String(network.index) } },
  );
  if (sent) {
    app.expanded = null;
    say('已移除 Wi‑Fi', '');
  }
}

async function removeAccount(account) {
  const sent = await send(
    'account_remove',
    { account_id: account.id },
    { target: { type: 'account', id: account.id } },
  );
  if (sent) {
    app.expanded = null;
    clearDrafts(`label:${account.id}`);
    clearDrafts(`key:${account.id}`);
    say('已移除账户', '');
  }
}

async function saveDeepseek(account) {
  const label = (app.drafts.get(`label:${account.id}`) ?? account.label ?? '').trim();
  const key = (app.drafts.get(`key:${account.id}`) ?? '').trim();
  const problem = labelProblem(label, true) || (key ? keyProblem(key) : '');
  if (problem) return say(problem, 'error');
  const sent = await send(
    'deepseek_save',
    {
      account_id: account.id,
      label,
      ...(key ? { api_key: key } : {}),
    },
    { target: { type: 'account', id: account.id } },
  );
  if (sent) {
    clearDrafts(`key:${account.id}`);
    clearDrafts(`label:${account.id}`);
    say(key ? '新密钥已保存，待验证' : '备注名已保存', '');
    render(true);
  }
  return undefined;
}

async function queueCodex(account) {
  const sent = await send(
    'codex_queue',
    { account_id: account.id, label: account.label ?? '' },
    { target: { type: 'account', id: account.id } },
  );
  if (sent) say('点“完成设置”后，在 Passport 上完成授权', '');
}

async function addCodex(event) {
  event.preventDefault();
  const label = byId('codex-label').value.trim();
  const problem = labelProblem(label, false);
  if (problem) return say(problem, 'error');
  const sent = await send('codex_queue', { label });
  if (sent) {
    closeSheet();
    say('已添加 ChatGPT。点“完成设置”后，在 Passport 上完成授权', '');
  }
  return undefined;
}

async function addDeepseek(event) {
  event.preventDefault();
  const label = byId('deepseek-label').value.trim();
  const key = byId('deepseek-key').value.trim();
  const problem = labelProblem(label, true) || keyProblem(key);
  const error = byId('deepseek-error');
  error.hidden = !problem;
  error.textContent = problem;
  if (problem) return undefined;
  const sent = await send('deepseek_save', { label, api_key: key });
  if (sent) {
    closeSheet();
    say('密钥已保存，待验证', '');
  }
  return undefined;
}

function openSheet(kind) {
  app.sheet = kind;
  byId('deepseek-error').hidden = true;
  renderSheet();
  (kind === 'codex' ? byId('codex-label') : byId('deepseek-label')).focus?.();
}

function closeSheet() {
  app.sheet = null;
  for (const id of ['codex-label', 'deepseek-label', 'deepseek-key']) byId(id).value = '';
  byId('deepseek-key').type = 'password';
  byId('deepseek-show').checked = false;
  renderSheet();
}

async function saveSettings() {
  const mode = byId('refresh-select').value;
  const current = app.state?.settings ?? {};
  await send('settings_save', {
    auto_refresh: mode !== REFRESH_MANUAL,
    refresh_seconds: mode === REFRESH_MANUAL ? current.refresh_seconds || 300 : Number(mode),
    screen_timeout_seconds: Number(byId('sleep-select').value),
  });
}

async function finish() {
  if (!interactive()) return;
  if (app.transport.kind === 'http') {
    const sent = await send('setup_close', {}, { wait: false });
    if (!sent) return;
    app.closing = true;
    say('热点即将关闭，请看 Passport 屏幕上的结果。手机会自动断开。', '');
    app.state = null;
    render(true);
    return;
  }
  const sent = await send('validate', {}, { wait: false });
  if (!sent) return;
  app.validateJob = sent.request_id;
  say('', '');
  render(true);
  await poll(); // show the first step at once
}

async function cancelAuthorization() {
  const id = app.state?.operation?.request_id;
  if (!id || app.busy) return;
  await send('operation_cancel', { target_request_id: id }, { wait: false });
}

// ---------------------------------------------------------------------------------------
// Connecting
// ---------------------------------------------------------------------------------------

// Step 1: choose the port and open it. This may restart the Passport, which is why its USB setting
// is opened only afterwards.
async function connectSerial() {
  if (app.connecting || app.connected || app.waiting) return;
  app.ended = false;
  say('', '');
  if (!app.transport.hasPort()) {
    app.connecting = true;
    render(true);
    try {
      await app.transport.openPort();
    } catch (error) {
      app.connecting = false;
      if (error?.name === 'NotFoundError' || error?.name === 'AbortError')
        say('没有选择设备', 'warn');
      else if (error?.name === 'InvalidStateError') say(MESSAGES.serial_busy, 'error');
      else handleFailure(error);
      render(true);
      return;
    }
    app.connecting = false;
  }
  await waitForWindow();
}

// Step 2: ask for a session every 1.5 seconds until the Passport's USB setting is open.
async function waitForWindow() {
  if (app.waiting) return;
  app.waiting = true;
  render(true);
  while (app.waiting) {
    try {
      await app.transport.openSession({ timeoutMs: SESSION_OPEN_TRY_MS });
    } catch (error) {
      if (!app.waiting) return;
      if (
        ['serial_timeout', 'serial_write_timeout', 'device_session_expired'].includes(error?.code)
      ) {
        await sleep(SESSION_RETRY_MS);
        continue;
      }
      if (error?.code === 'device_session_busy') {
        // Another page opened a session a moment ago: the Passport lets go of an idle one soon.
        say('另一个设置页面刚刚连接过 Passport，几秒后自动重试', 'warn');
        await sleep(SESSION_BUSY_RETRY_MS);
        continue;
      }
      app.waiting = false;
      handleFailure(error);
      render(true);
      return;
    }
    if (!app.waiting) return; // cancelled while the answer was on its way
    app.waiting = false;
    app.connected = true;
    try {
      if (acceptState(await app.transport.stateGet())) {
        say('', '');
        render(true);
      }
    } catch (error) {
      app.connected = false;
      handleFailure(error);
      render(true);
    }
    return;
  }
}

async function connectHttp(event) {
  event?.preventDefault();
  const code = normalizeAccessCode(byId('code-input').value);
  if (code.length !== 19) return say('访问码是 16 位，格式 XXXX-XXXX-XXXX-XXXX', 'error');
  app.transport.setCode(code);
  app.connecting = true;
  app.ended = false;
  say('', '');
  render(true);
  try {
    const first = await app.transport.openSession();
    byId('code-input').value = '';
    app.connected = true;
    app.connecting = false;
    if (!acceptState(first)) return undefined;
    render(true);
  } catch (error) {
    app.connecting = false;
    app.connected = false;
    handleFailure(error);
    render(true);
  }
  return undefined;
}

async function disconnect() {
  app.waiting = false;
  app.connected = false;
  app.state = null;
  app.validateJob = '';
  clearSecrets();
  await app.transport.close();
  say('USB 已断开', '');
  render(true);
}

function release() {
  app.waiting = false;
  app.connected = app.connecting = false;
  app.state = null;
  clearSecrets();
  byId('code-input').value = '';
  void app.transport.close();
  render(true);
}

// ---------------------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------------------

function bind() {
  byId('code-form').addEventListener('submit', connectHttp);
  byId('code-input').addEventListener('input', (event) => {
    event.target.value = normalizeAccessCode(event.target.value);
  });
  byId('usb-connect').addEventListener('click', connectSerial);
  byId('usb-disconnect').addEventListener('click', disconnect);
  byId('add-account').addEventListener('click', () => openSheet('codex'));
  byId('add-wifi').addEventListener('click', () => openWifiForm());
  byId('wifi-form').addEventListener('submit', saveWifi);
  byId('wifi-cancel').addEventListener('click', () => {
    app.wifiForm = null;
    byId('wifi-password').value = '';
    render(true);
  });
  byId('wifi-show').addEventListener('change', (event) => {
    byId('wifi-password').type = event.target.checked ? 'text' : 'password';
  });
  byId('refresh-select').addEventListener('change', saveSettings);
  byId('sleep-select').addEventListener('change', saveSettings);
  byId('finish').addEventListener('click', finish);
  byId('cancel-auth').addEventListener('click', cancelAuthorization);
  byId('pick-codex').addEventListener('click', () => openSheet('codex'));
  byId('pick-deepseek').addEventListener('click', () => openSheet('deepseek'));
  byId('codex-form').addEventListener('submit', addCodex);
  byId('deepseek-form').addEventListener('submit', addDeepseek);
  byId('deepseek-show').addEventListener('change', (event) => {
    byId('deepseek-key').type = event.target.checked ? 'text' : 'password';
  });
  byId('sheet-cancel').addEventListener('click', closeSheet);
  document.addEventListener('keydown', (event) => {
    if (event.key === 'Escape' && app.sheet) closeSheet();
  });
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden) void poll();
  });
  window.addEventListener('pagehide', release);
  // An open USB session also polls while the page is hidden: the Passport treats a silent page as
  // gone after a few seconds. A hidden hotspot page stays quiet.
  setInterval(() => {
    const wait = validating() ? POLL_VALIDATING_MS : POLL_MS;
    const active = !document.hidden || (app.transport.kind === 'serial' && app.connected);
    if (active && app.connected && Date.now() - app.lastPollAt >= wait - 100) void poll();
    renderRemaining();
  }, 1000);
}

function start() {
  const hotspot = location.protocol === 'http:';
  app.transport = hotspot
    ? createHttpTransport((path, init) => fetch(path, init))
    : createSerialTransport(navigator);
  bind();
  if (!app.transport.supported) {
    app.blocked = 'unsupported_browser';
    render(true);
    return;
  }
  render(true);
  if (hotspot) {
    // The QR code carries the access code in the address fragment; read it once and remove it.
    const fragment = new URLSearchParams(location.hash.slice(1)).get('code');
    history.replaceState(null, '', location.pathname + location.search);
    const code = normalizeAccessCode(fragment ?? '');
    if (code.length === 19) {
      byId('code-input').value = code;
      void connectHttp();
    }
  }
}

start();
