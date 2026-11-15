// On-device console command runner used by the remote CoAP "console" request.
//
// On a board with a UART shell (CONFIG_SHELL) the same command set is also
// reachable locally on the serial port; this module gives the server a way to
// run those commands over the encrypted CoAP/WireGuard link.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Execute a shell command and write up to out_len - 1 bytes of output into
// out (always NUL-terminated). Returns the number of bytes written.
//
// The caller must hold g_state_lock, because relay/pump commands mutate state.
size_t mraf_console_run(const char *command, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
