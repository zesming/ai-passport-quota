// Run the setup page in a real Chrome: no console error, no Content-Security-Policy violation, the
// frame guard, and a full USB session against a simulated serial port.
//
//   CHROME="<path to Chrome>" node tests/test_setup_page_browser.mjs
//
// Without CHROME this test is skipped: it needs a browser, and tests/test_portable_phone.mjs covers
// the same page logic without one.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createHotspotServer, usbPreviewHtml } from '../tools/preview_portable.mjs';

const chrome = process.env.CHROME;
if (!chrome) {
  console.log('Setup page in a browser: skipped (set CHROME to a Chrome or Chromium binary)');
  process.exit(0);
}
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function waitFor(check, { timeoutMs = 10000, stepMs = 100, what = 'condition' } = {}) {
  const deadline = Date.now() + timeoutMs;
  let lastError = null;
  while (Date.now() < deadline) {
    try {
      const value = await check();
      if (value) return value;
    } catch (error) {
      lastError = error;
    }
    await pause(stepMs);
  }
  throw new Error(`Timed out waiting for ${what}${lastError ? `: ${lastError.message}` : ''}`);
}

async function withBrowser(run) {
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'setup-page-browser-'));
  const port = 9300 + Math.floor(Math.random() * 500);
  const args = [
    '--headless=new',
    `--remote-debugging-port=${port}`,
    `--user-data-dir=${profile}`,
    '--no-first-run',
    '--disable-gpu',
    ...(process.env.CI ? ['--no-sandbox'] : []),
    'about:blank',
  ];
  const child = spawn(chrome, args, { stdio: 'ignore' });
  try {
    await waitFor(
      async () => {
        const response = await fetch(`http://127.0.0.1:${port}/json/version`);
        return response.ok;
      },
      { timeoutMs: 12000, stepMs: 200, what: 'Chrome DevTools port' },
    );
    return await run(port, profile);
  } finally {
    child.kill();
    await pause(500);
    fs.rmSync(profile, { recursive: true, force: true, maxRetries: 5 });
  }
}

async function openTab(port) {
  const response = await fetch(`http://127.0.0.1:${port}/json/new?about:blank`, { method: 'PUT' });
  assert.equal(response.ok, true, 'open new browser tab');
  const tab = await response.json();
  const socket = new WebSocket(tab.webSocketDebuggerUrl);
  await new Promise((resolve) => socket.addEventListener('open', resolve));
  let counter = 0;
  const waiting = new Map();
  const problems = [];
  socket.addEventListener('message', (message) => {
    const data = JSON.parse(message.data);
    if (data.id && waiting.has(data.id)) waiting.get(data.id)(data);
    else if (data.method === 'Runtime.exceptionThrown') problems.push(data.params);
    else if (data.method === 'Log.entryAdded' && data.params.entry.level === 'error')
      problems.push(data.params.entry);
    else if (data.method === 'Runtime.consoleAPICalled' && data.params.type === 'error')
      problems.push(data.params);
  });
  const call = (method, params = {}) =>
    new Promise((resolve) => {
      counter += 1;
      waiting.set(counter, resolve);
      socket.send(JSON.stringify({ id: counter, method, params }));
    });
  for (const domain of ['Runtime', 'Log', 'Page']) await call(`${domain}.enable`);
  return {
    problems,
    async goto(url) {
      await call('Page.navigate', { url });
      await waitFor(
        async () =>
          (await this.evaluate(
            "location.href !== 'about:blank' && document.readyState === 'complete'",
          )) === true,
        { timeoutMs: 8000, stepMs: 100, what: `load ${url}` },
      );
    },
    async evaluate(expression) {
      const result = await call('Runtime.evaluate', {
        expression,
        awaitPromise: true,
        returnByValue: true,
      });
      if (result.result?.exceptionDetails) {
        throw new Error(
          `Runtime.evaluate exception: ${result.result.exceptionDetails.text ?? expression}`,
        );
      }
      return result.result?.result?.value;
    },
    close() {
      socket.close();
    },
  };
}

await withBrowser(async (port, profile) => {
  // 1. The committed file, opened from disk: no error, no policy violation, card radius valid.
  const file = await openTab(port);
  await file.goto(pathToFileURL(path.join(root, 'main/setup_page.html')).href);
  await waitFor(
    async () =>
      (await file.evaluate("document.getElementById('conn-text')?.textContent")) === '未连接',
    { what: 'file:// initial render' },
  );
  const cardRadius = await file.evaluate(
    "getComputedStyle(document.querySelector('.card')).borderRadius",
  );
  assert.notEqual(cardRadius, '0px', 'cards must have a defined border-radius');
  assert.deepEqual(file.problems, []);
  file.close();

  // 2. A USB session against a simulated serial port, with box-model checks on #bar.
  const preview = path.join(profile, 'setup-page-browser-usb.html');
  fs.writeFileSync(preview, usbPreviewHtml({ scenario: 'pending' }));
  const session = await openTab(port);
  await session.goto(pathToFileURL(preview).href);
  await waitFor(
    async () =>
      (await session.evaluate("document.getElementById('usb-connect')?.hidden")) === false,
    { what: 'USB connect button' },
  );
  await session.evaluate("document.getElementById('usb-connect').click()");
  await waitFor(
    async () =>
      (await session.evaluate("document.getElementById('conn-text')?.textContent")) ===
      '已连接（USB）',
    { what: 'USB session connected' },
  );
  const barMetrics = await session.evaluate(`(() => {
    const bar = document.getElementById('bar');
    const rect = bar.getBoundingClientRect();
    const style = getComputedStyle(bar);
    return {
      width: rect.width,
      height: rect.height,
      innerWidth: window.innerWidth,
      animationName: style.animationName,
    };
  })()`);
  assert.equal(barMetrics.width, barMetrics.innerWidth, '#bar must span the full viewport width');
  assert(barMetrics.height >= 60, `#bar must not collapse (got ${barMetrics.height}px)`);
  assert.equal(barMetrics.animationName, 'none', '#bar must not inherit skeleton pulse animation');
  // Sheet focus trap and focus restoration in a real browser.
  const focusCheck = await session.evaluate(`(() => {
    const trigger = document.getElementById('add-account');
    trigger.focus();
    trigger.click();
    const first = document.getElementById('pick-codex');
    const last = document.getElementById('sheet-cancel');
    last.focus();
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Tab', cancelable: true }));
    const wrappedForward = document.activeElement === first;
    document.dispatchEvent(
      new KeyboardEvent('keydown', { key: 'Tab', shiftKey: true, cancelable: true }),
    );
    const wrappedBackward = document.activeElement === last;
    last.blur();
    const blurredToBody = document.activeElement === document.body;
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Tab', cancelable: true }));
    const trappedFromBody = blurredToBody && document.activeElement === first;
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
    const restored = document.activeElement === trigger;
    return { wrappedForward, wrappedBackward, trappedFromBody, restored };
  })()`);
  assert.deepEqual(focusCheck, {
    wrappedForward: true,
    wrappedBackward: true,
    trappedFromBody: true,
    restored: true,
  });
  assert.match(
    await session.evaluate("document.getElementById('bar-text').textContent"),
    /2 项待验证/,
  );
  await session.evaluate("document.getElementById('finish').click()");
  await waitFor(
    async () =>
      (await session.evaluate("document.getElementById('bar-text').textContent")) === '全部正常',
    { timeoutMs: 10000, stepMs: 150, what: 'USB validation finished' },
  );
  assert.equal(
    await session.evaluate('performance.getEntriesByType("resource").length'),
    0,
    'a USB session loads nothing and requests nothing',
  );
  assert.deepEqual(session.problems, []);
  session.close();

  // 3. Hotspot HTTP path with real Content-Security-Policy response header and #code= fragment.
  const hotspot = createHotspotServer({ scenario: 'pending' });
  await new Promise((resolve) => hotspot.server.listen(0, '127.0.0.1', resolve));
  try {
    const hotspotPort = hotspot.server.address().port;
    const code = hotspot.device().config.code;
    const httpTab = await openTab(port);
    await httpTab.goto(`http://127.0.0.1:${hotspotPort}/#code=${code}`);
    await waitFor(
      async () =>
        (await httpTab.evaluate("document.getElementById('conn-text')?.textContent")) ===
        '已连接（热点）',
      { what: 'hotspot connected via #code=' },
    );
    assert.equal(
      await httpTab.evaluate('location.hash'),
      '',
      'access code cleared from address bar',
    );
    const hotspotBar = await httpTab.evaluate(`(() => {
      const rect = document.getElementById('bar').getBoundingClientRect();
      return { width: rect.width, height: rect.height, innerWidth: window.innerWidth };
    })()`);
    assert.equal(hotspotBar.width, hotspotBar.innerWidth);
    assert(hotspotBar.height >= 60);
    await httpTab.evaluate("document.getElementById('finish').click()");
    await waitFor(
      async () =>
        (await httpTab.evaluate("document.getElementById('conn-text')?.textContent")) ===
        '热点即将关闭',
      { what: 'hotspot setup_close state' },
    );
    assert.deepEqual(httpTab.problems, []);
    httpTab.close();
  } finally {
    hotspot.server.close();
  }

  // 4. In a frame the page does nothing. The same file, served without headers, as the page of
  //    GitHub Pages or a file would be.
  const server = http.createServer((request, response) => {
    if (request.url === '/inner') {
      response.writeHead(200, { 'Content-Type': 'text/html;charset=utf-8' });
      response.end(fs.readFileSync(path.join(root, 'main/setup_page.html')));
    } else {
      response.writeHead(200, { 'Content-Type': 'text/html' });
      response.end('<iframe id="inner" src="/inner"></iframe>');
    }
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  try {
    const framed = await openTab(port);
    await framed.goto(`http://127.0.0.1:${server.address().port}/`);
    await waitFor(
      async () =>
        (await framed.evaluate(
          "document.getElementById('inner')?.contentDocument?.documentElement?.textContent",
        )) === '请直接打开此页面',
      { what: 'frame guard text' },
    );
    assert.equal(
      await framed.evaluate(
        "document.getElementById('inner').contentDocument.getElementById('conn-text')",
      ),
      null,
      'nothing of the page was built',
    );
    framed.close();
  } finally {
    server.close();
  }
});

console.log(
  'Setup page in a browser: file://, USB & hotspot HTTP without console/CSP errors, ' +
    'box model, frame guard PASS',
);
