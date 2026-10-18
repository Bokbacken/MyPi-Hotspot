#pragma once

// SDIO bus clock helpers (v0.9.5). ESP-IDF's SDIO init caps the card clock at 40 MHz
// (SDMMC_FREQ_HIGHSPEED) even when ESP-Hosted asks for more; sdio_clk.c lifts that cap so the
// Optimize setting "50 MHz" really runs the bus at 50 MHz.

/** Clock the SDMMC host actually runs the ESP-Hosted slot at (kHz), 0 if unknown/not initialized. */
int sdio_clk_real_khz(void);
