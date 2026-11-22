"""Cross-boundary contract tests between the real setup page JS and real firmware C."""

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from runtime_helpers import ROOT

VERIFIER_HARNESS = r"""
#include "quota_portal.h"
#include "quota_service.h"
#include "quota_usb.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static quota_service_view_t s_view;
void quota_service_lock(void) {}
void quota_service_unlock(void) {}
quota_service_view_t *quota_service_view(void) { return &s_view; }
void quota_service_post(quota_app_event_kind_t kind) { (void)kind; }
void quota_service_wake_network(void) {}

static bool content_type_ok(const char *ct)
{
    return !strcmp(ct, "application/json") || !strcmp(ct, "application/json; charset=utf-8");
}

int main(void)
{
    char line[QUOTA_MAX_PROVISION_FRAME_BYTES + 256];
    unsigned http_count = 0, usb_count = 0;
    while (fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0)
            continue;
        if (!strncmp(line, "HTTP\t", 5)) {
            char *tab = strchr(line + 5, '\t');
            assert(tab != NULL);
            *tab = '\0';
            const char *ct = line + 5;
            const char *body = tab + 1;
            if (!content_type_ok(ct)) {
                fprintf(stderr, "HTTP Content-Type rejected: %s\n", ct);
                return 2;
            }
            quota_portable_command_t cmd;
            if (!quota_portal_parse_command(body, strlen(body), &cmd)) {
                fprintf(stderr, "HTTP command rejected by firmware: %s\n", body);
                return 3;
            }
            http_count++;
        } else if (!strncmp(line, "USB\t", 4)) {
            const char *frame = line + 4;
            quota_usb_request_t req;
            const char *error = NULL;
            if (!quota_usb_parse(frame, strlen(frame), &req, &error)) {
                fprintf(stderr, "USB frame rejected by firmware (%s): %s\n",
                        error ? error : "unknown", frame);
                return 4;
            }
            usb_count++;
        } else {
            fprintf(stderr, "unknown record: %s\n", line);
            return 5;
        }
    }
    assert(http_count >= 14 && usb_count >= 14);
    printf("verified http=%u usb=%u\n", http_count, usb_count);
    return 0;
}
"""

NODE_EMITTER = r"""
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { createHttpTransport } from './main/setup/transport_http.mjs';
import { openUsbDeviceSession } from './main/setup/transport_serial.mjs';
import { createSimDevice, createSimSerial } from './tools/fake_device.mjs';
import { loadPage } from './tests/fake_dom.mjs';

const mode = process.argv[1];

if (mode === 'emit') {
  const records = [];
  const CODE = 'K7QM-2X9D-PA4T-Z8RW';
  const http = createHttpTransport(async (route, init = {}) => {
    if (init.method === 'POST' && route === '/api/command') {
      const ct = init.headers?.['Content-Type'] ?? init.headers?.['content-type'] ?? '';
      records.push(`HTTP\t${ct}\t${init.body}`);
      const parsed = JSON.parse(init.body);
      return {
        ok: true,
        status: 202,
        json: async () => ({ request_id: parsed.request_id, accepted: true }),
      };
    }
    return { ok: true, status: 200, json: async () => ({ protocol: 3, firmware: '3.0.0-test' }) };
  });
  http.setCode(CODE);

  const commands = [
    { op: 'network_save', ssid: 'HomeWiFi', password: 'password123', phone_utc: 1800000000 },
    { op: 'network_save', ssid: 'Cafe', password: '', open_network: true, network_index: 1 },
    { op: 'network_remove', network_index: 1 },
    { op: 'deepseek_save', label: 'Work', api_key: 'sk-valid-key-123' },
    {
      op: 'deepseek_save',
      label: 'Renamed',
      account_id: '0123456789abcdef0123456789abcdef',
    },
    { op: 'codex_queue', label: 'TeamGPT' },
    { op: 'codex_queue', account_id: '0123456789abcdef0123456789abcdef' },
    { op: 'account_remove', account_id: '0123456789abcdef0123456789abcdef' },
    {
      op: 'settings_save',
      refresh_seconds: 900,
      auto_refresh: true,
      screen_timeout_seconds: 120,
    },
    { op: 'operation_cancel', target_request_id: 'a1b2c3d4' },
    { op: 'setup_close' },
  ];

  let seq = 1;
  for (const cmd of commands) {
    const request_id = seq.toString(16).padStart(8, '0');
    seq += 1;
    await http.command({ v: 3, request_id, ...cmd });
  }

  // Also drive the real setup_page.html UI over HTTP so app.mjs-built payloads (including
  // network_save, deepseek_save, codex_queue, settings_save, setup_close) hit the real C parser.
  const html = fs.readFileSync(path.resolve('main/setup_page.html'), 'utf8');
  const uiDevice = createSimDevice({ accounts: [], networks: [] });
  uiDevice.openHotspot(CODE);
  const uiPage = loadPage(html, {
    protocol: 'http:',
    origin: 'http://192.168.4.1',
    hash: `#code=${CODE}`,
    fetchImpl: async (route, init = {}) => {
      if (init.method === 'POST' && route === '/api/command') {
        const ct = init.headers?.['Content-Type'] ?? init.headers?.['content-type'] ?? '';
        records.push(`HTTP\t${ct}\t${init.body}`);
      }
      const parsed = init.body ? JSON.parse(init.body) : null;
      const res = uiDevice.http(init.method, route, init.headers, parsed);
      return { ok: res.status < 400, status: res.status, json: async () => res.json };
    },
  });
  await uiPage.advance(500);
  await uiPage.node('wifi-ssid').type_('ContractNet');
  await uiPage.node('wifi-password').type_('secret1234');
  await uiPage.node('wifi-form').dispatch('submit');
  await uiPage.advance(500);
  uiPage.node('refresh-select').value = '900';
  await uiPage.node('refresh-select').dispatch('change');
  await uiPage.advance(500);
  uiPage.node('finish').click();
  await uiPage.advance(500);

  const device = createSimDevice({ accounts: [], networks: [] });
  device.openUsb();
  const rawSerial = createSimSerial(device);
  const decoder = new TextDecoder();
  const wrappedSerial = {
    async requestPort() {
      const port = await rawSerial.requestPort();
      const origOpen = port.open.bind(port);
      port.open = async (...args) => {
        await origOpen(...args);
        const origWriter = port.writable.getWriter.bind(port.writable);
        port.writable.getWriter = () => {
          const writer = origWriter();
          const origWrite = writer.write.bind(writer);
          writer.write = async (chunk) => {
            const text = decoder.decode(chunk);
            for (const line of text.split('\n')) {
              if (line.trim()) records.push(`USB\t${line.trim()}`);
            }
            return origWrite(chunk);
          };
          return writer;
        };
      };
      return port;
    },
  };

  const port = await wrappedSerial.requestPort();
  const session = await openUsbDeviceSession(port);
  await session.openSession();
  await session.stateGet();
  for (const cmd of [...commands, { op: 'validate' }]) {
    const request_id = seq.toString(16).padStart(8, '0');
    seq += 1;
    try {
      await session.command({ v: 3, request_id, ...cmd });
    } catch {
      // Fake device may reject stateful follow-ups; the real C parser verifies the emitted frame.
    }
  }
  await session.close();
  process.stdout.write(records.join('\n') + '\n');
} else if (mode === 'render') {
  const apState = JSON.parse(process.argv[2]);
  const usbState = JSON.parse(process.argv[3]);
  const html = fs.readFileSync(path.resolve('main/setup_page.html'), 'utf8');

  const apPage = loadPage(html, {
    protocol: 'http:',
    origin: 'http://192.168.4.1',
    hash: '#code=K7QM-2X9D-PA4T-Z8RW',
    fetchImpl: async (route) => {
      if (route === '/api/state') return { ok: true, status: 200, json: async () => apState };
      return { ok: true, status: 202, json: async () => ({ accepted: true }) };
    },
  });
  await apPage.advance(500);
  const apAccounts = apPage.node('account-list').textContent;
  const apNetworks = apPage.node('wifi-list').textContent;
  assert(
    apAccounts.includes('$12.30'),
    `AP mode must render real firmware USD balance: ${apAccounts}`,
  );
  assert(apAccounts.includes('TeamGPT'), `AP mode must render queued ChatGPT: ${apAccounts}`);
  assert(apNetworks.includes('old-hotspot'), `AP mode must render saved network: ${apNetworks}`);
  assert(
    apPage.node('finish').textContent.includes('完成设置'),
    'AP mode footer must show 完成设置',
  );

  const usbDevice = createSimDevice({ accounts: [], networks: [] });
  usbDevice.openUsb();
  const origFrame = usbDevice.frame.bind(usbDevice);
  usbDevice.frame = (frame) => {
    const reply = origFrame(frame);
    if (reply && reply.op === 'state') {
      return { ...reply, state: usbState };
    }
    return reply;
  };
  const usbPage = loadPage(html, {
    protocol: 'file:',
    serial: createSimSerial(usbDevice),
  });
  usbPage.node('usb-connect').click();
  for (let i = 0; i < 40 && !usbPage.node('conn-text').textContent.startsWith('已连接'); i += 1) {
    await usbPage.advance(250);
  }
  assert(usbPage.node('conn-text').textContent.startsWith('已连接'), 'USB page failed to connect');
  const usbAccounts = usbPage.node('account-list').textContent;
  const usbNetworks = usbPage.node('wifi-list').textContent;
  assert(
    usbAccounts.includes('$12.30'),
    `USB mode must render real firmware USD balance: ${usbAccounts}`,
  );
  assert(usbAccounts.includes('TeamGPT'), `USB mode must render queued ChatGPT: ${usbAccounts}`);
  assert(usbNetworks.includes('old-hotspot'), `USB mode must render saved network: ${usbNetworks}`);
  console.log('render contract PASS');
}
"""


class SetupProtocolContract(unittest.TestCase):
    def test_frontend_wire_frames_accepted_by_real_firmware_parsers(self):
        with tempfile.TemporaryDirectory(prefix="quota-contract-") as directory:
            tmp = Path(directory)
            (tmp / "verifier.c").write_text(VERIFIER_HARNESS)
            verifier = tmp / "verifier"
            cc = os.environ.get("CC", "cc")
            compiled = subprocess.run(
                [
                    cc,
                    "-std=c11",
                    "-D_DEFAULT_SOURCE",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wno-deprecated-declarations",
                    "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=undefined",
                    "-fno-omit-frame-pointer",
                    "-DQUOTA_HOST_TEST",
                    "-I" + str(ROOT / "tests/host_sdk"),
                    "-I" + str(ROOT / "tests/bsp_stubs"),
                    "-I" + str(ROOT / "components/bsp/include"),
                    "-I" + str(ROOT / "main"),
                    "-I" + str(ROOT / "tests/cjson"),
                    str(tmp / "verifier.c"),
                    str(ROOT / "main/quota_portal.c"),
                    str(ROOT / "main/quota_usb.c"),
                    str(ROOT / "main/quota_json.c"),
                    str(ROOT / "main/quota_logic.c"),
                    str(ROOT / "tests/host_sdk/host_sdk_embedded.c"),
                    str(ROOT / "tests/host_sdk/host_sdk_defaults.c"),
                    str(ROOT / "tests/host_sdk/module_defaults.c"),
                    str(ROOT / "tests/cjson/cJSON.c"),
                    "-lm",
                    "-o",
                    str(verifier),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)

            emitted = subprocess.run(
                ["node", "--input-type=module", "-e", NODE_EMITTER, "emit"],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            self.assertEqual(emitted.returncode, 0, emitted.stderr)

            verified = subprocess.run(
                [str(verifier)], input=emitted.stdout, capture_output=True, text=True
            )
            self.assertEqual(verified.returncode, 0, verified.stderr)
            self.assertIn("verified http=", verified.stdout)

    def test_real_firmware_state_json_renders_in_real_setup_page(self):
        with tempfile.TemporaryDirectory(prefix="quota-contract-state-") as directory:
            controller = Path(directory) / "controller"
            compiled = subprocess.run(
                [
                    os.environ.get("CC", "cc"),
                    "-std=c11",
                    "-D_DEFAULT_SOURCE",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wno-deprecated-declarations",
                    "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=undefined",
                    "-fno-omit-frame-pointer",
                    "-I" + str(ROOT / "tests/host_sdk"),
                    "-I" + str(ROOT / "tests/bsp_stubs"),
                    "-I" + str(ROOT / "main"),
                    "-I" + str(ROOT / "components/bsp/include"),
                    "-I" + str(ROOT / "tests/cjson"),
                    str(ROOT / "tests/portable_controller_sdk/runtime.c"),
                    str(ROOT / "main/quota_logic.c"),
                    str(ROOT / "main/quota_catalog.c"),
                    str(ROOT / "tests/cjson/cJSON.c"),
                    "-lm",
                    "-o",
                    str(controller),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            dumped = subprocess.run(
                [str(controller), "dump-contract-state"], capture_output=True, text=True
            )
            self.assertEqual(dumped.returncode, 0, dumped.stdout + dumped.stderr)
            ap_json = usb_json = None
            for line in dumped.stdout.splitlines():
                if line.startswith("AP_STATE:"):
                    ap_json = line[len("AP_STATE:") :]
                elif line.startswith("USB_STATE:"):
                    usb_json = line[len("USB_STATE:") :]
            self.assertIsNotNone(ap_json)
            self.assertIsNotNone(usb_json)
            # Sanity-check valid JSON before handing off to real frontend page.
            json.loads(ap_json)
            json.loads(usb_json)

            rendered = subprocess.run(
                ["node", "--input-type=module", "-e", NODE_EMITTER, "render", ap_json, usb_json],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            self.assertEqual(rendered.returncode, 0, rendered.stdout + rendered.stderr)
            self.assertIn("render contract PASS", rendered.stdout)


if __name__ == "__main__":
    unittest.main()
