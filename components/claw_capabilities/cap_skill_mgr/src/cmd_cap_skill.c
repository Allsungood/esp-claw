/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_cap_skill.h"

#include <stdio.h>
#include <stdlib.h>

#include "argtable3/argtable3.h"
#include "cJSON.h"
#include "claw_cap.h"
#include "claw_skill.h"
#include "esp_console.h"

#define CMD_CAP_SKILL_OUTPUT_SIZE 4096

static struct {
    struct arg_lit *list;
    struct arg_lit *catalog;
    struct arg_str *publish;
    struct arg_str *remove;
    struct arg_str *activate;
    struct arg_str *session;
    struct arg_end *end;
} skill_args;

static void free_string_array(char **items, size_t count)
{
    size_t i;

    if (!items) {
        return;
    }

    for (i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

static int call_skill_cap(const char *cap_name, const char *input_json, const char *session_id)
{
    char *output = NULL;
    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_CONSOLE,
        .session_id = session_id,
    };
    esp_err_t err;

    output = calloc(1, CMD_CAP_SKILL_OUTPUT_SIZE);
    if (!output) {
        printf("Out of memory\n");
        return 1;
    }

    err = claw_cap_call(cap_name, input_json, &ctx, output, CMD_CAP_SKILL_OUTPUT_SIZE);
    if (err == ESP_OK) {
        printf("%s\n", output);
        free(output);
        return 0;
    }

    printf("%s\n", output[0] ? output : esp_err_to_name(err));
    free(output);
    return 1;
}

static char *build_single_skill_id_json(const char *skill_id)
{
    cJSON *root = NULL;
    char *rendered = NULL;

    root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "skill_id", skill_id);
    rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return rendered;
}

static int skill_func(int argc, char **argv)
{
    char **active_skill_ids = NULL;
    size_t active_skill_count = 0;
    char *input_json = NULL;
    const char *session_id;
    int rc;
    esp_err_t err;
    int nerrors = arg_parse(argc, argv, (void **)&skill_args);
    int operation_count;
    size_t i;

    if (nerrors != 0) {
        arg_print_errors(stderr, skill_args.end, argv[0]);
        return 1;
    }

    operation_count = skill_args.list->count + skill_args.catalog->count +
                      skill_args.publish->count + skill_args.remove->count +
                      skill_args.activate->count;
    if (operation_count != 1) {
        printf("Exactly one operation must be specified\n");
        return 1;
    }

    session_id = skill_args.session->count ? skill_args.session->sval[0] : "default";

    if (skill_args.catalog->count) {
        return call_skill_cap("list_skill", "{}", session_id);
    }

    if (skill_args.publish->count) {
        input_json = build_single_skill_id_json(skill_args.publish->sval[0]);
        if (!input_json) {
            printf("Out of memory\n");
            return 1;
        }

        rc = call_skill_cap("publish_skill", input_json, session_id);
        free(input_json);
        return rc;
    }

    if (skill_args.remove->count) {
        input_json = build_single_skill_id_json(skill_args.remove->sval[0]);
        if (!input_json) {
            printf("Out of memory\n");
            return 1;
        }

        rc = call_skill_cap("remove_skill", input_json, session_id);
        free(input_json);
        return rc;
    }

    if (skill_args.activate->count) {
        input_json = build_single_skill_id_json(skill_args.activate->sval[0]);
        if (!input_json) {
            printf("Out of memory\n");
            return 1;
        }

        rc = call_skill_cap("activate_skill", input_json, session_id);
        free(input_json);
        return rc;
    }

    err = claw_skill_load_active_skill_ids(session_id, &active_skill_ids, &active_skill_count);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        printf("skill list failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("session=%s\n", session_id);
    if (active_skill_count == 0) {
        printf("(no active skills)\n");
    } else {
        for (i = 0; i < active_skill_count; i++) {
            printf("%s\n", active_skill_ids[i]);
        }
    }

    free_string_array(active_skill_ids, active_skill_count);
    return 0;
}

void register_cap_skill(void)
{
    skill_args.list = arg_lit0("l", "list", "List active skills for one session");
    skill_args.catalog = arg_lit0(NULL, "catalog", "Print the skills catalog JSON");
    skill_args.publish = arg_str0("p", "publish", "<skill_id>", "Validate and publish a runtime skill");
    skill_args.remove = arg_str0("r", "remove", "<skill_id>", "Recursively remove a runtime skill directory");
    skill_args.activate = arg_str0("a", "activate", "<skill_id>", "Activate one skill");
    skill_args.session = arg_str0("s", "session", "<session_id>", "Session id, defaults to 'default'");
    skill_args.end = arg_end(16);

    const esp_console_cmd_t skill_cmd = {
        .command = "skill",
        .help = "Skill operations.\n"
        "Examples:\n"
        " skill --catalog\n"
        " skill --publish weather_v2\n"
        " skill --remove weather_v2\n"
        " skill --list --session default\n"
        " skill --activate weather --session default\n",
        .func = skill_func,
        .argtable = &skill_args,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&skill_cmd));
}
