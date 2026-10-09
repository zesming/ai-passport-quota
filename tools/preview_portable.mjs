// Manually started synthetic fixture for the setup page; it never contacts a Passport or a
// provider. A simulated Passport (tools/fake_device.mjs) answers over both transports.
//
//   node tools/preview_portable.mjs [--scenario=default] [--firmware=3] [--port=4328]
//
// Hotspot transport: open the printed http://127.0.0.1 address. The page is served the way the
// Passport serves it, with the same Content-Security-Policy header.
// USB transport: open the printed file:// address. A simulated serial port stands in for
// navigator.serial. Open the browser console to drive the simulated Passport:
//   passportSim.approveLogin()  finish a ChatGPT authorization that waits on the Passport
//   passportSim.openUsb()       open the USB setup window again after it timed out
//
// Scenarios: default, empty, pending, failed, full.  --firmware=2 pretends to be a firmware that
// speaks protocol 2; --firmware=4 one that speaks a newer protocol.
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { buildSetupPage } from './build_setup_page.mjs';
import { createSimDevice, SCENARIOS } from './fake_device.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..');

// The Passport sends its page's own meta CSP as a header and adds frame-ancestors, which a meta
// tag cannot carry (see portal_csp in main/quota_portal.c).
export function deviceCsp(html) {
  const meta = html.match(/http-equiv="Content-Security-Policy" content="([^"]*)"/);
  return `${meta ? meta[1] : "default-src 'none'"}; frame-ancestors 'none'`;
}

export function simulatedDevice({ scenario = 'default', firmware = 3, session = 'hotspot' } = {}) {
  const device = createSimDevice({
    protocol: Number(firmware),
    firmware: `${firmware}.0.0-preview`,
  });
  (SCENARIOS[scenario] ?? SCENARIOS.default)(device);
  if (session === 'hotspot') device.openHotspot();
  else if (session === 'usb') device.openUsb();
  return device;
}

// The setup page with a simulated Web Serial port in front of it, for file:// previews.
export function usbPreviewHtml({ scenario = 'default', firmware = 3 } = {}) {
  const source = fs
    .readFileSync(path.join(here, 'fake_device.mjs'), 'utf8')
    .replace(/^export\s+/gm, '');
  const preamble = `(() => {
${source}
const device = createSimDevice({ protocol: ${Number(firmware)}, firmware: '${firmware}.0.0-preview' });
SCENARIOS[${JSON.stringify(scenario)}](device);
device.openUsb();
Object.defineProperty(navigator, 'serial', { value: createSimSerial(device), configurable: true });
window.passportSim = device;
setInterval(() => device.advance(250), 250);
})();`;
  return buildSetupPage({ preamble }).html;
}

export function createHotspotServer(options = {}) {
  let device = simulatedDevice(options);
  const html = () => fs.readFileSync(path.join(root, 'main/setup_page.html'), 'utf8');
  const timer = setInterval(() => device.advance(250), 250);
  const server = http.createServer(async (request, response) => {
    const url = new URL(request.url, 'http://127.0.0.1');
    if (url.pathname.startsWith('/__preview/')) {
      device = simulatedDevice({ ...options, scenario: url.pathname.slice(11) });
      response.writeHead(302, { Location: `/#code=${device.config.code}` });
      response.end();
      return;
    }
    if (url.pathname === '/' && request.method === 'GET') {
      const page = html();
      response.writeHead(200, {
        'Content-Type': 'text/html;charset=utf-8',
        'Cache-Control': 'no-store',
        'Referrer-Policy': 'no-referrer',
        'X-Content-Type-Options': 'nosniff',
        'X-Frame-Options': 'DENY',
        'Content-Security-Policy': deviceCsp(page),
      });
      response.end(page);
      return;
    }
    if (url.pathname.startsWith('/api/')) {
      let body = '';
      for await (const chunk of request) body += chunk;
      let parsed = null;
      try {
        parsed = body ? JSON.parse(body) : null;
      } catch {
        parsed = null;
      }
      const headers = { 'X-AIQ-Access': request.headers['x-aiq-access'] };
      const result = device.http(request.method, url.pathname, headers, parsed);
      response.writeHead(result.status, {
        'Content-Type': 'application/json;charset=utf-8',
        'Cache-Control': 'no-store',
      });
      response.end(JSON.stringify(result.json));
      return;
    }
    response.writeHead(404);
    response.end();
  });
  server.on('close', () => clearInterval(timer));
  return { server, device: () => device };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const option = (name, fallback) =>
    process.argv.find((item) => item.startsWith(`--${name}=`))?.slice(name.length + 3) ?? fallback;
  const options = { scenario: option('scenario', 'default'), firmware: option('firmware', '3') };
  const port = Number(option('port', '4328'));
  const usbFile = path.join(os.tmpdir(), 'passport-setup-preview-usb.html');
  fs.writeFileSync(usbFile, usbPreviewHtml(options));
  const { server, device } = createHotspotServer(options);
  server.listen(port, '127.0.0.1', () => {
    process.stdout.write(
      `Hotspot: http://127.0.0.1:${port}/#code=${device().config.code}\n` +
        `USB:     ${pathToFileURL(usbFile)}\n` +
        'Scenarios: /__preview/default /__preview/empty /__preview/pending /__preview/failed ' +
        '/__preview/full\n',
    );
  });
}
