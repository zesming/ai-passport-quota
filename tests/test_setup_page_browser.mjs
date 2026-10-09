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
import { usbPreviewHtml } from '../tools/preview_portable.mjs';

const chrome = process.env.CHROME;
if (!chrome) {
  console.log('Setup page in a browser: skipped (set CHROME to a Chrome or Chromium binary)');
  process.exit(0);
}
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function withBrowser(run) {
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'setup-page-browser-'));
  const port = 9300 + Math.floor(Math.random() * 500);
  const child = spawn(
    chrome,
    [
      '--headless=new',
      `--remote-debugging-port=${port}`,
      `--user-data-dir=${profile}`,
      '--no-first-run',
      '--disable-gpu',
      'about:blank',
    ],
    { stdio: 'ignore' },
  );
  try {
    for (let i = 0; i < 60; i += 1) {
      try {
        await fetch(`http://127.0.0.1:${port}/json/version`);
        break;
      } catch {
        await pause(200);
      }
    }
    return await run(port);
  } finally {
    child.kill();
    await pause(500);
    fs.rmSync(profile, { recursive: true, force: true, maxRetries: 5 });
  }
}

async function openTab(port) {
  const tab = await (
    await fetch(`http://127.0.0.1:${port}/json/new?about:blank`, { method: 'PUT' })
  ).json();
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
      await pause(1500);
    },
    async evaluate(expression) {
      const result = await call('Runtime.evaluate', {
        expression,
        awaitPromise: true,
        returnByValue: true,
      });
      return result.result?.result?.value;
    },
  };
}

await withBrowser(async (port) => {
  // 1. The committed file, opened from disk: no error, no policy violation.
  const file = await openTab(port);
  await file.goto(pathToFileURL(path.join(root, 'main/setup_page.html')).href);
  assert.equal(await file.evaluate("document.getElementById('conn-text').textContent"), '未连接');
  assert.deepEqual(file.problems, []);

  // 2. A USB session against a simulated serial port, from a file too.
  const preview = path.join(os.tmpdir(), 'setup-page-browser-usb.html');
  fs.writeFileSync(preview, usbPreviewHtml({ scenario: 'pending' }));
  const session = await openTab(port);
  await session.goto(pathToFileURL(preview).href);
  await session.evaluate("document.getElementById('usb-connect').click()");
  await pause(1500);
  assert.equal(
    await session.evaluate("document.getElementById('conn-text').textContent"),
    '已连接（USB）',
  );
  assert.match(
    await session.evaluate("document.getElementById('bar-text').textContent"),
    /2 项待验证/,
  );
  await session.evaluate("document.getElementById('finish').click()");
  await pause(3500);
  assert.equal(
    await session.evaluate("document.getElementById('bar-text').textContent"),
    '全部正常',
  );
  assert.equal(
    await session.evaluate('performance.getEntriesByType("resource").length'),
    0,
    'a USB session loads nothing and requests nothing',
  );
  assert.deepEqual(session.problems, []);

  // 3. In a frame the page does nothing. The same file, served without headers, as the page of
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
  const framed = await openTab(port);
  await framed.goto(`http://127.0.0.1:${server.address().port}/`);
  assert.equal(
    await framed.evaluate(
      "document.getElementById('inner').contentDocument.documentElement.textContent",
    ),
    '请直接打开此页面',
  );
  assert.equal(
    await framed.evaluate(
      "document.getElementById('inner').contentDocument.getElementById('conn-text')",
    ),
    null,
    'nothing of the page was built',
  );
  server.close();
});

console.log(
  'Setup page in a browser: file:// without console or CSP errors, USB session, frame guard PASS',
);
