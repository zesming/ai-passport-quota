// Run the production setup page (main/setup_page.html, built from main/setup/) against a software
// Passport over both transports: the hotspot over HTTP, and USB over Web Serial.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  buildSetupPage,
  flattenModules,
  lintSources,
  readSources,
} from '../tools/build_setup_page.mjs';
import { createSimDevice, createSimSerial, SCENARIOS } from '../tools/fake_device.mjs';
import { createHotspotServer, deviceCsp, usbPreviewHtml } from '../tools/preview_portable.mjs';
import {
  UsbDeviceSession,
  serialErrorMessage,
  startDeviceSerial,
} from '../main/setup/transport_serial.mjs';
import { all, byClass, byText, flush, loadPage } from './fake_dom.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const { html, csp } = buildSetupPage();
const CODE = 'K7QM-2X9D-PA4T-Z8RW';

// ---------------------------------------------------------------------------------------
// The page file: generated, one style, one module script, hash-only CSP, no inline anything.

{
  assert.equal(fs.readFileSync(path.join(root, 'main/setup_page.html'), 'utf8'), html);
  assert.equal(html.match(/<style>/g).length, 1);
  assert.equal(html.match(/<script\b/g).length, 1);
  assert.equal(html.match(/<script type="module">/g).length, 1);
  const script = html.match(/<script type="module">([\s\S]*?)<\/script>/)[1];
  const style = html.match(/<style>([\s\S]*?)<\/style>/)[1];
  const hash = (text) => `'sha256-${createHash('sha256').update(text).digest('base64')}'`;
  assert.equal(
    csp,
    [
      "default-src 'none'",
      `script-src ${hash(script)}`,
      `style-src ${hash(style)}`,
      'img-src data:',
      "connect-src 'self'",
      "base-uri 'none'",
      "form-action 'none'",
    ].join('; '),
  );
  assert(html.includes(`<meta http-equiv="Content-Security-Policy" content="${csp}">`));
  assert(script.startsWith('\nif (window.top !== window.self) {'), 'the frame guard comes first');
  assert(script.includes("document.documentElement.textContent = '请直接打开此页面'; throw 0;"));
  // Nothing the policy would block, and nothing that needs the network or the browser store.
  assert(!/\sstyle\s*=/.test(html.replace(style, '')), 'no style attributes');
  assert(!/\son[a-z]+\s*=/i.test(html.replace(script, '')), 'no inline event handlers');
  assert(!/javascript:/i.test(html));
  assert(!/<(?:link|img|iframe|object|embed)\b/i.test(html));
  assert(!/localStorage|sessionStorage|indexedDB|document\.cookie/.test(script));
  const ours = /github\.com\/zesming\/ai-passport-quota|zesming\.github\.io\/ai-passport-quota/;
  for (const [url] of script.matchAll(/https?:\/\/[^\s'"`]+/g)) assert(ours.test(url), url);
  assert(html.includes('width=device-width, initial-scale=1'));
  assert(/min-height: 44px/.test(style) && /font-size: 16px/.test(style));
  assert(/prefers-color-scheme: dark/.test(style));
  assert.equal(byteLength(html) < 120 * 1024, true, 'the page stays small enough to embed');
}
function byteLength(text) {
  return Buffer.byteLength(text);
}

// The build refuses what the policy would block, and says what and where.
{
  const clean = readSources();
  assert.deepEqual(lintSources(clean), []);
  const dirty = (name, edit) => ({ ...clean, [name]: edit(clean[name]) });
  const cases = [
    ['page.html', (s) => s.replace('<h1>', '<h1 style="color:red">'), 'style='],
    ['page.html', (s) => s.replace('<h1>', '<h1 onclick="x()">'), 'event handler'],
    ['page.html', (s) => s.replace('<h1>', '<h1 onClick = "x()">'), 'event handler'],
    [
      'page.html',
      (s) => s.replace('<footer', '<a href="javascript:void(0)"></a><footer'),
      'javascript:',
    ],
    ['app.mjs', (s) => `${s}\nnode.innerHTML = '<style>.a{}</style>';`, '<style>'],
    ['app.mjs', (s) => `${s}\nnode.innerHTML = '<p style="x">';`, 'style='],
    ['app.mjs', (s) => `${s}\nnode.innerHTML = '<p onclick="f()">';`, 'event handler'],
    ['app.mjs', (s) => `${s}\nnode.setAttribute('style', 'x');`, 'setAttribute'],
    ['transport_http.mjs', (s) => `${s}\nconst url = 'javascript:alert(1)';`, 'javascript:'],
    [
      'page.html',
      (s) => s.replace('</head>', '<link rel="stylesheet" href="x.css"></head>'),
      'loads a resource',
    ],
  ];
  for (const [name, edit, expected] of cases) {
    const problems = lintSources(dirty(name, edit));
    assert(
      problems.some((problem) => problem.startsWith(name) && problem.includes(expected)),
      `${name}: ${expected} is refused (${problems.join('; ')})`,
    );
    assert.throws(() => buildSetupPage({ files: dirty(name, edit) }), /not CSP-clean/);
  }
  // Comments may talk about all of this.
  assert.deepEqual(lintSources(dirty('app.mjs', (s) => `// a style= attribute\n${s}`)), []);
  assert.throws(
    () => flattenModules(dirty('app.mjs', (s) => `${s}\nconst textEncoder = 1;`)),
    /textEncoder is already declared/,
  );
  assert.throws(
    () => flattenModules(dirty('app.mjs', (s) => `${s}\nexport default 1;`)),
    /unsupported import\/export/,
  );
  // The command line does the same: a bad source fails the build, and writes nothing.
  const copy = fs.mkdtempSync(path.join(os.tmpdir(), 'setup-build-'));
  fs.mkdirSync(path.join(copy, 'tools'));
  fs.mkdirSync(path.join(copy, 'main'));
  fs.copyFileSync(
    path.join(root, 'tools/build_setup_page.mjs'),
    path.join(copy, 'tools/build_setup_page.mjs'),
  );
  fs.cpSync(path.join(root, 'main/setup'), path.join(copy, 'main/setup'), { recursive: true });
  const pagePath = path.join(copy, 'main/setup/page.html');
  fs.writeFileSync(
    pagePath,
    fs.readFileSync(pagePath, 'utf8').replace('<h1>', '<h1 onclick="x()">'),
  );
  const run = (...args) =>
    spawnSync(process.execPath, ['tools/build_setup_page.mjs', ...args], {
      cwd: copy,
      encoding: 'utf8',
    });
  const refused = run();
  assert.equal(refused.status, 1);
  assert(refused.stderr.includes('event handler'));
  assert(!fs.existsSync(path.join(copy, 'main/setup_page.html')));
  fs.writeFileSync(pagePath, clean['page.html']);
  assert.equal(run('--check').status, 1, 'a missing page is out of date');
  assert.equal(run().status, 0);
  assert.equal(run('--check').status, 0);
  fs.appendFileSync(path.join(copy, 'main/setup/style.css'), '\n.extra { color: red; }\n');
  const stale = run('--check');
  assert.equal(stale.status, 1);
  assert(stale.stderr.includes('out of date'));
  fs.rmSync(copy, { recursive: true, force: true });
}

// ---------------------------------------------------------------------------------------
// Helpers for the page tests.

const response = (result) => ({
  ok: result.status < 400,
  status: result.status,
  json: async () => result.json,
});

function hotspot(device, { hash = '', fetches } = {}) {
  return loadPage(html, {
    protocol: 'http:',
    origin: 'http://192.168.4.1',
    hash,
    fetchImpl: async (route, init) => {
      fetches?.push({ route, init });
      const body = init.body ? JSON.parse(init.body) : null;
      return response(device.http(init.method, route, init.headers, body));
    },
  });
}

function usb(device, options = {}) {
  const serial = createSimSerial(device);
  const page = loadPage(html, { protocol: options.protocol ?? 'file:', serial, ...options });
  return { page, serial };
}

// Let the page, and the Passport's clock, run on.
async function elapse(page, device, ms) {
  for (let spent = 0; spent < ms; spent += 250) {
    device.advance(250);
    await page.advance(250);
  }
}

async function until(page, condition, what = 'condition') {
  for (let i = 0; i < 60; i += 1) {
    if (condition()) return;
    await page.advance(250);
  }
  assert.fail(`timed out waiting for ${what}`);
}

const text = (page, id) => page.node(id).textContent;
const connected = (page) => text(page, 'conn-text').startsWith('已连接');
const rows = (page, id) => page.node(id).children;
const rowText = (row) => row.textContent;
const notice = (page) => (page.node('notice').hidden ? '' : text(page, 'notice-text'));
const click = async (page, id) => page.node(id).click();

async function connectUsb(page, device) {
  await click(page, 'usb-connect');
  await until(page, () => connected(page), 'the USB connection');
}

// The editor fields of an open row, by their caption.
function editorInput(page, caption) {
  for (const label of all(page.node('account-list')).filter((n) => n.tagName === 'LABEL')) {
    if (label.children[0]?.textContent.startsWith(caption)) return label.children[1];
  }
  return null;
}

async function openRow(page, listId, index) {
  await byClass(page.node(listId), 'row-main')[index].click();
}

// ---------------------------------------------------------------------------------------
// The page does not run in a frame, and the first screen of each transport.

{
  const framed = loadPage(html, { protocol: 'http:', top: false });
  assert.equal(framed.thrown, 0, 'the script stops itself');
  assert.equal(framed.document.documentElement.textContent, '请直接打开此页面');
  assert.equal(framed.log.fetches.length, 0);
  assert.equal(text(framed, 'conn-text'), '', 'nothing of the page was set up');
}
{
  // The transport follows the protocol the page was opened with. Not a loopback check.
  for (const [protocol, hostname] of [
    ['https:', 'zesming.github.io'],
    ['file:', ''],
    ['https:', 'localhost'],
    ['https:', '192.168.1.20'],
  ]) {
    const device = createSimDevice();
    SCENARIOS.default(device);
    device.openUsb();
    const { page, serial } = usb(device, { protocol, origin: `${protocol}//${hostname}` });
    assert.equal(page.thrown, null);
    assert.equal(text(page, 'conn-text'), '未连接');
    assert.equal(page.node('usb-connect').hidden, false, `${protocol} uses USB`);
    assert.equal(page.node('code-form').hidden, true);
    await connectUsb(page, device);
    assert.equal(serial.requested, 1);
    assert.equal(
      JSON.stringify(serial.lastOptions),
      JSON.stringify({ filters: [{ usbVendorId: 0x303a, usbProductId: 0x1001 }] }),
    );
    assert.equal(page.log.fetches.length, 0, 'USB makes no network request');
  }
  const device = createSimDevice();
  const page = hotspot(device);
  assert.equal(page.node('code-form').hidden, false, 'http: uses the hotspot');
  assert.equal(page.node('usb-connect').hidden, true);
  // A browser without Web Serial is told what to use.
  const plain = loadPage(html, { protocol: 'https:', origin: 'https://zesming.github.io' });
  assert.equal(text(plain, 'conn-text'), '请用桌面版 Chrome 或 Edge 打开');
  assert.equal(plain.node('usb-connect').hidden, true);
  assert.equal(plain.node('content').hidden, true);
}

// ---------------------------------------------------------------------------------------
// USB: a first-time Passport, from nothing to validated, without the network.

{
  const device = createSimDevice();
  device.openUsb();
  const { page } = usb(device);
  await connectUsb(page, device);
  assert.equal(text(page, 'conn-text'), '已连接（USB）');
  assert.equal(text(page, 'remaining'), '剩余 2:00');
  // No Wi-Fi yet: the account area is dimmed and the Wi-Fi form is open.
  assert.equal(page.node('accounts-hint').hidden, false);
  assert.equal(page.node('add-account').disabled, true);
  assert.equal(page.node('wifi-form').hidden, false);
  assert.equal(text(page, 'accounts-title'), '账户 0/8');
  assert.equal(text(page, 'wifi-title'), 'Wi‑Fi 0/3');

  // The page checks what the Passport checks, before sending anything.
  await page.node('wifi-ssid').type_('Home');
  await page.node('wifi-password').type_('short');
  await page.node('wifi-form').dispatch('submit');
  assert(text(page, 'wifi-error').includes('8 至 63'));
  assert.equal(device.log.length, 0);
  await page.node('wifi-password').type_('right-password');
  await page.node('wifi-form').dispatch('submit');
  assert.equal(device.networks[0].password, 'right-password');
  assert.equal(device.networks[0].validation, 'pending');
  assert.equal(page.node('wifi-password').value, '', 'the password is not kept');
  assert.equal(text(page, 'wifi-title'), 'Wi‑Fi 1/3');
  assert.equal(page.node('accounts-hint').hidden, true);
  assert(rowText(rows(page, 'wifi-list')[0]).includes('待验证'));
  assert.equal(text(page, 'bar-text'), '有 1 项待验证');
  assert.equal(device.validateRuns, 0, 'saving validates nothing');

  // Add a DeepSeek key and a ChatGPT account.
  await click(page, 'add-account');
  assert.equal(page.node('sheet').hidden, false);
  await click(page, 'pick-deepseek');
  assert.equal(page.node('deepseek-form').hidden, false);
  await page.node('deepseek-label').type_('Notes');
  await page.node('deepseek-form').dispatch('submit');
  assert(text(page, 'deepseek-error').includes('密钥不能为空'));
  await page.node('deepseek-key').type_('sk-good-key');
  await page.node('deepseek-form').dispatch('submit');
  assert.equal(page.node('sheet').hidden, true);
  assert.equal(device.accounts[0].key, 'sk-good-key');
  assert.equal(page.node('deepseek-key').value, '', 'the key is not kept');
  await click(page, 'add-account');
  await click(page, 'pick-codex');
  await page.node('codex-label').type_('Work');
  await page.node('codex-form').dispatch('submit');
  assert.equal(device.queue.label, 'Work');
  assert.equal(text(page, 'accounts-title'), '账户 2/8');
  assert.equal(rows(page, 'account-list').length, 2);
  assert(rowText(rows(page, 'account-list')[1]).includes('Work'));
  assert.equal(text(page, 'bar-text'), '有 3 项待验证');
  assert.equal(device.validateRuns, 0);

  // 完成设置 sends validate; the page then shows each step and locks the controls.
  await click(page, 'finish');
  assert.equal(device.validateRuns, 1);
  assert.equal(device.log.at(-1).op, 'validate');
  await elapse(page, device, 250);
  assert.equal(text(page, 'bar-text'), '正在连接 Wi‑Fi…');
  assert.equal(text(page, 'finish'), '验证中…');
  assert.equal(page.node('finish').disabled, true);
  assert.equal(page.node('add-account').disabled, true);
  assert.equal(page.node('refresh-select').disabled, true);
  await elapse(page, device, 1000);
  assert.equal(text(page, 'bar-text'), '正在验证 DeepSeek…');
  await elapse(page, device, 1000);
  assert.equal(text(page, 'bar-text'), '请在 Passport 上完成 ChatGPT 授权');
  assert.equal(page.node('cancel-auth').hidden, false);
  assert.equal(device.networks[0].validation, 'ok');
  assert.equal(device.accounts[0].validation, 'ok');
  assert(text(page, 'remaining').startsWith('剩余 4:'), 'the validate command topped the time up');
  // No change gets through while it runs.
  await elapse(page, device, 500);
  device.approveLogin();
  await elapse(page, device, 2000);
  assert(!page.node('finish').disabled || text(page, 'bar-text') === '全部正常');
  assert.equal(text(page, 'bar-text'), '全部正常');
  assert(notice(page).includes('全部正常'));
  assert.equal(rows(page, 'account-list').length, 2);
  assert(rowText(rows(page, 'account-list')[0]).includes('¥10.00'));
  assert(rowText(rows(page, 'account-list')[1]).includes('剩余 73%'));
  assert.equal(page.node('finish').disabled, true, 'nothing left to validate');
  assert.equal(page.log.fetches.length, 0, 'no network request at all');
  // Keys and passwords went only to the Passport, over the serial port.
  assert.deepEqual(
    device.log.map((body) => body.op),
    ['network_save', 'deepseek_save', 'codex_queue', 'validate'],
  );
}

// ---------------------------------------------------------------------------------------
// USB: a failed Wi-Fi row is edited and validated again; only what is not ok is retried.

{
  const device = createSimDevice();
  SCENARIOS.default(device);
  device.openUsb();
  const { page } = usb(device);
  await connectUsb(page, device);
  assert.equal(text(page, 'bar-text'), '全部正常');
  assert.equal(page.node('finish').disabled, true);
  await click(page, 'add-wifi');
  await page.node('wifi-ssid').type_('Office');
  await page.node('wifi-password').type_('bad-password');
  await page.node('wifi-form').dispatch('submit');
  await click(page, 'finish');
  await elapse(page, device, 2000);
  assert.equal(text(page, 'bar-text'), '1 项验证失败');
  assert(notice(page).includes('1 项验证失败'));
  const failed = rows(page, 'wifi-list')[1];
  assert(rowText(failed).includes('验证失败'));
  await openRow(page, 'wifi-list', 1);
  assert(
    text(page, 'wifi-list').includes('Wi‑Fi 密码错误或信号弱'),
    'the error comes with its cause',
  );
  assert(text(page, 'wifi-list').includes('点“修改”重新输入'));
  // 修改: the form opens on that network; the corrected password is validated again.
  await byText(page.node('wifi-list'), '修改', 'button').click();
  assert.equal(page.node('wifi-form').hidden, false);
  assert.equal(page.node('wifi-ssid').value, 'Office');
  await page.node('wifi-password').type_('right-password');
  await page.node('wifi-form').dispatch('submit');
  assert.equal(device.networks[1].validation, 'pending');
  assert.equal(device.log.at(-1).network_index, 1);
  const before = device.validateRuns;
  await click(page, 'finish');
  await elapse(page, device, 2000);
  assert.equal(device.validateRuns, before + 1);
  assert.equal(text(page, 'bar-text'), '全部正常');
  assert(rowText(rows(page, 'wifi-list')[1]).includes('正常'));
  assert(rowText(rows(page, 'wifi-list')[1]).includes('正在使用'));
}

// ---------------------------------------------------------------------------------------
// USB: DeepSeek key rows, ChatGPT rows, removal with a confirmation, device settings.

{
  const device = createSimDevice();
  SCENARIOS.failed(device);
  device.openUsb();
  const { page } = usb(device);
  await connectUsb(page, device);
  assert.equal(text(page, 'bar-text'), '2 项验证失败');
  // ChatGPT row: re-authorize or remove.
  await openRow(page, 'account-list', 0);
  const labels = all(page.node('account-list'))
    .filter((n) => n.tagName === 'BUTTON')
    .map((n) => n.textContent);
  assert(labels.includes('重新授权') && labels.includes('移除'));
  await byText(page.node('account-list'), '重新授权', 'button').click();
  assert.equal(device.queue.id, device.accounts[0].id);
  assert(rowText(rows(page, 'account-list')[0]).includes('待验证'));
  assert.equal(text(page, 'bar-text'), '有 1 项待验证，2 项验证失败');
  // DeepSeek row: the failed key is replaced.
  await openRow(page, 'account-list', 2);
  assert(text(page, 'account-list').includes('密钥无效'));
  assert.equal(editorInput(page, '备注名').value, 'Research');
  await editorInput(page, '新密钥').type_('sk-new-key');
  await byText(page.node('account-list'), '保存', 'button').click();
  assert.equal(device.accounts[2].key, 'sk-new-key');
  assert.equal(device.accounts[2].validation, 'pending');
  assert.equal(editorInput(page, '新密钥').value, '', 'the key is not kept');
  // A half-typed edit survives the page's polling.
  await editorInput(page, '备注名').type_('Research 2');
  await elapse(page, device, 4000);
  assert.equal(editorInput(page, '备注名').value, 'Research 2');
  // Removal needs a second press within three seconds.
  const remove = () => byText(page.node('account-list'), '移除', 'button');
  await remove().click();
  assert.equal(device.accounts.length, 3);
  assert(byText(page.node('account-list'), '确认移除', 'button'));
  assert(byClass(page.node('account-list'), 'confirm').length === 1);
  await page.advance(3500);
  assert.equal(byText(page.node('account-list'), '确认移除', 'button'), undefined, 'it reverts');
  await remove().click();
  await byText(page.node('account-list'), '确认移除', 'button').click();
  assert.equal(device.accounts.length, 2);
  assert.equal(device.log.at(-1).op, 'account_remove');
  // Device settings are sent as they change.
  page.node('refresh-select').value = '900';
  await page.node('refresh-select').dispatch('change');
  assert.deepEqual(device.settings, {
    auto_refresh: true,
    refresh_seconds: 900,
    screen_timeout_seconds: 120,
  });
  page.node('refresh-select').value = 'manual';
  await page.node('refresh-select').dispatch('change');
  assert.equal(device.settings.auto_refresh, false);
  assert.equal(device.settings.refresh_seconds, 900);
  page.node('sleep-select').value = '0';
  await page.node('sleep-select').dispatch('change');
  assert.equal(device.settings.screen_timeout_seconds, 0);
  assert.equal(page.node('refresh-select').value, 'manual');
}

// ---------------------------------------------------------------------------------------
// Limits, and the Passport's own refusals worded for the user.

{
  const device = createSimDevice();
  SCENARIOS.full(device);
  device.openUsb();
  const { page } = usb(device);
  await connectUsb(page, device);
  assert.equal(text(page, 'accounts-title'), '账户 8/8');
  assert.equal(text(page, 'wifi-title'), 'Wi‑Fi 3/3');
  assert.equal(page.node('add-account').disabled, true);
  assert.equal(page.node('add-wifi').disabled, true);
  assert.equal(page.node('add-account').title, '最多 8 个账户，请先移除一个');
  assert.equal(page.node('add-wifi').title, '最多 3 个 Wi‑Fi，请先移除一个');
}
{
  const device = createSimDevice();
  SCENARIOS.default(device);
  device.openUsb();
  const { page } = usb(device);
  await connectUsb(page, device);
  const cases = [
    ['account_limit', '最多 8 个账户，请先移除一个'],
    ['network_limit', '最多 3 个 Wi‑Fi，请先移除一个'],
    ['login_pending', '已有一个 ChatGPT 账户在等待授权，请先点“完成设置”'],
    ['storage_failed', 'Passport 没能保存，请稍后重试'],
  ];
  for (const [code, message] of cases) {
    device.failNext = code;
    page.node('refresh-select').value = '1800';
    await page.node('refresh-select').dispatch('change');
    assert.equal(notice(page), message, code);
  }
  // A Passport that is validating answers busy.
  await click(page, 'add-wifi');
  await page.node('wifi-ssid').type_('Office');
  await page.node('wifi-password').type_('right-password');
  await page.node('wifi-form').dispatch('submit');
  await click(page, 'finish');
  device.step = 'wifi';
  assert.equal(page.node('add-account').disabled, true, 'controls are locked while validating');
  device.advance(250);
  device.session.lastSeen = 0;
  const lateFrame = device.frame({
    v: 3,
    op: 'command',
    request_id: 'abcdef01',
    session_id: device.session.id,
    body: { v: 3, op: 'refresh', request_id: 'abcdef01' },
  });
  assert.equal(lateFrame.error_code, 'busy');
  assert.equal(lateFrame.protocol, 3);
  assert(lateFrame.firmware);
}

// ---------------------------------------------------------------------------------------
// The session ends: the Passport's time, a takeover by another page, a lost port.

{
  // The two minutes pass without a change: the port stays open and the page waits for the
  // Passport's USB setting to be opened again, then connects by itself.
  const device = createSimDevice();
  SCENARIOS.default(device);
  device.openUsb();
  const { page, serial } = usb(device);
  await connectUsb(page, device);
  await elapse(page, device, 125000);
  assert.equal(connected(page), false);
  assert.equal(text(page, 'conn-text'), '请在 Passport 上打开 USB 设置');
  assert.equal(notice(page), '设置时间已到。请在 Passport 上重新打开 USB 设置');
  assert.equal(page.node('content').className, 'loading', 'the lists show placeholders');
  assert.equal(serial.requested, 1);
  device.openUsb();
  await elapse(page, device, 3000);
  assert.equal(connected(page), true);
  assert.equal(serial.requested, 1, 'no second port prompt');
  assert.equal(rows(page, 'account-list').length, 2);
}
{
  // Opening the port restarts the Passport. The USB setting is opened after the port, and the page
  // asks for the session again and again until the Passport answers.
  const device = createSimDevice();
  SCENARIOS.default(device);
  device.openUsb(); // the wrong order: it is closed again by the restart
  const serial = createSimSerial(device, { resetsOnOpen: true });
  const page = loadPage(html, { protocol: 'https:', origin: 'https://zesming.github.io', serial });
  const frames = [];
  const answer = device.frame;
  device.frame = (frame) => {
    frames.push(frame);
    return answer(frame);
  };
  await click(page, 'usb-connect');
  assert.equal(serial.port.opened, true);
  assert.equal(device.session, null, 'the restart closed the USB setting');
  await elapse(page, device, 6000);
  assert.equal(connected(page), false);
  assert.equal(text(page, 'conn-text'), '请在 Passport 上打开 USB 设置');
  assert.equal(page.node('usb-disconnect').hidden, false);
  assert.equal(text(page, 'usb-disconnect'), '取消');
  assert(frames.length >= 3 && frames.every((frame) => frame.op === 'session_open'));
  assert(frames.length <= 5, 'one try every 1.5 seconds, not a burst');
  assert.equal(serial.requested, 1);
  const tries = frames.length;
  device.openUsb(); // now the user opens it on the Passport
  await elapse(page, device, 3000);
  assert.equal(connected(page), true);
  assert(frames.length > tries && frames.at(-2).op === 'session_open');
  assert.equal(page.node('content').className, '');
  assert.equal(rows(page, 'account-list').length, 2);
  assert.equal(page.log.fetches.length, 0);
}
{
  // Cancelling while waiting gives the port back.
  const device = createSimDevice();
  const { page, serial } = usb(device);
  await click(page, 'usb-connect');
  await elapse(page, device, 3000);
  assert.equal(text(page, 'conn-text'), '请在 Passport 上打开 USB 设置');
  await click(page, 'usb-disconnect');
  assert.equal(serial.port.opened, false);
  assert.equal(text(page, 'conn-text'), '未连接');
  assert.equal(page.node('usb-connect').hidden, false);
}
{
  // A change that the Passport never finishes is not reported as done, and its row says it is
  // being saved meanwhile.
  const device = createSimDevice();
  SCENARIOS.default(device);
  device.openUsb();
  const { page } = usb(device);
  await connectUsb(page, device);
  device.stallJobs = true;
  await openRow(page, 'account-list', 1);
  const saving = byText(page.node('account-list'), '保存', 'button').click();
  await page.advance(1000);
  assert(rowText(rows(page, 'account-list')[1]).includes('保存中…'));
  assert.equal(page.node('add-account').disabled, true);
  for (let i = 0; i < 100; i += 1) {
    device.advance(250);
    await page.advance(250);
  }
  await saving;
  assert.equal(notice(page), 'Passport 还没有确认这次操作。请看一下列表里的状态，再决定要不要重试');
  assert(!rowText(rows(page, 'account-list')[1]).includes('保存中…'));
}
{
  // Another page takes the link over: this one learns why.
  const device = createSimDevice();
  SCENARIOS.default(device);
  device.openUsb();
  const { page } = usb(device);
  await connectUsb(page, device);
  device.advance(7000); // the opener has been silent
  const taken = device.frame({ v: 3, op: 'session_open', request_id: 'feedface' });
  assert.equal(taken.ok, true, 'a silent opener may be replaced');
  await elapse(page, device, 2500);
  assert(notice(page).includes('另一个页面接管'));
  assert.equal(page.node('content').hidden, true);
}

{
  // After a restart a saved network is "已保存": not waiting, not failed, nothing to validate.
  const device = createSimDevice();
  SCENARIOS.default(device);
  device.openUsb();
  const { page } = usb(device);
  await connectUsb(page, device);
  const wifi = rowText(rows(page, 'wifi-list')[0]);
  assert(wifi.includes('已保存') && wifi.includes('正在使用'));
  assert.equal(text(page, 'bar-text'), '全部正常');
  assert.equal(page.node('finish').disabled, true);
  // Editing the password of the network in use: the stored one keeps working until it validates.
  await openRow(page, 'wifi-list', 0);
  await byText(page.node('wifi-list'), '修改', 'button').click();
  await page.node('wifi-password').type_('bad-new-password');
  await page.node('wifi-form').dispatch('submit');
  assert.equal(device.networks[0].password, 'password');
  assert(rowText(rows(page, 'wifi-list')[0]).includes('待验证'));
  await click(page, 'finish');
  await elapse(page, device, 2500);
  assert(rowText(rows(page, 'wifi-list')[0]).includes('验证失败'));
  assert.equal(device.networks[0].password, 'password', 'a failing password is never stored');
  await openRow(page, 'wifi-list', 0);
  await byText(page.node('wifi-list'), '修改', 'button').click();
  await page.node('wifi-password').type_('good-new-password');
  await page.node('wifi-form').dispatch('submit');
  await click(page, 'finish');
  await elapse(page, device, 2500);
  assert(rowText(rows(page, 'wifi-list')[0]).includes('正常'));
  assert.equal(device.networks[0].password, 'good-new-password');
}
{
  // A ChatGPT authorization that timed out or was cancelled keeps its account, failed, to retry.
  const device = createSimDevice({ loginMs: 3000 });
  SCENARIOS.default(device);
  device.openUsb();
  const { page } = usb(device);
  await connectUsb(page, device);
  await click(page, 'add-account');
  await click(page, 'pick-codex');
  await page.node('codex-label').type_('Work');
  await page.node('codex-form').dispatch('submit');
  await click(page, 'finish');
  await elapse(page, device, 6000);
  const row = () => rowText(rows(page, 'account-list')[2]);
  assert(row().includes('Work') && row().includes('验证失败'));
  assert.equal(text(page, 'bar-text'), '1 项验证失败');
  await openRow(page, 'account-list', 2);
  assert(text(page, 'account-list').includes('授权超时'));
  await byText(page.node('account-list'), '重新授权', 'button').click();
  assert(row().includes('待验证'));
  await click(page, 'finish');
  await elapse(page, device, 1500);
  assert.equal(text(page, 'bar-text'), '请在 Passport 上完成 ChatGPT 授权');
  await click(page, 'cancel-auth');
  await elapse(page, device, 1500);
  assert(row().includes('验证失败') && text(page, 'account-list').includes('已取消'));
  assert.equal(device.accounts.length, 2);
  assert(device.queue, 'the account is still queued');
}
{
  // Opened from a file, an out-of-date page points at the published page of the right version.
  const newer = createSimDevice({ protocol: 4, firmware: '4.0.0' });
  newer.openUsb();
  const { page } = usb(newer);
  await click(page, 'usb-connect');
  await until(page, () => notice(page) !== '', 'the notice');
  assert.equal(notice(page), '设置页版本过旧');
  await click(page, 'notice-action');
  assert.equal(page.log.opened[0][0], 'https://zesming.github.io/ai-passport-quota/p4/');
}

{
  // The Passport does not read the port while its USB setting is closed. Once its 64-byte buffer is
  // full the host's write blocks; that must not close the port or pile up frames.
  const device = createSimDevice();
  SCENARIOS.default(device);
  const serial = createSimSerial(device, { backpressure: true });
  const page = loadPage(html, { protocol: 'file:', serial });
  const frames = [];
  const answer = device.frame;
  device.frame = (frame) => {
    frames.push(frame.op);
    return answer(frame);
  };
  await click(page, 'usb-connect');
  await elapse(page, device, 12000);
  assert.equal(text(page, 'conn-text'), '请在 Passport 上打开 USB 设置');
  assert.equal(serial.port.opened, true, 'a blocked write does not close the port');
  assert.equal(page.thrown, null);
  device.openUsb();
  await elapse(page, device, 4000);
  assert.equal(connected(page), true);
  assert.equal(rows(page, 'account-list').length, 2);
  assert(frames.length <= 6, `no pile of frames behind a blocked write (${frames.length})`);
  assert(frames.every((op) => ['session_open', 'state_get'].includes(op)));
}
{
  // The Passport restarts while the page is connected: after long silence the page waits for the
  // USB setting to be opened again.
  const device = createSimDevice();
  SCENARIOS.default(device);
  device.openUsb();
  const { page } = usb(device);
  await connectUsb(page, device);
  device.reset();
  await elapse(page, device, 12000);
  assert.equal(connected(page), true, 'a few seconds of silence are not a restart');
  await elapse(page, device, 18000);
  assert.equal(connected(page), false);
  assert.equal(text(page, 'conn-text'), '请在 Passport 上打开 USB 设置');
  assert.equal(notice(page), 'Passport 可能重启了，请在 Passport 上打开 USB 设置');
  device.openUsb();
  await elapse(page, device, 4000);
  assert.equal(connected(page), true);
  assert.equal(notice(page), '');
}
for (const mode of [true, 'buffered']) {
  // The network task is busy for 15 seconds (a slow HTTPS request): frames are delayed, not
  // dropped. That is neither a restart nor a broken link, with or without host buffering.
  const device = createSimDevice();
  SCENARIOS.default(device);
  device.openUsb();
  const serial = createSimSerial(device, { backpressure: mode });
  const page = loadPage(html, { protocol: 'file:', serial });
  const ops = [];
  const answer = device.frame;
  device.frame = (frame) => {
    ops.push(frame.op);
    return answer(frame);
  };
  await click(page, 'usb-connect');
  await elapse(page, device, 2000);
  assert.equal(connected(page), true);
  device.stall(15000);
  await elapse(page, device, 16000);
  assert.equal(connected(page), true, `busy firmware, buffering ${mode}`);
  assert.equal(serial.port.opened, true);
  assert.equal(page.thrown, null);
  await elapse(page, device, 4000);
  assert.equal(connected(page), true);
  assert.equal(notice(page), '');
  assert(!ops.slice(1).includes('session_open'), 'the session was never asked for again');
  assert.equal(rows(page, 'account-list').length, 2);
}
{
  // Silence longer than that is taken for a restart. The page asks again with the same opener, so
  // a Passport that was only busy gives the same session back instead of answering session_busy.
  const device = createSimDevice();
  SCENARIOS.default(device);
  device.openUsb();
  const serial = createSimSerial(device, { backpressure: 'buffered' });
  const page = loadPage(html, { protocol: 'file:', serial });
  const opens = [];
  const answer = device.frame;
  device.frame = (frame) => {
    if (frame.op === 'session_open') opens.push(frame.request_id);
    return answer(frame);
  };
  await click(page, 'usb-connect');
  await elapse(page, device, 2000);
  const session = device.session.id;
  device.stall(45000);
  await elapse(page, device, 36000);
  assert.equal(text(page, 'conn-text'), '请在 Passport 上打开 USB 设置');
  await elapse(page, device, 14000);
  assert.equal(connected(page), true, 'the same session came back');
  assert.equal(device.session.id, session);
  assert.equal(new Set(opens).size, 1, 'one opener throughout');
  assert(!notice(page).includes('另一个'));
}
{
  // Waiting, and another page opened a session a moment ago: the Passport says session_busy, so
  // the page waits about six seconds and asks again instead of stopping.
  const device = createSimDevice();
  SCENARIOS.default(device);
  device.openUsb();
  assert.equal(device.frame({ v: 3, op: 'session_open', request_id: 'feedface' }).ok, true);
  const { page } = usb(device);
  await click(page, 'usb-connect');
  await elapse(page, device, 3000);
  assert.equal(connected(page), false);
  assert.equal(text(page, 'conn-text'), '请在 Passport 上打开 USB 设置');
  assert(notice(page).includes('另一个设置页面'));
  await elapse(page, device, 12000);
  assert.equal(connected(page), true, 'the idle opener was replaced');
  assert.equal(notice(page), '');
}

// ---------------------------------------------------------------------------------------
// Protocol handshake: an old firmware, a newer firmware, and nothing editable in either case.

for (const transport of ['usb', 'http']) {
  const old = createSimDevice({ protocol: 2, firmware: '1.4.0' });
  SCENARIOS.default(old);
  if (transport === 'usb') old.openUsb();
  else old.openHotspot();
  const page = transport === 'usb' ? usb(old).page : hotspot(old, { hash: `#code=${CODE}` });
  if (transport === 'usb') await click(page, 'usb-connect');
  await until(page, () => notice(page) !== '', 'the version notice');
  assert.equal(notice(page), 'Passport 固件过旧，请先升级固件', transport);
  assert.equal(page.node('notice-action').hidden, false);
  assert.equal(text(page, 'notice-action'), '查看升级方法');
  await click(page, 'notice-action');
  assert(page.log.opened[0][0].includes('github.com/zesming/ai-passport-quota'));
  for (const id of ['content', 'bar']) assert.equal(page.node(id).hidden, true, `${id} hidden`);
  assert.equal(old.log.length, 0, 'nothing was changed on an old Passport');

  const newer = createSimDevice({ protocol: 4, firmware: '4.0.0' });
  SCENARIOS.default(newer);
  if (transport === 'usb') newer.openUsb();
  else newer.openHotspot();
  const other =
    transport === 'usb'
      ? usb(newer, {
          protocol: 'https:',
          origin: 'https://zesming.github.io',
          pathname: '/ai-passport-quota/p3/',
        }).page
      : hotspot(newer, { hash: `#code=${CODE}` });
  if (transport === 'usb') await click(other, 'usb-connect');
  await until(other, () => notice(other) !== '', 'the newer-version notice');
  assert.equal(notice(other), '设置页版本过旧', transport);
  for (const id of ['content', 'bar']) assert.equal(other.node(id).hidden, true, `${id} hidden`);
  if (transport === 'usb') {
    assert.equal(text(other, 'notice-action'), '打开匹配的页面');
    await click(other, 'notice-action');
    assert.equal(other.log.opened[0][0], 'https://zesming.github.io/ai-passport-quota/p4/');
  }
  assert.equal(newer.log.length, 0);
}

// ---------------------------------------------------------------------------------------
// Hotspot: the access code, the lockout, and a close that hands over to the Passport.

{
  const device = createSimDevice();
  SCENARIOS.default(device);
  device.openHotspot();
  const fetches = [];
  const page = hotspot(device, { hash: `#code=${CODE.toLowerCase()}`, fetches });
  await until(page, () => connected(page), 'the hotspot connection');
  assert.deepEqual(page.log.replaced, ['/'], 'the code is removed from the address');
  assert.equal(text(page, 'conn-text'), '已连接（热点）');
  assert.equal(fetches[0].init.headers['X-AIQ-Access'], CODE);
  assert.equal(page.node('add-account').disabled, false);
  assert.equal(rows(page, 'account-list').length, 2);
  // Changes go to the hotspot as commands; validate is a USB command and is never sent.
  page.node('sleep-select').value = '300';
  await page.node('sleep-select').dispatch('change');
  assert.equal(device.settings.screen_timeout_seconds, 300);
  await click(page, 'add-wifi');
  await page.node('wifi-ssid').type_('Office');
  await page.node('wifi-password').type_('right-password');
  await page.node('wifi-form').dispatch('submit');
  assert.equal(device.networks[1].validation, 'pending');
  assert(text(page, 'bar-text').includes('Passport 会关闭热点并开始验证'));
  assert.equal(page.node('finish').disabled, false);
  await click(page, 'finish');
  assert.equal(device.log.at(-1).op, 'setup_close');
  assert(!device.log.some((body) => body.op === 'validate'));
  assert.equal(notice(page), '热点即将关闭，请看 Passport 屏幕上的结果。手机会自动断开。');
  assert.equal(page.node('content').hidden, true);
  assert.equal(page.node('bar').hidden, true);
  assert.equal(device.validateRuns, 1, 'the Passport validates after closing the hotspot');
  const polls = fetches.length;
  await elapse(page, device, 5000);
  assert.equal(fetches.length, polls, 'the page stops talking to the closed hotspot');
}
{
  // Typing the code by hand: spaces, lower case, and look-alike letters are forgiven.
  const device = createSimDevice();
  SCENARIOS.default(device);
  device.openHotspot();
  const page = hotspot(device);
  assert.equal(page.node('code-form').hidden, false);
  assert.equal(text(page, 'conn-text'), '未连接');
  await page.node('code-input').type_('k7qm 2x9d-pa4t z8rw');
  assert.equal(page.node('code-input').value, CODE);
  await page.node('code-input').type_('K7QM-2X9D-PA4T-Z8R');
  await page.node('code-form').dispatch('submit');
  assert(notice(page).includes('16 位'));
  await page.node('code-input').type_('o7QM-2X9D-PA4T-Z8RW'); // an O reads as a zero
  assert.equal(page.node('code-input').value, '07QM-2X9D-PA4T-Z8RW');
  // Wrong codes: refused with a hint, and five of them lock the Passport.
  for (let attempt = 1; attempt <= 4; attempt += 1) {
    await page.node('code-input').type_('0000-0000-0000-000' + attempt);
    await page.node('code-form').dispatch('submit');
    assert.equal(notice(page), '访问码不对，请对照 Passport 屏幕');
    assert.equal(page.node('code-form').hidden, false);
  }
  await page.node('code-input').type_('0000-0000-0000-0005');
  await page.node('code-form').dispatch('submit');
  assert.equal(notice(page), '访问码错误次数过多，请在 Passport 上重新开启热点设置');
  await page.node('code-input').type_(CODE); // even the right one is refused now
  await page.node('code-form').dispatch('submit');
  assert.equal(notice(page), '访问码错误次数过多，请在 Passport 上重新开启热点设置');
  assert.equal(page.node('content').hidden, true);
  // A new hotspot session starts afresh.
  device.openHotspot();
  await page.node('code-input').type_(CODE);
  await page.node('code-form').dispatch('submit');
  await until(page, () => connected(page), 'the connection after a new session');
}
{
  // The lockout counts wrong codes in a row: a right one starts the count again.
  const device = createSimDevice();
  device.openHotspot();
  const wrong = { 'X-AIQ-Access': '0000-0000-0000-0000' };
  const right = { 'X-AIQ-Access': CODE };
  for (let round = 0; round < 3; round += 1) {
    for (let attempt = 1; attempt <= 4; attempt += 1)
      assert.equal(device.http('GET', '/api/state', wrong).json.error_code, 'unauthorized');
    assert.equal(device.http('GET', '/api/state', right).status, 200);
  }
}
{
  // The hotspot's time runs out.
  const device = createSimDevice();
  SCENARIOS.default(device);
  device.openHotspot();
  const page = hotspot(device, { hash: `#code=${CODE}` });
  await until(page, () => connected(page), 'the connection');
  device.advance(601000);
  await elapse(page, device, 2000);
  assert(notice(page).includes('设置时间已到'));
  assert(notice(page).includes('重新开启热点设置'));
  assert.equal(page.node('code-form').hidden, false);
}
{
  // Staying on the page: the remaining time counts down, and a change tops it up to five minutes.
  const device = createSimDevice();
  SCENARIOS.default(device);
  device.openHotspot();
  const page = hotspot(device, { hash: `#code=${CODE}` });
  await until(page, () => connected(page), 'the connection');
  const seconds = () => {
    const [, minutes, rest] = text(page, 'remaining').match(/剩余 (\d+):(\d+)/);
    return Number(minutes) * 60 + Number(rest);
  };
  assert.equal(text(page, 'remaining'), '剩余 10:00');
  await elapse(page, device, 4000);
  assert(Math.abs(seconds() - 596) <= 1, text(page, 'remaining'));
  for (let i = 0; i < 540; i += 20) {
    device.advance(20000);
    await page.advance(20000, 1000);
  }
  assert(seconds() < 70, 'about a minute is left');
  page.node('sleep-select').value = '600';
  await page.node('sleep-select').dispatch('change');
  assert(seconds() >= 299, 'the change topped the time up');
}

// ---------------------------------------------------------------------------------------
// Keeping the link alive, and letting go of secrets.

{
  const device = createSimDevice();
  SCENARIOS.default(device);
  device.openUsb();
  const { page } = usb(device);
  await connectUsb(page, device);
  const reads = [];
  const answer = device.frame;
  device.frame = (frame) => {
    if (frame.op === 'state_get') reads.push(frame.request_id);
    return answer(frame);
  };
  page.document.hidden = true;
  await page.advance(2100);
  assert(reads.length >= 1, 'a hidden page with an open USB session still reads state');
}
{
  const device = createSimDevice();
  SCENARIOS.default(device);
  device.openHotspot();
  const fetches = [];
  const page = hotspot(device, { hash: `#code=${CODE}`, fetches });
  await until(page, () => connected(page), 'the connection');
  page.document.hidden = true;
  const before = fetches.length;
  await page.advance(10000);
  assert.equal(fetches.length, before, 'a hidden hotspot page stays quiet');
  page.document.hidden = false;
  await page.documentEvent('visibilitychange');
  assert(fetches.length > before, 'and reads again when shown');
}
{
  // Leaving the page drops the secrets the user typed and the open port.
  const device = createSimDevice();
  SCENARIOS.default(device);
  device.openUsb();
  const { page, serial } = usb(device);
  await connectUsb(page, device);
  await click(page, 'add-wifi');
  await page.node('wifi-ssid').type_('Office');
  await page.node('wifi-password').type_('typed-password');
  await click(page, 'add-account');
  await click(page, 'pick-deepseek');
  await page.node('deepseek-key').type_('typed-key');
  await page.windowEvent('pagehide');
  assert.equal(page.node('wifi-password').value, '');
  assert.equal(page.node('deepseek-key').value, '');
  assert.equal(page.node('content').hidden, true);
  assert.equal(serial.port.opened, false, 'the port is released');
}

// ---------------------------------------------------------------------------------------
// The preview tool: both transports and old firmware, with the device's own CSP header.

{
  const server = createHotspotServer({ scenario: 'default', firmware: 3 });
  await new Promise((resolve) => server.server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.server.address().port}`;
  const page = await fetch(`${base}/`);
  const body = await page.text();
  assert.equal(body, html);
  const header = page.headers.get('content-security-policy');
  assert.equal(header, deviceCsp(html));
  assert(header.endsWith("; frame-ancestors 'none'"));
  assert(header.includes(csp), 'the header repeats the page policy');
  assert.equal(page.headers.get('x-frame-options'), 'DENY');
  const denied = await fetch(`${base}/api/state`, { headers: { 'X-AIQ-Access': 'nope' } });
  assert.equal(denied.status, 403);
  assert.equal((await denied.json()).error_code, 'unauthorized');
  const state = await (
    await fetch(`${base}/api/state`, { headers: { 'X-AIQ-Access': server.device().config.code } })
  ).json();
  assert.equal(state.protocol, 3);
  assert.equal(state.accounts.length, 2);
  await fetch(`${base}/__preview/empty`, { redirect: 'manual' });
  const empty = await (
    await fetch(`${base}/api/state`, { headers: { 'X-AIQ-Access': server.device().config.code } })
  ).json();
  assert.equal(empty.accounts.length, 0);
  server.server.close();
  const old = createHotspotServer({ firmware: 2 });
  await new Promise((resolve) => old.server.listen(0, '127.0.0.1', resolve));
  const oldState = await (
    await fetch(`http://127.0.0.1:${old.server.address().port}/api/state`, {
      headers: { 'X-AIQ-Access': old.device().config.code },
    })
  ).json();
  assert.equal(oldState.protocol, undefined, 'a v2 firmware reports no protocol');
  old.server.close();
  const usbHtml = usbPreviewHtml({ scenario: 'pending', firmware: 3 });
  assert(usbHtml.includes('navigator'), 'the USB preview carries a simulated serial port');
  assert.equal(usbHtml.match(/<script\b/g).length, 2);
  assert.equal(usbHtml.match(/sha256-/g).length, 3, 'one hash per inline block, all in the CSP');
}

// ---------------------------------------------------------------------------------------
// The serial layer: request ids, retries, and what counts as transmitted.

{
  assert(
    serialErrorMessage(Object.assign(new Error('busy'), { code: 'device_session_busy' })).includes(
      '数秒',
    ),
  );
  assert(
    serialErrorMessage(Object.assign(new Error('x'), { code: 'device_invalid_session' })).includes(
      '另一个页面',
    ),
  );
  const reply = (id) => ({ v: 3, op: 'result', request_id: id, ok: true });
  const makePort = ({ failWrite = false, answerWith = null } = {}) => {
    let controller;
    const writes = [];
    const readable = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    const writable = new WritableStream({
      write(chunk) {
        if (failWrite) throw Error('write failed');
        writes.push(chunk);
        const id = JSON.parse(new TextDecoder().decode(chunk).slice(5)).request_id;
        controller.enqueue(
          new TextEncoder().encode(`@AIQ:${JSON.stringify(reply(answerWith ?? id))}\n`),
        );
      },
    });
    return { port: { readable, writable }, writes };
  };
  const frame = new TextEncoder().encode(
    '@AIQ:' + JSON.stringify({ v: 3, op: 'state_get', request_id: '0a0b0c0d' }) + '\n',
  );
  let fired = 0;
  const ok = makePort();
  const serial = startDeviceSerial(ok.port, '0a0b0c0d');
  const result = await serial.send(frame, {
    timeoutMs: 1000,
    onWritten() {
      fired++;
      assert.equal(ok.writes.length, 1, 'frame is already written');
    },
  });
  assert.equal(result.ok, true);
  assert.equal(fired, 1);
  await serial.close();
  // An answer for another request id is not an answer.
  const other = makePort({ answerWith: 'ffffffff' });
  const mismatched = startDeviceSerial(other.port, '0a0b0c0d');
  await assert.rejects(mismatched.send(frame, { timeoutMs: 20 }), {
    code: 'serial_timeout',
  });
  await mismatched.close();
  const bad = makePort({ failWrite: true });
  const failing = startDeviceSerial(bad.port, '0a0b0c0d');
  fired = 0;
  await assert.rejects(
    failing.send(frame, {
      timeoutMs: 1000,
      onWritten() {
        fired++;
      },
    }),
    { code: 'serial_write_error' },
  );
  assert.equal(fired, 0, 'a failed write is not reported as transmitted');
  await failing.close();
  const session = new UsbDeviceSession(
    {
      async send(_bytes, _id, options) {
        options.onWritten?.();
        return { ok: false, error_code: 'busy', protocol: 3, firmware: '3.0.0' };
      },
      async close() {},
    },
    { requestId: () => '11223344' },
  );
  session.sessionId = 'f'.repeat(32);
  session.limits = { max_command_bytes: 2048, max_frame_bytes: 4096, max_state_bytes: 16384 };
  session.sessionExpiresAt = Date.now() + 60000;
  const refresh = (id, onTransmit) => ({
    op: 'command',
    request_id: id,
    body: { v: 3, op: 'refresh', request_id: id },
    onTransmit,
  });
  let transmitted = 0;
  await assert.rejects(
    session.mutation(
      refresh('55667788', () => {
        transmitted++;
      }),
    ),
    (error) =>
      error.code === 'device_busy' &&
      error.device.protocol === 3 &&
      error.device.firmware === '3.0.0',
  );
  assert.equal(transmitted, 1);
  const timeoutThenBusy = new UsbDeviceSession(
    {
      calls: 0,
      async send(_bytes, _id, options) {
        options.onWritten?.();
        if (this.calls++ === 0)
          throw Object.assign(new Error('timeout'), { code: 'serial_timeout' });
        return { ok: false, error_code: 'busy' };
      },
      async close() {},
    },
    { requestId: () => '11223344' },
  );
  timeoutThenBusy.sessionId = 'f'.repeat(32);
  timeoutThenBusy.limits = session.limits;
  timeoutThenBusy.sessionExpiresAt = Date.now() + 60000;
  await assert.rejects(
    timeoutThenBusy.mutation(refresh('55667788')),
    (error) => error.code === 'device_busy' && error.diagnostics.retry_used === true,
  );
  session.sessionExpiresAt = Date.now() - 1;
  transmitted = 0;
  await assert.rejects(
    session.mutation(
      refresh('55667788', () => {
        transmitted++;
      }),
    ),
    { code: 'device_session_expired' },
  );
  assert.equal(transmitted, 0, 'a locally detected expiry never reaches the device');
  // A v2 firmware answers a v3 opening with unsupported_version and no protocol.
  const v2 = createSimDevice({ protocol: 2 });
  v2.openUsb();
  const refusal = v2.frame({ v: 3, op: 'session_open', request_id: 'abcdef01' });
  assert.equal(refusal.error_code, 'unsupported_version');
  assert.equal(refusal.protocol, undefined);
}

console.log(
  'Setup page: build and CSP, both transports, version handshake, access code lockout, ' +
    'validation flow, no network request from USB PASS',
);
