/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "claw_launcher.h"
#include "claw_skill.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define CLAW_LAUNCHER_FILE_MAX_BYTES 4096
#define CLAW_LAUNCHER_SCHEMA_VERSION 1
#define CLAW_LAUNCHER_MAX_LISTENERS 4
#define CLAW_LAUNCHER_LOCK_TIMEOUT_MS 5000
#define CLAW_LAUNCHER_MAX_PATH_LEN 128
#define CLAW_LAUNCHER_FILENAME "launcher.json"
#define CLAW_LAUNCHER_TEMP_SUFFIX ".tmp"
#define CLAW_LAUNCHER_BACKUP_SUFFIX ".bak"

static const char *TAG = "claw_launcher";

typedef struct {
    char *skill_id;
    char *display_name;
    char *entry;
    char *icon;
    char *args_json;
    int order;
    bool visible;
} claw_launcher_owned_entry_t;

typedef struct {
    claw_launcher_changed_cb_t callback;
    void *user_ctx;
} claw_launcher_listener_t;

typedef struct {
    bool initialized;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t operation_lock;
    claw_launcher_owned_entry_t *entries;
    size_t entry_count;
    claw_launcher_listener_t listeners[CLAW_LAUNCHER_MAX_LISTENERS];
} claw_launcher_state_t;

typedef struct {
    claw_launcher_owned_entry_t *entries;
    size_t entry_count;
    size_t skill_index;
} claw_launcher_snapshot_t;

static claw_launcher_state_t s_launcher;

static void free_entry(claw_launcher_owned_entry_t *entry)
{
    if (!entry) {
        return;
    }
    free(entry->skill_id);
    free(entry->display_name);
    free(entry->entry);
    free(entry->icon);
    free(entry->args_json);
    memset(entry, 0, sizeof(*entry));
}

static void free_entries(claw_launcher_owned_entry_t *entries, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free_entry(&entries[i]);
    }
    free(entries);
}

static bool has_suffix(const char *value, const char *suffix)
{
    if (!value || !suffix) {
        return false;
    }
    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);
    return value_len >= suffix_len && strcmp(value + value_len - suffix_len, suffix) == 0;
}

static bool relative_path_is_valid(const char *path)
{
    return path && path[0] && path[0] != '/' && !strstr(path, "..") && !strchr(path, '\\');
}

static char *path_join_dup(const char *dir, const char *name)
{
    int len = snprintf(NULL, 0, "%s/%s", dir, name);
    if (len < 0) {
        return NULL;
    }
    char *path = malloc((size_t)len + 1);
    if (path) {
        snprintf(path, (size_t)len + 1, "%s/%s", dir, name);
    }
    return path;
}

static esp_err_t read_file_dup(const char *path, char **out_text)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    long size = ftell(file);
    if (size < 0 || size > CLAW_LAUNCHER_FILE_MAX_BYTES || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return size > CLAW_LAUNCHER_FILE_MAX_BYTES ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
    }
    char *text = calloc(1, (size_t)size + 1);
    if (!text) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    size_t read_bytes = fread(text, 1, (size_t)size, file);
    bool failed = ferror(file) != 0;
    fclose(file);
    if (failed || read_bytes != (size_t)size) {
        free(text);
        return ESP_FAIL;
    }
    *out_text = text;
    return ESP_OK;
}

static esp_err_t write_file_text(const char *path, const char *text)
{
    if (!path || !text) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(path, "wb");
    if (!file) {
        return ESP_FAIL;
    }
    bool failed = fputs(text, file) < 0 || fflush(file) != 0 || fsync(fileno(file)) != 0;
    if (fclose(file) != 0) {
        failed = true;
    }
    return failed ? ESP_FAIL : ESP_OK;
}

static esp_err_t remove_file_if_exists(const char *path)
{
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (remove(path) == 0 || errno == ENOENT) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t build_transaction_paths(const char *path, char *temp_path, size_t temp_path_size, char *backup_path, size_t backup_path_size)
{
    if (!path || !temp_path || !backup_path || snprintf(temp_path, temp_path_size, "%s%s", path, CLAW_LAUNCHER_TEMP_SUFFIX) >= (int)temp_path_size ||
            snprintf(backup_path, backup_path_size, "%s%s", path, CLAW_LAUNCHER_BACKUP_SUFFIX) >= (int)backup_path_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* Keep the previous definition until the registry accepts the replacement. */
static esp_err_t begin_file_update(const char *path, const char *replacement, bool *out_had_previous)
{
    char temp_path[CLAW_LAUNCHER_MAX_PATH_LEN];
    char backup_path[CLAW_LAUNCHER_MAX_PATH_LEN];
    struct stat st = {0};
    bool target_exists = false;
    esp_err_t err;

    if (!path || !out_had_previous) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_had_previous = false;
    err = build_transaction_paths(path, temp_path, sizeof(temp_path), backup_path, sizeof(backup_path));
    if (err != ESP_OK) {
        return err;
    }
    err = remove_file_if_exists(temp_path);
    if (err != ESP_OK) {
        return err;
    }
    if (stat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            return ESP_ERR_INVALID_STATE;
        }
        target_exists = true;
    } else if (errno != ENOENT) {
        return ESP_FAIL;
    }

    /* Recover an interrupted update before starting a new one. */
    if (stat(backup_path, &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (target_exists) {
            err = remove_file_if_exists(backup_path);
            if (err != ESP_OK) {
                return err;
            }
        } else if (rename(backup_path, path) != 0) {
            return ESP_FAIL;
        } else {
            target_exists = true;
        }
    } else if (errno != ENOENT) {
        return ESP_FAIL;
    }

    if (replacement) {
        err = write_file_text(temp_path, replacement);
        if (err != ESP_OK) {
            (void)remove_file_if_exists(temp_path);
            return err;
        }
    }
    if (target_exists && rename(path, backup_path) != 0) {
        (void)remove_file_if_exists(temp_path);
        return ESP_FAIL;
    }
    if (replacement && rename(temp_path, path) != 0) {
        if (target_exists) {
            (void)rename(backup_path, path);
        }
        (void)remove_file_if_exists(temp_path);
        return ESP_FAIL;
    }
    *out_had_previous = target_exists;
    return ESP_OK;
}

static esp_err_t finish_file_update(const char *path, bool had_previous, bool commit)
{
    char temp_path[CLAW_LAUNCHER_MAX_PATH_LEN];
    char backup_path[CLAW_LAUNCHER_MAX_PATH_LEN];
    esp_err_t err = build_transaction_paths(path, temp_path, sizeof(temp_path), backup_path, sizeof(backup_path));
    if (err != ESP_OK) {
        return err;
    }
    (void)remove_file_if_exists(temp_path);
    if (commit) {
        return had_previous ? remove_file_if_exists(backup_path) : ESP_OK;
    }
    err = remove_file_if_exists(path);
    if (err != ESP_OK) {
        return err;
    }
    if (had_previous && rename(backup_path, path) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool key_is_allowed(const char *key)
{
    static const char *const keys[] = {"schema_version", "entry", "icon", "display_name", "args", "order", "visible"};
    for (size_t i = 0; key && i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strcmp(key, keys[i]) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t parse_entry(const claw_skill_catalog_entry_t *skill, size_t default_order, claw_launcher_owned_entry_t *out)
{
    char *launcher_path = path_join_dup(skill->skill_dir, CLAW_LAUNCHER_FILENAME);
    char *text = NULL;
    cJSON *root = NULL;
    esp_err_t err;

    if (!launcher_path) {
        return ESP_ERR_NO_MEM;
    }
    err = read_file_dup(launcher_path, &text);
    if (err != ESP_OK) {
        free(launcher_path);
        return err;
    }
    root = cJSON_ParseWithOpts(text, NULL, true);
    free(text);
    if (!cJSON_IsObject(root)) {
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    cJSON *field = NULL;
    cJSON_ArrayForEach(field, root) {
        if (!key_is_allowed(field->string)) {
            err = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
    }

    cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    cJSON *entry = cJSON_GetObjectItemCaseSensitive(root, "entry");
    cJSON *icon = cJSON_GetObjectItemCaseSensitive(root, "icon");
    cJSON *display_name = cJSON_GetObjectItemCaseSensitive(root, "display_name");
    cJSON *args = cJSON_GetObjectItemCaseSensitive(root, "args");
    cJSON *order = cJSON_GetObjectItemCaseSensitive(root, "order");
    cJSON *visible = cJSON_GetObjectItemCaseSensitive(root, "visible");
    bool has_icon = icon && !cJSON_IsNull(icon);
    bool has_args = args && !cJSON_IsNull(args);
    if (!cJSON_IsNumber(schema) || schema->valuedouble != (double)schema->valueint || schema->valueint != CLAW_LAUNCHER_SCHEMA_VERSION ||
            !cJSON_IsString(entry) || !relative_path_is_valid(entry->valuestring) || !has_suffix(entry->valuestring, ".lua") ||
            (has_icon && (!cJSON_IsString(icon) || !relative_path_is_valid(icon->valuestring) || (!has_suffix(icon->valuestring, ".jpg") && !has_suffix(icon->valuestring, ".jpeg")))) ||
            (display_name && (!cJSON_IsString(display_name) || !display_name->valuestring || !display_name->valuestring[0])) ||
            (has_args && !cJSON_IsObject(args)) || (order && (!cJSON_IsNumber(order) || order->valuedouble != (double)order->valueint)) ||
            (visible && !cJSON_IsBool(visible))) {
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    out->skill_id = strdup(skill->id);
    out->display_name = strdup(display_name ? display_name->valuestring : skill->id);
    out->entry = path_join_dup(skill->skill_dir, entry->valuestring);
    out->icon = has_icon ? path_join_dup(skill->skill_dir, icon->valuestring) : NULL;
    out->args_json = has_args ? cJSON_PrintUnformatted(args) : NULL;
    out->order = order ? order->valueint : (int)default_order;
    out->visible = visible ? cJSON_IsTrue(visible) : true;
    if (!out->skill_id || !out->display_name || !out->entry || (has_icon && !out->icon) || (has_args && !out->args_json)) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    struct stat st = {0};
    if (stat(out->entry, &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGW(TAG, "launcher entry missing: skill=%s path=%s", skill->id, out->entry);
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    if (out->icon && (stat(out->icon, &st) != 0 || !S_ISREG(st.st_mode))) {
        ESP_LOGW(TAG, "launcher icon missing, using default: skill=%s path=%s", skill->id, out->icon);
        free(out->icon);
        out->icon = NULL;
    }
    err = ESP_OK;

cleanup:
    if (err != ESP_OK) {
        free_entry(out);
    }
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "invalid launcher definition: skill=%s path=%s err=%s", skill->id, launcher_path, esp_err_to_name(err));
    }
    cJSON_Delete(root);
    free(launcher_path);
    return err;
}

static esp_err_t collect_skill(const claw_skill_catalog_entry_t *skill, void *user_ctx)
{
    claw_launcher_snapshot_t *snapshot = user_ctx;
    claw_launcher_owned_entry_t entry = {0};
    esp_err_t err = parse_entry(skill, snapshot->skill_index++, &entry);

    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE || err == ESP_FAIL) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    claw_launcher_owned_entry_t *grown = realloc(snapshot->entries, sizeof(*grown) * (snapshot->entry_count + 1));
    if (!grown) {
        free_entry(&entry);
        return ESP_ERR_NO_MEM;
    }
    snapshot->entries = grown;
    snapshot->entries[snapshot->entry_count++] = entry;
    return ESP_OK;
}

typedef struct {
    const char *skill_id;
    char *skill_dir;
    size_t skill_dir_size;
    esp_err_t result;
} runtime_skill_lookup_t;

static esp_err_t find_runtime_skill(const claw_skill_catalog_entry_t *skill, void *user_ctx)
{
    runtime_skill_lookup_t *lookup = user_ctx;
    if (strcmp(skill->id, lookup->skill_id) != 0) {
        return ESP_OK;
    }
    if (skill->manage_mode != CLAW_SKILL_MANAGE_MODE_RUNTIME || !skill->skill_dir) {
        lookup->result = ESP_ERR_INVALID_STATE;
    } else if (snprintf(lookup->skill_dir, lookup->skill_dir_size, "%s", skill->skill_dir) >= (int)lookup->skill_dir_size) {
        lookup->result = ESP_ERR_INVALID_SIZE;
    } else {
        lookup->result = ESP_OK;
    }
    return ESP_OK;
}

static esp_err_t resolve_runtime_skill(const char *skill_id, char *skill_dir, size_t skill_dir_size)
{
    if (!skill_id || !skill_id[0] || strchr(skill_id, '/') || strchr(skill_id, '\\') || strstr(skill_id, "..") || !skill_dir || !skill_dir_size) {
        return ESP_ERR_INVALID_ARG;
    }
    runtime_skill_lookup_t lookup = {
        .skill_id = skill_id,
        .skill_dir = skill_dir,
        .skill_dir_size = skill_dir_size,
        .result = ESP_ERR_NOT_FOUND,
    };
    esp_err_t err = claw_skill_foreach_catalog_entry(find_runtime_skill, &lookup);
    return err == ESP_OK ? lookup.result : err;
}

static bool file_exists(const char *path)
{
    struct stat st = {0};
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static esp_err_t build_definition_json(const char *skill_id, const char *skill_dir, const claw_launcher_definition_t *definition, char **out_text)
{
    char payload_path[CLAW_LAUNCHER_MAX_PATH_LEN];
    cJSON *launcher = NULL;
    cJSON *args = NULL;
    bool use_icon = definition->icon != NULL;
    esp_err_t err = ESP_OK;

    if (!out_text) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;
    if (!definition->entry || !relative_path_is_valid(definition->entry) || !has_suffix(definition->entry, ".lua") ||
            (definition->display_name && !definition->display_name[0]) ||
            (definition->icon && (!relative_path_is_valid(definition->icon) || (!has_suffix(definition->icon, ".jpg") && !has_suffix(definition->icon, ".jpeg"))))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(payload_path, sizeof(payload_path), "%s/%s", skill_dir, definition->entry) >= (int)sizeof(payload_path) || !file_exists(payload_path)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (definition->icon && (snprintf(payload_path, sizeof(payload_path), "%s/%s", skill_dir, definition->icon) >= (int)sizeof(payload_path) || !file_exists(payload_path))) {
        ESP_LOGW(TAG, "launcher icon unavailable, using default: skill=%s icon=%s", skill_id, definition->icon);
        use_icon = false;
    }
    if (definition->args_json) {
        args = cJSON_ParseWithOpts(definition->args_json, NULL, true);
        if (!cJSON_IsObject(args)) {
            cJSON_Delete(args);
            return ESP_ERR_INVALID_ARG;
        }
    }

    launcher = cJSON_CreateObject();
    if (!launcher || !cJSON_AddNumberToObject(launcher, "schema_version", CLAW_LAUNCHER_SCHEMA_VERSION) ||
            !cJSON_AddStringToObject(launcher, "entry", definition->entry) ||
            (definition->display_name && !cJSON_AddStringToObject(launcher, "display_name", definition->display_name)) ||
            (use_icon && !cJSON_AddStringToObject(launcher, "icon", definition->icon))) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    if (args && !cJSON_AddItemToObject(launcher, "args", args)) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    if (args) {
        args = NULL;
    }
    if ((definition->has_order && !cJSON_AddNumberToObject(launcher, "order", definition->order)) ||
            (definition->has_visible && !cJSON_AddBoolToObject(launcher, "visible", definition->visible))) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    *out_text = cJSON_PrintUnformatted(launcher);
    if (!*out_text) {
        err = ESP_ERR_NO_MEM;
    } else if (strlen(*out_text) > CLAW_LAUNCHER_FILE_MAX_BYTES) {
        free(*out_text);
        *out_text = NULL;
        err = ESP_ERR_INVALID_SIZE;
    }

cleanup:
    cJSON_Delete(args);
    cJSON_Delete(launcher);
    return err;
}

static void fill_view(const claw_launcher_owned_entry_t *entry, claw_launcher_entry_t *out)
{
    *out = (claw_launcher_entry_t) {
        .skill_id = entry->skill_id,
        .display_name = entry->display_name,
        .entry = entry->entry,
        .icon = entry->icon,
        .args_json = entry->args_json,
        .order = entry->order,
        .visible = entry->visible,
    };
}

static void notify_changed(void)
{
    claw_launcher_listener_t listeners[CLAW_LAUNCHER_MAX_LISTENERS] = {0};
    if (xSemaphoreTake(s_launcher.lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    memcpy(listeners, s_launcher.listeners, sizeof(listeners));
    xSemaphoreGive(s_launcher.lock);
    for (size_t i = 0; i < CLAW_LAUNCHER_MAX_LISTENERS; i++) {
        if (listeners[i].callback) {
            listeners[i].callback(listeners[i].user_ctx);
        }
    }
}

static void skill_registry_changed(void *user_ctx)
{
    (void)user_ctx;
    esp_err_t err = claw_launcher_reload();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reload after skill change failed: %s", esp_err_to_name(err));
    }
}

esp_err_t claw_launcher_init(void)
{
    if (s_launcher.initialized) {
        return ESP_OK;
    }
    s_launcher.lock = xSemaphoreCreateMutex();
    s_launcher.operation_lock = xSemaphoreCreateMutex();
    if (!s_launcher.lock || !s_launcher.operation_lock) {
        if (s_launcher.lock) {
            vSemaphoreDelete(s_launcher.lock);
        }
        if (s_launcher.operation_lock) {
            vSemaphoreDelete(s_launcher.operation_lock);
        }
        memset(&s_launcher, 0, sizeof(s_launcher));
        return ESP_ERR_NO_MEM;
    }
    s_launcher.initialized = true;
    esp_err_t err = claw_skill_register_registry_changed_cb(skill_registry_changed, NULL);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_launcher.lock);
        vSemaphoreDelete(s_launcher.operation_lock);
        memset(&s_launcher, 0, sizeof(s_launcher));
        return err;
    }
    return ESP_OK;
}

static esp_err_t reload_registry(void)
{
    claw_launcher_snapshot_t snapshot = {0};
    esp_err_t err = claw_skill_foreach_catalog_entry(collect_skill, &snapshot);
    if (err != ESP_OK) {
        free_entries(snapshot.entries, snapshot.entry_count);
        return err;
    }
    if (xSemaphoreTake(s_launcher.lock, pdMS_TO_TICKS(CLAW_LAUNCHER_LOCK_TIMEOUT_MS)) != pdTRUE) {
        free_entries(snapshot.entries, snapshot.entry_count);
        return ESP_ERR_TIMEOUT;
    }
    claw_launcher_owned_entry_t *old_entries = s_launcher.entries;
    size_t old_count = s_launcher.entry_count;
    s_launcher.entries = snapshot.entries;
    s_launcher.entry_count = snapshot.entry_count;
    ESP_LOGI(TAG, "Reloaded registry with %u launcher(s)", (unsigned)s_launcher.entry_count);
    xSemaphoreGive(s_launcher.lock);
    free_entries(old_entries, old_count);
    return ESP_OK;
}

esp_err_t claw_launcher_reload(void)
{
    if (!s_launcher.initialized || !s_launcher.lock || !s_launcher.operation_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_launcher.operation_lock, pdMS_TO_TICKS(CLAW_LAUNCHER_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = reload_registry();
    xSemaphoreGive(s_launcher.operation_lock);
    if (err == ESP_OK) {
        notify_changed();
    }
    return err;
}

esp_err_t claw_launcher_register_changed_cb(claw_launcher_changed_cb_t callback, void *user_ctx)
{
    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_launcher.initialized || !s_launcher.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_launcher.lock, pdMS_TO_TICKS(CLAW_LAUNCHER_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    size_t free_index = CLAW_LAUNCHER_MAX_LISTENERS;
    for (size_t i = 0; i < CLAW_LAUNCHER_MAX_LISTENERS; i++) {
        if (s_launcher.listeners[i].callback == callback && s_launcher.listeners[i].user_ctx == user_ctx) {
            xSemaphoreGive(s_launcher.lock);
            return ESP_OK;
        }
        if (!s_launcher.listeners[i].callback && free_index == CLAW_LAUNCHER_MAX_LISTENERS) {
            free_index = i;
        }
    }
    if (free_index == CLAW_LAUNCHER_MAX_LISTENERS) {
        xSemaphoreGive(s_launcher.lock);
        return ESP_ERR_NO_MEM;
    }
    s_launcher.listeners[free_index] = (claw_launcher_listener_t) {.callback = callback, .user_ctx = user_ctx};
    xSemaphoreGive(s_launcher.lock);
    return ESP_OK;
}

esp_err_t claw_launcher_foreach_entry(claw_launcher_catalog_cb_t callback, void *user_ctx)
{
    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_launcher.initialized || !s_launcher.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_launcher.lock, pdMS_TO_TICKS(CLAW_LAUNCHER_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    for (size_t i = 0; i < s_launcher.entry_count; i++) {
        claw_launcher_entry_t view;
        fill_view(&s_launcher.entries[i], &view);
        esp_err_t err = callback(&view, user_ctx);
        if (err != ESP_OK) {
            xSemaphoreGive(s_launcher.lock);
            return err;
        }
    }
    xSemaphoreGive(s_launcher.lock);
    return ESP_OK;
}

static bool contains_entry_locked(const char *skill_id)
{
    for (size_t i = 0; i < s_launcher.entry_count; i++) {
        if (strcmp(s_launcher.entries[i].skill_id, skill_id) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t contains_entry(const char *skill_id, bool *out_found)
{
    if (xSemaphoreTake(s_launcher.lock, pdMS_TO_TICKS(CLAW_LAUNCHER_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out_found = contains_entry_locked(skill_id);
    xSemaphoreGive(s_launcher.lock);
    return ESP_OK;
}

static esp_err_t rollback_definition(const char *path, bool had_previous, bool reload)
{
    esp_err_t err = finish_file_update(path, had_previous, false);
    if (err == ESP_OK && reload) {
        err = reload_registry();
    }
    return err;
}

esp_err_t claw_launcher_set(const char *skill_id, const claw_launcher_definition_t *definition)
{
    char skill_dir[CLAW_LAUNCHER_MAX_PATH_LEN];
    char launcher_path[CLAW_LAUNCHER_MAX_PATH_LEN];
    char *launcher_text = NULL;
    bool had_previous = false;
    bool registry_changed = false;
    bool entry_found = false;
    esp_err_t err;

    if (!skill_id || !definition) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_launcher.initialized || !s_launcher.operation_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_launcher.operation_lock, pdMS_TO_TICKS(CLAW_LAUNCHER_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    err = resolve_runtime_skill(skill_id, skill_dir, sizeof(skill_dir));
    if (err != ESP_OK || snprintf(launcher_path, sizeof(launcher_path), "%s/%s", skill_dir, CLAW_LAUNCHER_FILENAME) >= (int)sizeof(launcher_path)) {
        err = err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
        goto cleanup;
    }
    err = build_definition_json(skill_id, skill_dir, definition, &launcher_text);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = begin_file_update(launcher_path, launcher_text, &had_previous);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to update launcher definition: skill=%s err=%s", skill_id, esp_err_to_name(err));
        goto cleanup;
    }
    err = reload_registry();
    if (err == ESP_OK) {
        registry_changed = true;
        err = contains_entry(skill_id, &entry_found);
        if (err == ESP_OK && !entry_found) {
            err = ESP_ERR_NOT_FOUND;
        }
    }
    if (err != ESP_OK) {
        esp_err_t rollback_err = rollback_definition(launcher_path, had_previous, registry_changed);
        ESP_LOGE(TAG, "launcher update rejected: skill=%s err=%s rollback=%s", skill_id, esp_err_to_name(err), esp_err_to_name(rollback_err));
        goto cleanup;
    }
    err = finish_file_update(launcher_path, had_previous, true);
    if (err != ESP_OK) {
        esp_err_t rollback_err = rollback_definition(launcher_path, had_previous, true);
        ESP_LOGE(TAG, "failed to commit launcher update: skill=%s err=%s rollback=%s", skill_id, esp_err_to_name(err), esp_err_to_name(rollback_err));
    }

cleanup:
    free(launcher_text);
    xSemaphoreGive(s_launcher.operation_lock);
    if (registry_changed) {
        notify_changed();
    }
    return err;
}

esp_err_t claw_launcher_remove(const char *skill_id)
{
    char skill_dir[CLAW_LAUNCHER_MAX_PATH_LEN];
    char launcher_path[CLAW_LAUNCHER_MAX_PATH_LEN];
    bool had_previous = false;
    bool registry_changed = false;
    bool entry_found = false;
    esp_err_t err;

    if (!skill_id || !skill_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_launcher.initialized || !s_launcher.operation_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_launcher.operation_lock, pdMS_TO_TICKS(CLAW_LAUNCHER_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    err = resolve_runtime_skill(skill_id, skill_dir, sizeof(skill_dir));
    if (err != ESP_OK || snprintf(launcher_path, sizeof(launcher_path), "%s/%s", skill_dir, CLAW_LAUNCHER_FILENAME) >= (int)sizeof(launcher_path)) {
        err = err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
        goto cleanup;
    }
    err = begin_file_update(launcher_path, NULL, &had_previous);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to remove launcher definition: skill=%s err=%s", skill_id, esp_err_to_name(err));
        goto cleanup;
    }
    err = reload_registry();
    if (err == ESP_OK) {
        registry_changed = true;
        err = contains_entry(skill_id, &entry_found);
        if (err == ESP_OK && entry_found) {
            err = ESP_ERR_INVALID_STATE;
        }
    }
    if (err != ESP_OK) {
        esp_err_t rollback_err = rollback_definition(launcher_path, had_previous, registry_changed);
        ESP_LOGE(TAG, "launcher removal rejected: skill=%s err=%s rollback=%s", skill_id, esp_err_to_name(err), esp_err_to_name(rollback_err));
        goto cleanup;
    }
    err = finish_file_update(launcher_path, had_previous, true);
    if (err != ESP_OK) {
        esp_err_t rollback_err = rollback_definition(launcher_path, had_previous, true);
        ESP_LOGE(TAG, "failed to commit launcher removal: skill=%s err=%s rollback=%s", skill_id, esp_err_to_name(err), esp_err_to_name(rollback_err));
    }

cleanup:
    xSemaphoreGive(s_launcher.operation_lock);
    if (registry_changed) {
        notify_changed();
    }
    return err;
}
