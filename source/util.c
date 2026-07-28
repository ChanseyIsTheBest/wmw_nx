/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "libc_shim.h"
#include "config.h"

#if DEBUG_LOG

static int s_nxlinkSock = -1;

static void initNxLink(void) {
  if (R_FAILED(socketInitializeDefault()))
    return;
  s_nxlinkSock = nxlinkStdio();
  if (s_nxlinkSock < 0)
    socketExit();
}

static void deinitNxLink(void) {
  if (s_nxlinkSock >= 0) {
    close(s_nxlinkSock);
    socketExit();
    s_nxlinkSock = -1;
  }
}

void userAppInit(void) {
  initNxLink();
}

void userAppExit(void) {
  deinitNxLink();
}

#endif

#if DEBUG_LOG
static FILE *s_log_fp = NULL;
static char  s_log_buf[32 * 1024];
static int   s_log_since_flush = 0;
#endif

int debugPrintf(char *text, ...) {
#if DEBUG_LOG
  va_list list;

  // Open the log ONCE and keep it open. Re-opening (fopen/fclose) per line is a
  // multi-ms SD-card metadata op, and the engine logs from inside nativeRender,
  // so per-call reopens turn ordinary frames into render spikes. Open-once plus a
  // large user buffer and a batched flush keeps writes cheap while still landing
  // the log on disk frequently enough to survive a hang.
  if (!s_log_fp) {
    // The lazy open allocates a newlib handle, so it takes the same lock as
    // every other open in the port -- debugPrintf is called from the render
    // thread, the FMOD pump and FMOD's own internal threads. Writes to the
    // already-open stream below are left unlocked; newlib locks per-stream.
    wmw_file_lock();
    if (!s_log_fp) {
      s_log_fp = fopen(LOG_NAME, "w");
      if (s_log_fp)
        setvbuf(s_log_fp, s_log_buf, _IOFBF, sizeof(s_log_buf));
    }
    wmw_file_unlock();
  }

  if (s_log_fp) {
    va_start(list, text);
    vfprintf(s_log_fp, text, list);
    va_end(list);
    // Flush every ~32 lines instead of every line: batches the actual SD write
    // so it lands off the hot per-frame path most of the time.
    if (++s_log_since_flush >= 32) {
      fflush(s_log_fp);
      s_log_since_flush = 0;
    }
  }

  va_start(list, text);
  vprintf(text, list);
  va_end(list);
#endif
  return 0;
}

// shared TLS block for libwmw.so's stack-protector guard (read from tpidr_el0 +
// 0x28). A single block is fine: the canary only needs to be stable per
// function entry/exit, and the engine never writes thread-locals through
// tpidr_el0 (it has no __tls_get_addr import). Generously sized and zeroed so
// any stray tpidr-relative access lands in valid memory.
static uint8_t s_tls_block[0x1000] __attribute__((aligned(16)));

void tls_setup_guard(void) {
  *(uint64_t *)(s_tls_block + 0x28) = 0x0123456789ABCDEFull;
  armSetTlsRw(s_tls_block);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }

// Force any buffered log lines to disk (call on shutdown so the tail survives).
void debugLogFlush(void) {
#if DEBUG_LOG
  if (s_log_fp) {
    fflush(s_log_fp);
    s_log_since_flush = 0;
  }
#endif
}
