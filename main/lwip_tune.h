#pragma once

// v0.9.2 LwIP / NAT tuning. Force-included into every lwIP source file (main/CMakeLists.txt,
// "-include lwip_tune.h") so ip4_napt.c reads the NAPT table size and the idle TCP timeout from
// variables set by opt_config.c instead of the compile-time defaults in lwip/lwip_napt.h
// (IP_NAPT_MAX 512, IP_NAPT_TIMEOUT_MS_TCP 30 min). Both macros are only used at runtime there:
// IP_NAPT_MAX when the table is allocated (first NAPT enable), the timeout in every NAPT GC pass.

#include <stdint.h>

extern uint16_t mypi_napt_max_entries;              // 512 / 1024 / 2048, set at boot (reboot required)
extern volatile uint32_t mypi_napt_tcp_timeout_ms;  // 7200 / 3600 / 1800 s in ms, runtime

#define IP_NAPT_MAX             (mypi_napt_max_entries)
#define IP_NAPT_TIMEOUT_MS_TCP  (mypi_napt_tcp_timeout_ms)
