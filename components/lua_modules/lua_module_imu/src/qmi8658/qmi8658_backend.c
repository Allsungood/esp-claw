/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * QMI8658C backend for lua_module_imu, used by the Labplus mPython Pro
 * (掌控板 3.0) where the chip sits on the shared I2C bus at address 0x6B.
 *
 * Unlike the BMI270 and ICM42670 backends this one is self-contained: it talks to
 * the chip through plain register reads and writes, so it pulls in no vendor
 * component. The initialisation sequence and the register semantics below were
 * verified against real hardware; see qmi8658_regs.h for the values.
 */

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "qmi8658_regs.h"

#include "lua_module_imu_backend.h"

static const char *TAG = "lua_module_imu.qmi8658";

static esp_err_t qmi8658_read(lua_imu_backend_ctx_t *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    if (ctx->i2c_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_bus_read_bytes(ctx->i2c_dev_handle, reg, len, data);
}

static esp_err_t qmi8658_write(lua_imu_backend_ctx_t *ctx, uint8_t reg, uint8_t value)
{
    if (ctx->i2c_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_bus_write_bytes(ctx->i2c_dev_handle, reg, 1, &value);
}

static esp_err_t qmi8658_backend_probe(lua_imu_backend_ctx_t *ctx, uint8_t i2c_addr)
{
    esp_err_t err = lua_imu_ctx_select_addr(ctx, i2c_addr);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t who_am_i = 0;
    err = qmi8658_read(ctx, QMI8658_REG_WHO_AM_I, &who_am_i, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read WHO_AM_I at 0x%02X: %s", i2c_addr, esp_err_to_name(err));
        return err;
    }
    if (who_am_i != QMI8658_WHO_AM_I_VALUE) {
        ESP_LOGE(TAG, "No QMI8658 at 0x%02X: WHO_AM_I is 0x%02X, expected 0x%02X",
                 i2c_addr, who_am_i, QMI8658_WHO_AM_I_VALUE);
        return ESP_ERR_NOT_FOUND;
    }

    /* Auto-increment for burst reads, sensors still off. */
    ESP_RETURN_ON_ERROR(qmi8658_write(ctx, QMI8658_REG_CTRL1, QMI8658_CTRL1_ADDR_AI),
                        TAG, "failed to configure CTRL1");
    ESP_RETURN_ON_ERROR(qmi8658_write(ctx, QMI8658_REG_CTRL2, QMI8658_CTRL2_ACCEL_8G),
                        TAG, "failed to configure accelerometer");
    ESP_RETURN_ON_ERROR(qmi8658_write(ctx, QMI8658_REG_CTRL3, QMI8658_CTRL3_GYRO_64DPS),
                        TAG, "failed to configure gyroscope");
    ESP_RETURN_ON_ERROR(qmi8658_write(ctx, QMI8658_REG_CTRL7, QMI8658_CTRL7_ENABLE_BOTH),
                        TAG, "failed to enable sensors");

    vTaskDelay(pdMS_TO_TICKS(QMI8658_STARTUP_DELAY_MS));

    ESP_LOGI(TAG, "QMI8658 ready at 0x%02X (+/-8 g, +/-64 dps)", i2c_addr);
    return ESP_OK;
}

static esp_err_t qmi8658_backend_read_sample(lua_imu_backend_ctx_t *ctx, lua_imu_sample_t *out)
{
    /* Accel and gyro outputs are contiguous from 0x35, six bytes each. */
    uint8_t raw[12] = { 0 };
    esp_err_t err = qmi8658_read(ctx, QMI8658_REG_ACCEL_XYZ, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    out->accel.x = (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    out->accel.y = (int16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8));
    out->accel.z = (int16_t)((uint16_t)raw[4] | ((uint16_t)raw[5] << 8));
    out->gyro.x = (int16_t)((uint16_t)raw[6] | ((uint16_t)raw[7] << 8));
    out->gyro.y = (int16_t)((uint16_t)raw[8] | ((uint16_t)raw[9] << 8));
    out->gyro.z = (int16_t)((uint16_t)raw[10] | ((uint16_t)raw[11] << 8));

    uint8_t status = 0;
    if (qmi8658_read(ctx, QMI8658_REG_STATUSINT, &status, 1) == ESP_OK) {
        out->status = status;
    } else {
        out->status = 0;
    }
    out->sens_time = esp_timer_get_time();
    return ESP_OK;
}

static esp_err_t qmi8658_backend_read_temperature(lua_imu_backend_ctx_t *ctx, int32_t *out)
{
    uint8_t raw[2] = { 0 };
    esp_err_t err = qmi8658_read(ctx, QMI8658_REG_TEMP_L, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }
    /* Chip-defined units: the high byte is whole degrees Celsius. */
    *out = (int32_t)(int8_t)raw[1];
    return ESP_OK;
}

static esp_err_t qmi8658_backend_read_int_status(lua_imu_backend_ctx_t *ctx, uint32_t *out)
{
    uint8_t status = 0;
    esp_err_t err = qmi8658_read(ctx, QMI8658_REG_STATUSINT, &status, 1);
    if (err != ESP_OK) {
        return err;
    }
    *out = status;
    return ESP_OK;
}

static bool qmi8658_backend_is_supported_addr(uint8_t i2c_addr)
{
    return i2c_addr == QMI8658_I2C_ADDRESS_LOW || i2c_addr == QMI8658_I2C_ADDRESS_HIGH;
}

static uint8_t qmi8658_backend_default_addr(void)
{
    return QMI8658_I2C_ADDRESS_LOW;
}

static int qmi8658_backend_sdo_level_for_addr(uint8_t i2c_addr)
{
    /* SA0/AD0 low selects 0x6A, high selects 0x6B. The mPython Pro straps it high. */
    return (i2c_addr == QMI8658_I2C_ADDRESS_HIGH) ? 1 : 0;
}

const lua_imu_backend_t lua_imu_backend = {
    .chip_name = "qmi8658",
    .state_size = 0,
    .probe = qmi8658_backend_probe,
    .destroy = NULL,
    .read_sample = qmi8658_backend_read_sample,
    .read_temperature = qmi8658_backend_read_temperature,
    .read_int_status = qmi8658_backend_read_int_status,
    .is_supported_addr = qmi8658_backend_is_supported_addr,
    .default_addr = qmi8658_backend_default_addr,
    .sdo_level_for_addr = qmi8658_backend_sdo_level_for_addr,
};
