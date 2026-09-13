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
 *        boot default. The file cannot be read from the factory hook because the
 *        factory runs before the data partition is mounted, so it is picked up from
 *        the draw wrapper once the filesystem is up.
 *
 *    The candidates cover the four landscape orientations with MV set, in both RGB
 *    and BGR colour order, since a wrong BGR bit swaps red and blue.
 *
 * 3. Draw hook
 *    MADCTL is rewritten from a wrapper around the panel's draw_bitmap. The wrapper
 *    is also where the button is polled, which keeps everything in the display task
 *    and avoids a separate polling task.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
/* esp_lcd_panel_t is only fully defined here; the ops/vendor headers expose the
 * handle but not the function-pointer table that the draw wrapper needs. */
#include "esp_lcd_panel_interface.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "MPYTHON_PRO_SETUP_DEVICE";

/*!< Centring offset of the 172-pixel-wide glass in the ST7789 240-pixel frame memory. */
#define MPYTHON_PRO_LCD_GAP_PX   ((240 - 172) / 2)

/*!< MADCTL as configured by board_devices.yaml: swap_xy only, so MV. */
#define MPYTHON_PRO_LCD_MADCTL   (0x20)

/*!< ST7789 command used directly: esp_lcd keeps MADCTL private. */
#define ST7789_CMD_MADCTL        (0x36)

/*!< Runtime override file, on the writable data root. */
#define MPYTHON_PRO_LCD_CFG_PATH "/fatfs/lcd.cfg"

/*!< Re-check the override file every N draws until it has been read once. */
#define MPYTHON_PRO_LCD_CFG_RETRY_DRAWS (120)

/*!< Onboard button B, already configured as a pulled-up input by the board manager. */
#define MPYTHON_PRO_BUTTON_B_GPIO  (46)

/*!< Minimum gap between two accepted button presses, in microseconds. */
#define MPYTHON_PRO_BUTTON_DEBOUNCE_US  (300000)

/*
 * Landscape orientations with MV set (bit 5), plus the BGR bit (bit 3) variants:
 *   MY = 0x80, MX = 0x40, MV = 0x20, BGR = 0x08
 */
static const uint8_t s_lcd_candidates[] = {
    0x20, 0x60, 0xA0, 0xE0,
    0x28, 0x68, 0xA8, 0xE8,
};

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_t *panel;
    esp_err_t (*draw_bitmap)(esp_lcd_panel_t *, int, int, int, int, const void *);
    int draws;
    bool cfg_loaded;
    uint8_t madctl;
    int x_gap;
    int y_gap;
    int candidate;
    int button_level;
    int64_t last_press_us;
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
    ESP_LOGW(TAG, "LCD orientation applied: MADCTL=0x%02X x_gap=%d y_gap=%d",
             madctl, x_gap, y_gap);
}

/*!< Returns true when the optional override file supplied the values. */
static bool mpython_pro_lcd_load_cfg(uint8_t *madctl, int *x_gap, int *y_gap)
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

    *madctl = (uint8_t)m;
    *x_gap = x;
    *y_gap = y;
    return true;
}

/*!< Advance to the next candidate when button B is pressed, and log which one. */
static void mpython_pro_lcd_poll_button(void)
{
    const int level = gpio_get_level((gpio_num_t)MPYTHON_PRO_BUTTON_B_GPIO);
    if (level == s_lcd.button_level) {
        return;
    }
    s_lcd.button_level = level;

    /* Button B is active low: a press pulls the line down. */
    if (level != 0) {
        return;
    }

    const int64_t now = esp_timer_get_time();
    if (now - s_lcd.last_press_us < MPYTHON_PRO_BUTTON_DEBOUNCE_US) {
        return;
    }
    s_lcd.last_press_us = now;

    s_lcd.candidate = (s_lcd.candidate + 1) % (int)(sizeof(s_lcd_candidates) / sizeof(s_lcd_candidates[0]));
    s_lcd.madctl = s_lcd_candidates[s_lcd.candidate];
    mpython_pro_lcd_apply(s_lcd.madctl, s_lcd.x_gap, s_lcd.y_gap);
    ESP_LOGW(TAG, "Button B: candidate %d/%d selected",
             s_lcd.candidate + 1, (int)(sizeof(s_lcd_candidates) / sizeof(s_lcd_candidates[0])));
}

static esp_err_t mpython_pro_lcd_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                             int x_end, int y_end, const void *color_data)
{
    if (!s_lcd.cfg_loaded && (s_lcd.draws++ % MPYTHON_PRO_LCD_CFG_RETRY_DRAWS) == 0) {
        uint8_t madctl = MPYTHON_PRO_LCD_MADCTL;
        int x_gap = 0;
        int y_gap = MPYTHON_PRO_LCD_GAP_PX;
        if (mpython_pro_lcd_load_cfg(&madctl, &x_gap, &y_gap)) {
            if (madctl != s_lcd.madctl || x_gap != s_lcd.x_gap || y_gap != s_lcd.y_gap) {
                s_lcd.madctl = madctl;
                s_lcd.x_gap = x_gap;
                s_lcd.y_gap = y_gap;
                mpython_pro_lcd_apply(madctl, x_gap, y_gap);
            }
            s_lcd.cfg_loaded = true;
        }
    }

    mpython_pro_lcd_poll_button();

    return s_lcd.draw_bitmap(panel, x_start, y_start, x_end, y_end, color_data);
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
    s_lcd.draw_bitmap = (*ret_panel)->draw_bitmap;
    s_lcd.madctl = MPYTHON_PRO_LCD_MADCTL;
    s_lcd.x_gap = 0;
    s_lcd.y_gap = MPYTHON_PRO_LCD_GAP_PX;
    s_lcd.candidate = 0;
    s_lcd.button_level = gpio_get_level((gpio_num_t)MPYTHON_PRO_BUTTON_B_GPIO);
    s_lcd.last_press_us = 0;

    /* Intercept drawing so the data partition has a chance to mount first, and so
     * button B can cycle the orientation from the display task. */
    (*ret_panel)->draw_bitmap = mpython_pro_lcd_draw_bitmap;
    if (s_lcd.draw_bitmap != NULL) {
        mpython_pro_lcd_apply(s_lcd.madctl, s_lcd.x_gap, s_lcd.y_gap);
    }

    ESP_LOGI(TAG, "ST7789 ready, default MADCTL=0x%02X y_gap=%d; press button B to cycle, "
             "or write '%s' with 'madctl x_gap y_gap'",
             MPYTHON_PRO_LCD_MADCTL, MPYTHON_PRO_LCD_GAP_PX, MPYTHON_PRO_LCD_CFG_PATH);
    return ESP_OK;
}
