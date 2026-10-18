#pragma once

// LwIP / NAT / TCP tuning (v0.9.2, TCP window added v0.9.4). Force-included into every lwIP source
// file (main/CMakeLists.txt, "-include lwip_tune.h") so lwIP reads these values from variables set by
// opt_config.c at boot instead of compile-time constants:
//   IP_NAPT_MAX            NAPT table size, used when the table is allocated (first NAPT enable)
//   IP_NAPT_TIMEOUT_MS_TCP NAPT idle TCP timeout, read in every NAPT GC pass (runtime)
//   TCP_WND                lwIP receive window (rcv_wnd of new pcbs, window updates, SYN window)
//
// TCP_WND: lwipopts.h defines it unconditionally as CONFIG_LWIP_TCP_WND_DEFAULT, so that macro is
// replaced here. lwIP's init.c also checks TCP_WND in #if sanity checks; in #if an unknown identifier
// evaluates to 0, so the expression below is the 128 KB maximum for the preprocessor (checks pass:
// <= 0xFFFF << TCP_RCV_SCALE) and the runtime variable in C code. mypi_tcp_wnd is never 0.
// Only TCP terminated on the P4 itself (web UI, OTA, iperf on the device) uses this window; NAT
// forwarded client traffic is not affected (the client and the remote server negotiate that window).

#include <stdint.h>
#include "sdkconfig.h"

extern uint16_t mypi_napt_max_entries;              // 512 / 1024 / 2048, set at boot (reboot required)
extern volatile uint32_t mypi_napt_tcp_timeout_ms;  // 7200 / 3600 / 1800 s in ms, runtime
extern uint32_t mypi_tcp_wnd;                       // 32 / 64 / 128 KB in bytes, set at boot (reboot required)

#define IP_NAPT_MAX             (mypi_napt_max_entries)
#define IP_NAPT_TIMEOUT_MS_TCP  (mypi_napt_tcp_timeout_ms)

#undef CONFIG_LWIP_TCP_WND_DEFAULT
#define CONFIG_LWIP_TCP_WND_DEFAULT (mypi_tcp_wnd ? mypi_tcp_wnd : 131072)
