/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board-specific setup for the Labplus mPython Pro (掌控板 3.0).
 *
 * The onboard ST7789 panel has no dedicated reset line and uses the stock esp_lcd
 * ST7789 driver, so the SPI display path only needs the panel factory hook that
 * dev_display_lcd_sub_spi.c resolves at link time.
 *
 * Three things about this panel are handled here.
 *
 * 1. Window offset
 *    The 172-pixel-wide glass is centred inside the ST7789's 240x320 frame memory,
 *    leaving a 34-pixel margin on each side. Without compensating for it the driver
 *    writes into the margin and the far edge of the glass is never addressed, which
 *    shows up as a band of uninitialised pixels. The board manager's display_lcd
 *    config has no gap field, so it is applied here. With swap_xy enabled the
 *    driver works in 320x172 landscape coordinates, so the offset lands on Y.
 *
 * 2. Orientation
 *    This panel's MADCTL mapping is not intuitive once MV is set by swap_xy, and a
 *    wrong value yields an image that is mirrored or rotated with no way to tell
 *    from the datasheet which combination is right. MADCTL is therefore treated as
 *    one opaque byte rather than as esp_lcd's mirror/swap pair, and it can be
 *    changed two ways at runtime, neither of which needs a rebuild:
 *
 *      - button B cycles through the candidate table below. The applied value is
 *        logged, so "press until it looks right" is enough to identify it.
 *      - /fatfs/lcd.cfg ("madctl_hex x_gap y_gap", e.g. "e0 0 34") overrides the
 *        boot default.
 *
 *    The candidates cover the four landscape orientations with MV set, in both RGB
 *    and BGR colour order, since a wrong BGR bit swaps red and blue.
 *
 * 3. Why a task rather than a draw hook
 *    Both the button and the config file are handled by a small polling task. A
 *    draw hook is not usable for this: the agent UI is static between messages, so
 *    draw_bitmap can go minutes without being called and button presses would be
 *    missed entirely.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MPYTHON_PRO_SETUP_DEVICE";

/*!< Centring offset of the 172-pixel-wide glass in the ST7789 240-pixel frame memory. */
#define MPYTHON_PRO_LCD_GAP_PX   ((240 - 172) / 2)

/*!< MADCTL as configured by board_devices.yaml: swap_xy only, so MV. */
#define MPYTHON_PRO_LCD_MADCTL   (0x20)

/*!< ST7789 command used directly: esp_lcd keeps MADCTL private. */
#define ST7789_CMD_MADCTL        (0x36)

/*!< Runtime override file, on the writable data root. */
#define MPYTHON_PRO_LCD_CFG_PATH "/fatfs/lcd.cfg"

/*!< Onboard button B, already configured as a pulled-up input by the board manager. */
#define MPYTHON_PRO_BUTTON_B_GPIO  (46)

#define MPYTHON_PRO_LCD_TASK_POLL_MS      50
#define MPYTHON_PRO_BUTTON_DEBOUNCE_US    300000
#define MPYTHON_PRO_LCD_CFG_RETRY_MS      1000
#define MPYTHON_PRO_LCD_TASK_STACK        3072
#define MPYTHON_PRO_LCD_TASK_PRIORITY     4

/*
 * Landscape orientations with MV set (bit 5), plus the BGR bit (bit 3) variants:
 *   MY = 0x80, MX = 0x40, MV = 0x20, BGR = 0x08
 */
static const uint8_t s_lcd_candidates[] = {
    0x20, 0x60, 0xA0, 0xE0,
    0x28, 0x68, 0xA8, 0xE8,
};

#define MPYTHON_PRO_LCD_CANDIDATE_COUNT \
    (sizeof(s_lcd_candidates) / sizeof(s_lcd_candidates[0]))

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_t *panel;
    uint8_t madctl;
    int x_gap;
    int y_gap;
    int candidate;
    int button_level;
    int64_t last_press_us;
    bool cfg_loaded;
} mpython_pro_lcd_t;

static mpython_pro_lcd_t s_lcd;

static void mpython_pro_lcd_apply(uint8_t madctl, int x_gap, int y_gap)
{
    esp_err_t ret = esp_lcd_panel_io_tx_param(s_lcd.io, ST7789_CMD_MADCTL, &madctl, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write MADCTL: %s", esp_err_to_name(ret));
        return;
    }
    ret = esp_lcd_panel_set_gap(s_lcd.panel, x_gap, y_gap);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set gap: %s", esp_err_to_name(ret));
        return;
    }
    s_lcd.madctl = madctl;
    s_lcd.x_gap = x_gap;
    s_lcd.y_gap = y_gap;
    ESP_LOGW(TAG, "LCD orientation applied: MADCTL=0x%02X x_gap=%d y_gap=%d",
             madctl, x_gap, y_gap);
}

/*!< Returns true when the optional override file supplied the values. */
static bool mpython_pro_lcd_load_cfg(void)
{
    FILE *file = fopen(MPYTHON_PRO_LCD_CFG_PATH, "r");
    if (file == NULL) {
        return false;
    }

    char line[64] = { 0 };
    const char *read = fgets(line, sizeof(line), file);
    fclose(file);
    if (read == NULL) {
        ESP_LOGW(TAG, "%s is empty, keeping defaults", MPYTHON_PRO_LCD_CFG_PATH);
        return false;
    }

    int m = 0;
    int x = 0;
    int y = 0;
    if (sscanf(line, "%i %i %i", &m, &x, &y) != 3) {
        ESP_LOGW(TAG, "%s must hold 'madctl x_gap y_gap', keeping defaults", MPYTHON_PRO_LCD_CFG_PATH);
        return false;
    }
    if (m < 0 || m > 0xFF || x < -120 || x > 120 || y < -120 || y > 120) {
        ESP_LOGW(TAG, "%s has out-of-range values, keeping defaults", MPYTHON_PRO_LCD_CFG_PATH);
        return false;
    }

    ESP_LOGW(TAG, "%s found: MADCTL=0x%02X x_gap=%d y_gap=%d",
             MPYTHON_PRO_LCD_CFG_PATH, m, x, y);
    mpython_pro_lcd_apply((uint8_t)m, x, y);
    return true;
}

static void mpython_pro_lcd_task(void *arg)
{
    (void)arg;
    int64_t next_cfg_try_ms = 0;

    for (;;) {
        if (!s_lcd.cfg_loaded && esp_timer_get_time() / 1000 >= next_cfg_try_ms) {
            next_cfg_try_ms = esp_timer_get_time() / 1000 + MPYTHON_PRO_LCD_CFG_RETRY_MS;
            s_lcd.cfg_loaded = mpython_pro_lcd_load_cfg();
        }

        const int level = gpio_get_level((gpio_num_t)MPYTHON_PRO_BUTTON_B_GPIO);
        if (level != s_lcd.button_level) {
            s_lcd.button_level = level;
            /* Button B is active low: a press pulls the line down. */
            if (level == 0) {
                const int64_t now = esp_timer_get_time();
                if (now - s_lcd.last_press_us >= MPYTHON_PRO_BUTTON_DEBOUNCE_US) {
                    s_lcd.last_press_us = now;
                    s_lcd.candidate = (s_lcd.candidate + 1) % (int)MPYTHON_PRO_LCD_CANDIDATE_COUNT;
                    ESP_LOGW(TAG, "Button B: candidate %d/%d",
                             s_lcd.candidate + 1, (int)MPYTHON_PRO_LCD_CANDIDATE_COUNT);
                    mpython_pro_lcd_apply(s_lcd_candidates[s_lcd.candidate], s_lcd.x_gap, s_lcd.y_gap);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(MPYTHON_PRO_LCD_TASK_POLL_MS));
    }
}

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io != NULL && panel_dev_config != NULL && ret_panel != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid ST7789 panel arguments");

    esp_err_t ret = esp_lcd_new_panel_st7789(io, panel_dev_config, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ST7789 panel: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_set_gap(*ret_panel, 0, MPYTHON_PRO_LCD_GAP_PX);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set ST7789 gap: %s", esp_err_to_name(ret));
        return ret;
    }

    s_lcd.io = io;
    s_lcd.panel = *ret_panel;
    s_lcd.madctl = MPYTHON_PRO_LCD_MADCTL;
    s_lcd.x_gap = 0;
    s_lcd.y_gap = MPYTHON_PRO_LCD_GAP_PX;
    s_lcd.candidate = 0;
    s_lcd.button_level = gpio_get_level((gpio_num_t)MPYTHON_PRO_BUTTON_B_GPIO);
    s_lcd.last_press_us = 0;
    s_lcd.cfg_loaded = false;

    mpython_pro_lcd_apply(s_lcd.madctl, s_lcd.x_gap, s_lcd.y_gap);

    const BaseType_t created = xTaskCreate(mpython_pro_lcd_task, "lcd_orient",
                                           MPYTHON_PRO_LCD_TASK_STACK, NULL,
                                           MPYTHON_PRO_LCD_TASK_PRIORITY, NULL);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "failed to create LCD task");

    ESP_LOGI(TAG, "ST7789 ready, default MADCTL=0x%02X y_gap=%d; press button B to cycle "
             "%d candidates, or provide '%s' with 'madctl x_gap y_gap'",
             MPYTHON_PRO_LCD_MADCTL, MPYTHON_PRO_LCD_GAP_PX,
             (int)MPYTHON_PRO_LCD_CANDIDATE_COUNT, MPYTHON_PRO_LCD_CFG_PATH);
    return ESP_OK;
}
