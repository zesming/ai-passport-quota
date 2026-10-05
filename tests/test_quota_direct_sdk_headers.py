#!/usr/bin/env python3
"""Exercise the installed SDK header serializer, without vendoring its source."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DirectSdkHeaders(unittest.TestCase):
    def test_real_sdk_maximum_bearer_header(self):
        sdk = os.environ.get("IDF_PATH")
        if not sdk:
            self.skipTest("IDF_PATH is required for the real SDK header test")
        library = Path(sdk) / "components/esp_http_client/lib"
        self.assertTrue((library / "http_header.c").is_file())
        with tempfile.TemporaryDirectory(prefix="quota-sdk-headers-") as directory:
            tmp = Path(directory)
            (tmp / "esp_err.h").write_text("""#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_NOT_FOUND 3
""")
            (tmp / "esp_log.h").write_text("""#pragma once
#define ESP_LOGE(tag,...) ((void)(tag))
#define ESP_LOGD(tag,...) ((void)(tag))
""")
            (tmp / "esp_check.h").write_text("""#pragma once
#include "esp_err.h"
#define unlikely(value) (value)
#define ESP_RETURN_ON_FALSE(value,result,tag,...) do { (void)(tag); if (!(value)) return result; } while (0)
""")
            (tmp / "test.c").write_text(r'''
#include "http_header.h"
#include "quota_direct_logic.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char *token = malloc(QUOTA_DIRECT_ACCESS_BYTES + 2); assert(token);
    memset(token, 'A', QUOTA_DIRECT_ACCESS_BYTES); token[QUOTA_DIRECT_ACCESS_BYTES] = 0;
    assert(quota_direct_token_is_safe(token, QUOTA_DIRECT_ACCESS_BYTES));
    token[QUOTA_DIRECT_ACCESS_BYTES] = 'A'; token[QUOTA_DIRECT_ACCESS_BYTES + 1] = 0;
    assert(!quota_direct_token_is_safe(token, QUOTA_DIRECT_ACCESS_BYTES));
    token[QUOTA_DIRECT_ACCESS_BYTES] = 0;
    char *bearer = malloc(QUOTA_DIRECT_ACCESS_BYTES + 8); assert(bearer);
    memcpy(bearer, "Bearer ", 7); memcpy(bearer + 7, token, QUOTA_DIRECT_ACCESS_BYTES + 1);
    char account[129]; memset(account, 'a', 128); account[128] = 0;
    http_header_handle_t header = http_header_init(); assert(header);
    assert(http_header_set(header, "Host", "chatgpt.com") == ESP_OK);
    assert(http_header_set(header, "User-Agent", "AIQuota-Portable-Experimental/1") == ESP_OK);
    assert(http_header_set(header, "Accept", "application/json") == ESP_OK);
    assert(http_header_set(header, "Authorization", bearer) == ESP_OK);
    assert(http_header_set(header, "ChatGPT-Account-ID", account) == ESP_OK);
    assert(http_header_set(header, "Content-Type", "application/x-www-form-urlencoded") == ESP_OK);
    assert(http_header_set(header, "Content-Length", "32768") == ESP_OK);
    /* SDK owns its own copy, so clearing the temporary cannot alter the wire. */
    memset(bearer, 0, QUOTA_DIRECT_ACCESS_BYTES + 8); free(bearer);
    char *stored = NULL; assert(http_header_get(header, "Authorization", &stored) == ESP_OK);
    assert(stored && strlen(stored) == QUOTA_DIRECT_ACCESS_BYTES + 7);

    char old[1024] = {0}; int available = sizeof(old);
    int next = http_header_generate_string(header, 0, old, &available);
    assert(next == 3 && available > 0 && !strstr(old, "Authorization"));
    available = sizeof(old); old[0] = 0;
    assert(http_header_generate_string(header, next, old, &available) == next);
    assert(available == 0); /* The old buffer cannot make forward progress. */

    const char *line = "GET /backend-api/wham/rate-limit-reset-credits HTTP/1.1\r\n";
    size_t capacity = 1024 + strlen(token);
    char *wire = calloc(1, capacity); assert(wire);
    size_t prefix = strlen(line); memcpy(wire, line, prefix);
    available = (int)(capacity - prefix);
    next = http_header_generate_string(header, 0, wire + prefix, &available);
    assert(next == 7);
    size_t total = prefix + (size_t)available; assert(total < capacity);
    char *authorization = strstr(wire, "Authorization: Bearer "); assert(authorization);
    authorization += strlen("Authorization: Bearer ");
    assert(!memcmp(authorization, token, QUOTA_DIRECT_ACCESS_BYTES));
    assert(!memcmp(authorization + QUOTA_DIRECT_ACCESS_BYTES, "\r\n", 2));
    assert(strstr(wire, account) && !memcmp(wire + total - 4, "\r\n\r\n", 4));
    memset(stored, 0, strlen(stored));
    assert(http_header_delete(header, "Authorization") == ESP_OK);
    assert(http_header_get(header, "Authorization", &stored) == ESP_OK && !stored);
    assert(http_header_destroy(header) == ESP_OK);
    memset(wire, 0, capacity); free(wire); memset(token, 0, QUOTA_DIRECT_ACCESS_BYTES + 2); free(token);
    puts("real SDK complete 8192-byte Bearer header: PASS");
}
''')
            output = tmp / "headers"
            command = [os.environ.get("CC", "cc"), "-std=gnu11", "-D_GNU_SOURCE",
                       "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations", "-fsanitize=address,undefined",
                       "-fno-omit-frame-pointer", "-g", "-I" + str(tmp),
                       "-I" + str(library / "include"), "-I" + str(ROOT / "main"),
                       "-I" + str(ROOT / "tests/cjson"), str(tmp / "test.c"),
                       str(library / "http_header.c"), str(library / "http_utils.c"),
                       str(ROOT / "main/quota_direct_logic.c"), str(ROOT / "main/quota_logic.c"),
                       str(ROOT / "tests/cjson/cJSON.c"), "-lm", "-o", str(output)]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(output)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("real SDK complete 8192-byte Bearer header: PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
