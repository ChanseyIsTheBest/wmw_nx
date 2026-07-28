/* util.h -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>

int debugPrintf(char *text, ...);
void debugLogFlush(void);

/* No CPU boost.
 *
 * The Switch's normal clock is far beyond what this game was built for -- it
 * targeted 2011 phones -- so ApmCpuBoostMode_FastLoad buys nothing measurable
 * and costs battery and heat. Frame timing in the main loop is the place to
 * check if that assumption ever stops holding:
 *
 *     frame 120: fps=60.0 render avg=4.2ms max=9.1ms | swap avg=0.3ms ...
 *
 * render avg well under the 16.7 ms budget means there is nothing to boost for.
 */

// libwmw.so was built with -mstack-protector-guard=tls: every guarded function
// reads the canary from tpidr_el0 + 0x28. The Switch leaves tpidr_el0 free
// (libnx keeps the thread TLS in tpidrro_el0), so we point it at a block with
// a guard. Must run on every thread that executes engine code.
void tls_setup_guard(void);

int ret0(void);
int retm1(void);

static inline void* armGetTlsRw(void) {
  void* ret;
  __asm__ ("mrs %x[data], s3_3_c13_c0_2" : [data] "=r" (ret));
  return ret;
}

static inline void armSetTlsRw(void *addr) {
  __asm__  ("msr s3_3_c13_c0_2, %0" : : "r"(addr));
}

static inline uint64_t umin(uint64_t a, uint64_t b) {
  return (a < b) ? a : b;
}

#endif
