/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *skill_id;
    const char *display_name;
    const char *entry;
    const char *icon;
    const char *args_json;
    int order;
    bool visible;
} claw_launcher_entry_t;

typedef struct {
    const char *display_name;
    const char *entry;
    const char *icon;
    const char *args_json;
    int order;
    bool has_order;
    bool visible;
    bool has_visible;
} claw_launcher_definition_t;

/* Entry strings are borrowed only for the duration of the catalog callback. */
/* Definition strings are borrowed only for the duration of claw_launcher_set(). */

typedef esp_err_t (*claw_launcher_catalog_cb_t)(const claw_launcher_entry_t *entry, void *user_ctx);
typedef void (*claw_launcher_changed_cb_t)(void *user_ctx);

esp_err_t claw_launcher_init(void);
esp_err_t claw_launcher_reload(void);
esp_err_t claw_launcher_register_changed_cb(claw_launcher_changed_cb_t callback, void *user_ctx);
esp_err_t claw_launcher_foreach_entry(claw_launcher_catalog_cb_t callback, void *user_ctx);
esp_err_t claw_launcher_set(const char *skill_id, const claw_launcher_definition_t *definition);
esp_err_t claw_launcher_remove(const char *skill_id);

#ifdef __cplusplus
}
#endif
