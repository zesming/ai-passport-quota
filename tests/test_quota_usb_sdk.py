"""Exercise USB polling against the installed SDK, with a synthetic FIFO."""

import os
from pathlib import Path
import unittest

from runtime_helpers import compile_and_run, vendor_function


class UsbSdk(unittest.TestCase):
    def test_basic_vfs_receives_without_a_driver(self):
        sdk = os.environ.get("IDF_PATH")
        if not sdk:
            self.skipTest("IDF_PATH is required for the real SDK USB test")
        source = (
            Path(sdk) / "components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c"
        ).read_text()
        functions = "\n".join(
            vendor_function(source, name, declaration)
            for name, declaration in (
                ("usb_serial_jtag_rx_char_no_driver", "static int"),
                ("usb_serial_jtag_read_char", "static int"),
                ("usb_serial_jtag_return_char", "static void"),
                ("usb_serial_jtag_read", "static ssize_t"),
                ("usb_serial_jtag_fcntl", "static int"),
                ("usb_serial_jtag_vfs_use_nonblocking", "void"),
            )
        )
        harness = r'''
#include "quota_logic.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define NONE (-1)
#define USJ_LOCAL_FD STDIN_FILENO
#define MIN(a, b) ((a) < (b) ? (a) : (b))
enum { ESP_LINE_ENDINGS_LF, ESP_LINE_ENDINGS_CR, ESP_LINE_ENDINGS_CRLF };
static struct {
    int peek_char, rx_mode, read_lock, write_lock;
    bool non_blocking;
    int (*rx_func)(int);
    void (*tx_func)(int, int);
    int (*fsync_func)(int);
} s_ctx = {.peek_char = NONE, .rx_mode = ESP_LINE_ENDINGS_LF};
static unsigned locks, fifo_calls;
static const char *fifo;
static size_t fifo_position, fifo_available;
static void _lock_acquire_recursive(int *lock) { (void)lock; locks++; }
static void _lock_release_recursive(int *lock) { (void)lock; assert(locks); locks--; }
static bool usb_serial_jtag_is_connected(void) { return true; }
static size_t usb_serial_jtag_get_read_bytes_available(void) { return 0; } /* No driver. */
static int usb_serial_jtag_ll_read_rxfifo(uint8_t *byte, int length) {
    assert(length == 1); fifo_calls++;
    if (fifo_position == fifo_available) return 0;
    *byte = (uint8_t)fifo[fifo_position++]; return 1;
}
static void usb_serial_jtag_tx_char_no_driver(int fd, int ch) { (void)fd; (void)ch; }
static int usb_serial_jtag_wait_tx_done_no_driver(int fd) { (void)fd; return 0; }
/* SDK callbacks intentionally leave some VFS parameters unused. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
'''
        harness += functions
        harness += r'''
#pragma GCC diagnostic pop
/* quota_service_start() calls fcntl(); route it to the SDK's USB VFS implementation. */
#include <stdarg.h>
int fcntl(int fd, int cmd, ...)
{
    va_list arguments;
    va_start(arguments, cmd);
    int argument = va_arg(arguments, int);
    va_end(arguments);
    return usb_serial_jtag_fcntl(fd, cmd, argument);
}
bool quota_service_init(void);
bool quota_service_start(void);
void network_task(void *argument);
static unsigned tasks;
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority,
                void **task)
{
    assert(fn == network_task && strcmp(name, "quota_network") == 0);
    assert(stack == 10240 && arg == NULL && priority == 5);
    tasks++;
    *task = (void *)1;
    return 1;
}
int main(void)
{
    char frame[4096];
    memset(frame, 'x', sizeof(frame));
    memcpy(frame, "@AIQ:", 5);
    frame[sizeof(frame) - 1] = '\n';
    fifo = frame;
    fifo_available = 64;
    usb_serial_jtag_vfs_use_nonblocking();
    usb_serial_jtag_fcntl(0, F_SETFL, O_NONBLOCK);
    char byte;
    /* The old startup cannot receive even when the hardware FIFO has data. */
    assert(usb_serial_jtag_read(0, &byte, 1) == -1 && errno == EWOULDBLOCK);
    assert(fifo_calls == 0 && fifo_position == 0);

    assert(quota_service_init() && quota_service_start() && !s_ctx.non_blocking && tasks == 1);
    quota_frame_decoder_t decoder;
    quota_frame_decoder_init(&decoder);
    const char *decoded = NULL;
    size_t length = 0;
    unsigned complete = 0;
    for (size_t packet = 64; packet <= sizeof(frame); packet += 64) {
        fifo_available = packet;
        while (usb_serial_jtag_read(0, &byte, 1) == 1) {
            quota_frame_result_t result =
                quota_frame_decoder_feed(&decoder, byte, &decoded, &length);
            assert(result != QUOTA_FRAME_TOO_LONG);
            if (result == QUOTA_FRAME_COMPLETE) {
                complete++;
                assert(length == sizeof(frame) - 1);
                assert(memcmp(decoded, frame, length) == 0);
            }
        }
        assert(fifo_position == packet && errno == EWOULDBLOCK && locks == 0);
    }
    assert(complete == 1);
    unsigned calls = fifo_calls;
    assert(usb_serial_jtag_read(0, &byte, 1) == -1 && fifo_calls == calls + 1);
    assert(quota_service_start() && tasks == 1); /* No additional reader/task. */
    puts("real SDK USB polling and 4096-byte frame: PASS");
}
'''
        compile_and_run(
            harness,
            "ai-quota-usb-sdk-",
            ("main/quota_logic.c", "main/quota_service.c", "tests/cjson/cJSON.c"),
            host_sdk=True,
        )


if __name__ == "__main__":
    unittest.main()
