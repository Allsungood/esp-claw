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
 * Two things about this panel are handled here.
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
 *    This panel's MADCTL mapping is not intuitive once MV is set by swap_xy, and
 *    getting it wrong produces an image that is mirrored or rotated with no way to
 *    tell from the datasheet alone. Rather than hard-coding a guess, the raw MADCTL
 *    byte, X gap and Y gap can be overridden at runtime from
 *
 *        /fatfs/lcd.cfg        e.g.  "e0 0 34"
 *
 *    holding three values in hex or decimal: MADCTL, x_gap, y_gap. The raw byte
 *    covers MX/MY/MV and the BGR colour-order bit, so colour order is tunable too.
 *    Keeping MADCTL as one opaque byte avoids having to reason about how the
 *    esp_lcd mirror/swap helpers combine.
 *
 *    The file cannot be read from the factory hook because the factory runs before
 *    the data partition is mounted, so the override is applied from a wrapper
 *    around the panel's draw_bitmap: the first draw installs the compiled-in
 *    defaults, and the file is picked up as soon as it becomes readable. If
 *    /fatfs/lcd.cfg is absent the compiled-in defaults apply and behaviour is
 *    unchanged.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
/* esp_lcd_panel_t is only fully defined here; the ops/vendor headers expose the
 * handle but not the function-pointer table that the draw wrapper needs. */
#include "esp_lcd_panel_interface.h"
#include "esp_log.h"

static const char *TAG = "MPYTHON_PRO_SETUP_DEVICE";

/*!< Centring offset of the 172-pixel-wide glass in the ST7789 240-pixel frame memory. */
#define MPYTHON_PRO_LCD_GAP_PX   ((240 - 172) / 2)

/*!< MADCTL as configured by board_devices.yaml: swap_xy only, so MV. */
#define MPYTHON_PRO_LCD_MADCTL   (0x20)

/*!< ST7789 command numbers used directly (esp_lcd keeps MADCTL private). */
#define ST7789_CMD_MADCTL        (0x36)

/*!< Runtime override file, on the writable data root. */
#define MPYTHON_PRO_LCD_CFG_PATH "/fatfs/lcd.cfg"

/*!< Re-check the override file every N draws until it has been read once. */
#define MPYTHON_PRO_LCD_CFG_RETRY_DRAWS (120)

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_t *panel;
    esp_err_t (*draw_bitmap)(esp_lcd_panel_t *, int, int, int, int, const void *);
    int draws;
    bool cfg_loaded;
    int madctl;
    int x_gap;
    int y_gap;
} mpython_pro_lcd_t;

static mpython_pro_lcd_t s_lcd;

static void mpython_pro_lcd_apply(int madctl, int x_gap, int y_gap)
{
    const uint8_t madctl_val = (uint8_t)madctl;

    esp_err_t ret = esp_lcd_panel_io_tx_param(s_lcd.io, ST7789_CMD_MADCTL, &madctl_val, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write MADCTL: %s", esp_err_to_name(ret));
        return;
    }
    ret = esp_lcd_panel_set_gap(s_lcd.panel, x_gap, y_gap);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set gap: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGW(TAG, "LCD orientation applied: MADCTL=0x%02X x_gap=%d y_gap=%d", madctl, x_gap, y_gap);
}

/*!< Returns true when the optional override file supplied the values. */
static bool mpython_pro_lcd_load_cfg(int *madctl, int *x_gap, int *y_gap)
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

    *madctl = m;
    *x_gap = x;
    *y_gap = y;
    return true;
}

static esp_err_t mpython_pro_lcd_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                             int x_end, int y_end, const void *color_data)
{
    if (!s_lcd.cfg_loaded && (s_lcd.draws++ % MPYTHON_PRO_LCD_CFG_RETRY_DRAWS) == 0) {
        int madctl = MPYTHON_PRO_LCD_MADCTL;
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

    /* Intercept drawing so the data partition has a chance to mount first. */
    (*ret_panel)->draw_bitmap = mpython_pro_lcd_draw_bitmap;
    if (s_lcd.draw_bitmap != NULL) {
        mpython_pro_lcd_apply(s_lcd.madctl, s_lcd.x_gap, s_lcd.y_gap);
    }

    ESP_LOGI(TAG, "ST7789 ready, defaults MADCTL=0x%02X y_gap=%d, override file %s",
             MPYTHON_PRO_LCD_MADCTL, MPYTHON_PRO_LCD_GAP_PX, MPYTHON_PRO_LCD_CFG_PATH);
    return ESP_OK;
}
