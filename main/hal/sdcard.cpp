// Ported from badge-bsp/targets/tanmatsu/badge_bsp_storage.c (the version the
// launcher uses). Two things here are not obvious and must not be "cleaned up":
//
// - ESP-IDF 6 gives the ESP32-P4 one SDMMC host controller shared by both slots.
//   ESP-Hosted (WiFi over SDIO, slot 1) already claimed it, so sdmmc_host_init()
//   would fail with ESP_ERR_NOT_FOUND. host.init is replaced by a no-op.
// - The matching deinit must only release slot 0, and it must first make the
//   SDMMC ISR point at a live slot, otherwise the next SDIO interrupt from
//   ESP-Hosted dereferences the slot that was just removed.

#include "sdcard.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_protocol_defs.h"
#include "sd_pwr_ctrl.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
}

static char const TAG[] = "adsb sdcard";

#define SD_MOUNT_POINT "/sd"

#if defined(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE) && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0))
#define SDCARD_WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT 1
#else
#define SDCARD_WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT 0
#endif

static sdmmc_card_t*        _card       = nullptr;
static sd_pwr_ctrl_handle_t _pwr_handle = nullptr;
// 2KB-aligned buffer in internal RAM: required for SDMMC, and ESP-IDF 6 calls
// heap_caps_get_allocated_size() on it, so it must come from the heap.
static uint8_t*             _dma_buf    = nullptr;

#if SDCARD_WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT
static esp_err_t sdmmc_host_init_noop(void) {
    return ESP_OK;
}

static esp_err_t sdmmc_host_deinit_slot_hosted(int slot) {
    if (slot != CONFIG_ESP_HOSTED_SDIO_SLOT) {
        sdmmc_command_t cmd = {};
        cmd.opcode     = SD_IO_RW_DIRECT;
        cmd.flags      = SCF_CMD_AC | SCF_RSP_R5;
        // Without a timeout this transaction gives up before the response
        // arrives and leaves it pending for ESP-Hosted's next one.
        cmd.timeout_ms = 1000;
        esp_err_t res = sdmmc_host_do_transaction(CONFIG_ESP_HOSTED_SDIO_SLOT, &cmd);
        if (res != ESP_OK) {
            ESP_LOGW(TAG, "Dummy transaction on the ESP-Hosted slot failed (%s)", esp_err_to_name(res));
        }
    }
    return sdmmc_host_deinit_slot(slot);
}
#endif

static esp_err_t power_cycle_sd_card(void) {
    if (_pwr_handle == nullptr) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Power cycling SD card...");

    // Pull the bus lines low so the card sees a clean power-down.
    static gpio_num_t const bus_pins[] = {GPIO_NUM_39, GPIO_NUM_40, GPIO_NUM_41, GPIO_NUM_42, GPIO_NUM_43,
                                          GPIO_NUM_44};

    gpio_config_t gpio_cfg = {};
    for (gpio_num_t pin : bus_pins) gpio_cfg.pin_bit_mask |= BIT64(pin);
    gpio_cfg.mode         = GPIO_MODE_OUTPUT_OD;
    gpio_cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    gpio_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_cfg.intr_type    = GPIO_INTR_DISABLE;
    esp_err_t res = gpio_config(&gpio_cfg);
    if (res != ESP_OK) return res;
    for (gpio_num_t pin : bus_pins) gpio_set_level(pin, 0);

    sd_pwr_ctrl_set_io_voltage(_pwr_handle, 0);
    vTaskDelay(pdMS_TO_TICKS(150));

    gpio_cfg.mode = GPIO_MODE_INPUT;
    res           = gpio_config(&gpio_cfg);
    if (res != ESP_OK) return res;
    sd_pwr_ctrl_set_io_voltage(_pwr_handle, 3300);
    vTaskDelay(pdMS_TO_TICKS(150));

    return ESP_OK;
}

esp_err_t sdcard_mount(void) {
    if (_card) return ESP_OK;

    if (_pwr_handle == nullptr) {
        sd_pwr_ctrl_ldo_config_t ldo_config = {
            .ldo_chan_id = 4,
        };
        esp_err_t res = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &_pwr_handle);
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create the SD LDO power control: %s", esp_err_to_name(res));
            return res;
        }
    }

    esp_err_t res = power_cycle_sd_card();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset the SD card: %s", esp_err_to_name(res));
        return res;
    }

    if (_dma_buf == nullptr) {
        _dma_buf = (uint8_t*)heap_caps_malloc(512 * 4, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (_dma_buf == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate the SD DMA buffer");
            return ESP_ERR_NO_MEM;
        }
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files              = 8;
    mount_config.allocation_unit_size   = 16 * 1024;

    sdmmc_host_t host    = SDMMC_HOST_DEFAULT();
    host.slot            = SDMMC_HOST_SLOT_0;
    host.max_freq_khz    = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = _pwr_handle;
    host.dma_aligned_buffer = _dma_buf;
#if SDCARD_WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT
    // host.init and host.init_p share a union, same for deinit/deinit_p: setting
    // deinit_p keeps slot 0 released without tearing down the shared controller.
    host.init     = &sdmmc_host_init_noop;
    host.deinit_p = &sdmmc_host_deinit_slot_hosted;
#endif

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk                 = GPIO_NUM_43;
    slot_config.cmd                 = GPIO_NUM_44;
    slot_config.d0                  = GPIO_NUM_39;
    slot_config.d1                  = GPIO_NUM_40;
    slot_config.d2                  = GPIO_NUM_41;
    slot_config.d3                  = GPIO_NUM_42;
    slot_config.width               = 4;
    slot_config.flags              |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    res = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &_card);
    if (res != ESP_OK) {
        if (res == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount the SD card filesystem");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the SD card (%s) - is one inserted?", esp_err_to_name(res));
        }
        _card = nullptr;
        return res;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    return ESP_OK;
}

bool sdcard_is_mounted(void) {
    return _card != nullptr;
}
