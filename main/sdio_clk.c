#include "sdio_clk.h"

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdio_clk";

#ifndef CONFIG_ESP_HOSTED_HOST_SDIO_SLOT
#define CONFIG_ESP_HOSTED_HOST_SDIO_SLOT 1
#endif

// sdmmc_io_enable_hs_mode() (ESP-IDF sdmmc_io.c) switches an SDIO card to high speed and sets
// card->max_freq_khz = SDMMC_FREQ_HIGHSPEED (40 MHz); sdmmc_init_host_frequency() then clocks the
// bus at that value, so ESP-Hosted's clock_freq_khz = 50000 silently ran at 40 MHz. The linker
// wraps the call (-Wl,--wrap=sdmmc_io_enable_hs_mode, main/CMakeLists.txt): after a successful HS
// switch the host's requested clock (max 50 MHz, the SDIO high-speed limit) is used instead. On the
// P4 a request above 40 MHz selects the 200 MHz SDIO PLL source (200 / 4 = exactly 50 MHz).
// A host request of <= 40 MHz is passed through unchanged.
esp_err_t __real_sdmmc_io_enable_hs_mode(sdmmc_card_t *card);
esp_err_t __wrap_sdmmc_io_enable_hs_mode(sdmmc_card_t *card);

esp_err_t __wrap_sdmmc_io_enable_hs_mode(sdmmc_card_t *card)
{
    esp_err_t err = __real_sdmmc_io_enable_hs_mode(card);
    if (err == ESP_OK && card->max_freq_khz == SDMMC_FREQ_HIGHSPEED &&
        card->host.max_freq_khz > SDMMC_FREQ_HIGHSPEED && card->host.max_freq_khz <= 50000) {
        card->max_freq_khz = card->host.max_freq_khz;
        ESP_LOGI(TAG, "SDIO high-speed mode: bus clock %d kHz (above the 40 MHz default cap)",
                 (int)card->max_freq_khz);
    }
    return err;
}

int sdio_clk_real_khz(void)
{
    int khz = 0;
    if (sdmmc_host_get_real_freq(CONFIG_ESP_HOSTED_HOST_SDIO_SLOT, &khz) != ESP_OK) return 0;
    return khz;
}
