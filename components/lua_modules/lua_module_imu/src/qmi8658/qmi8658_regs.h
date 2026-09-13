/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * QMI8658C register map. Values match the vendor datasheet register list, and the
 * WHO_AM_I / revision values were read back from Labplus mPython Pro hardware.
 */

#pragma once

#define QMI8658_REG_WHO_AM_I     0x00  /*!< Read-only, 0x05 for QMI8658 */
#define QMI8658_REG_REVISION     0x01  /*!< Read-only, 0x7C on mPython Pro hardware */
#define QMI8658_REG_CTRL1        0x02  /*!< Comms: auto-increment, endianness, interrupts */
#define QMI8658_REG_CTRL2        0x03  /*!< Accelerometer: [7:4] full scale, [3:0] ODR */
#define QMI8658_REG_CTRL3        0x04  /*!< Gyroscope: [7:4] full scale, [3:0] ODR */
#define QMI8658_REG_CTRL5        0x06  /*!< Low-pass filter */
#define QMI8658_REG_CTRL7        0x08  /*!< Sensor enable */
#define QMI8658_REG_STATUSINT    0x2D  /*!< Data-ready and latched status */
#define QMI8658_REG_TEMP_L       0x33  /*!< Temperature = TEMP_H + TEMP_L / 256 */
#define QMI8658_REG_TEMP_H       0x34
#define QMI8658_REG_ACCEL_XYZ    0x35  /*!< Six bytes: AX, AY, AZ, little endian */
#define QMI8658_REG_GYRO_XYZ     0x3B  /*!< Six bytes: GX, GY, GZ, little endian */
#define QMI8658_REG_RESET        0x60  /*!< Write QMI8658_RESET_CMD to soft reset */

#define QMI8658_WHO_AM_I_VALUE   0x05
#define QMI8658_RESET_CMD        0xB0

/*
 * CTRL1: ADDR_AI (bit 6) enables register auto-increment, which the multi-byte
 * accel/gyro burst read depends on. BE (bit 5) stays 0 for little endian, matching
 * how the sample bytes are assembled below.
 */
#define QMI8658_CTRL1_ADDR_AI    0x40

/*
 * CTRL2 / CTRL3 share the layout [7:4] full scale, [3:0] output data rate.
 * CTRL2 = 0x24 is +/-8 g at ODR index 4 (about 448 Hz);
 * CTRL3 = 0x24 is +/-64 dps at the same rate. These are the values verified on
 * hardware: a stationary board reports an acceleration magnitude of ~1 g.
 */
#define QMI8658_CTRL2_ACCEL_8G   0x24
#define QMI8658_CTRL3_GYRO_64DPS 0x24

/*
 * CTRL7 enables the sensors from the low bits: bit 0 = accelerometer, bit 1 =
 * gyroscope. The high bits are not the enable bits, which is easy to get wrong -
 * 0xC0 does not start either sensor.
 */
#define QMI8658_CTRL7_ENABLE_BOTH 0x03

/*!< Sensors need a moment after being enabled before the first valid frame. */
#define QMI8658_STARTUP_DELAY_MS  20

/*!< 7-bit addresses, selected by the SA0/AD0 strap. */
#define QMI8658_I2C_ADDRESS_LOW   0x6A
#define QMI8658_I2C_ADDRESS_HIGH  0x6B
