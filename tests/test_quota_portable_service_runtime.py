"""Compile and exercise the complete portable controller with SDK/provider seams."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from runtime_helpers import ROOT


class PortableServiceRuntime(unittest.TestCase):
    def test_whole_controller_runtime(self):
        with tempfile.TemporaryDirectory(prefix="quota-portable-controller-") as directory:
            executable = Path(directory) / "controller"
            command = [
                os.environ.get("CC", "cc"),
                "-std=c11",
                "-D_DEFAULT_SOURCE",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-deprecated-declarations",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-I" + str(ROOT / "tests/portable_controller_sdk"),
                "-I" + str(ROOT / "main"),
                "-I" + str(ROOT / "components/bsp/include"),
                "-I" + str(ROOT / "tests/cjson"),
                str(ROOT / "tests/portable_controller_sdk/runtime.c"),
                str(ROOT / "main/quota_logic.c"),
                str(ROOT / "main/quota_catalog.c"),
                str(ROOT / "tests/cjson/cJSON.c"),
                "-lm",
                "-o",
                str(executable),
            ]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            for case in (
                "boot",
                "pending",
                "key",
                "cancel",
                "queue",
                "cadence",
                "state-contract",
                "physical-gate",
                "typed-read",
                "unknown",
                "delete",
                "generation",
                "recovery-previous",
                "recovery-target",
                "recovery-conflict",
                "invalid-model",
                "full-native",
                "received-cancel",
                "physical-login",
                "received-model-unknown",
                "sleep-open-deadline",
                "prepare-unknown",
                "prepare-unknown-cancel",
                "unknown-corruption",
                "unknown-conflict",
                "label-only-full",
                "offline-deadlines",
                "dirty-expired-deadline",
                "v2-reauth-cache",
                "unknown-valid-prior",
                "usb-key-network",
                "usb-login-reopen-save",
                "open-while-active",
                "open-failure-backoff",
                "sntp-servers",
                "usb-pauses-auto-refresh",
                "fresh-device",
                "fresh-orphan-retry",
                "account-limit",
                "network-limit",
                "usb-validate-e2e",
                "validate-only-pending",
                "validate-busy-and-expiry",
                "validate-hotspot",
                "keys-wait-for-network",
                "ap-top-up",
                "login-queue",
                "queue-counts-toward-limit",
                "network-remove",
                "access-code",
                "network-saved-state",
                "staged-credentials",
                "login-needs-network",
                "hotspot-aborts-unstarted-login",
                "staged-fail-stays",
                "staged-stale",
            ):
                with self.subTest(case=case):
                    result = subprocess.run([str(executable), case], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("whole controller runtime passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
