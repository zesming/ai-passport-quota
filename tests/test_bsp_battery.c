// Exercise CW2017 battery driver (bsp_battery.c) against a register-level I2C fake.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../components/bsp/src/bsp_battery.c"

static int bus_token;
static int dev_token;
static esp_err_t fake_i2c_init_err = ESP_OK;
static esp_err_t fake_add_dev_err = ESP_OK;
static bool fake_chip_present = true;
static bool fake_fail_write;
static int fake_fail_read_reg = -1;
static unsigned fake_rm_calls;
static unsigned fake_delays;
static unsigned fake_soc_ready_after;
static unsigned fake_soc_reads;
static uint8_t fake_regs[256];

static void reset_fake(void)
{
    s_dev = NULL;
    fake_i2c_init_err = ESP_OK;
    fake_add_dev_err = ESP_OK;
    fake_chip_present = true;
    fake_fail_write = false;
    fake_fail_read_reg = -1;
    fake_rm_calls = 0;
    fake_delays = 0;
    fake_soc_ready_after = 0;
    fake_soc_reads = 0;
    memset(fake_regs, 0, sizeof(fake_regs));
    fake_regs[CW_REG_VERSION] = 0x70;
    fake_regs[CW_REG_CONFIG] = CW_CONFIG_SLEEP;
    fake_regs[CW_REG_SOC_H] = 75;
    fake_regs[CW_REG_VCELL_H] = 0x33;
    fake_regs[CW_REG_VCELL_H + 1] = 0x33;
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
    fake_delays++;
}

esp_err_t bsp_i2c_init(void)
{
    return fake_i2c_init_err;
}

i2c_master_bus_handle_t bsp_i2c_bus(void)
{
    return &bus_token;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                    i2c_master_dev_handle_t *dev)
{
    assert(bus == &bus_token);
    assert(cfg->device_address == BSP_I2C_CW2017_ADDR);
    if (fake_add_dev_err != ESP_OK)
        return fake_add_dev_err;
    *dev = &dev_token;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev)
{
    assert(dev == &dev_token);
    fake_rm_calls++;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *write_buf,
                              size_t write_size, int xfer_timeout_ms)
{
    (void)xfer_timeout_ms;
    assert(dev == &dev_token && write_size >= 2);
    if (!fake_chip_present || fake_fail_write)
        return ESP_FAIL;
    uint8_t reg = write_buf[0];
    for (size_t i = 1; i < write_size; i++) {
        assert((size_t)reg + i - 1 < sizeof(fake_regs));
        fake_regs[reg + i - 1] = write_buf[i];
    }
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *write_buf,
                                      size_t write_size, uint8_t *read_buf, size_t read_size,
                                      int xfer_timeout_ms)
{
    (void)xfer_timeout_ms;
    assert(dev == &dev_token && write_size == 1);
    uint8_t reg = write_buf[0];
    if (!fake_chip_present || fake_fail_read_reg == (int)reg)
        return ESP_FAIL;
    if (reg == CW_REG_SOC_H && read_size == 1) {
        fake_soc_reads++;
        read_buf[0] = fake_soc_reads < fake_soc_ready_after ? 0xFF : fake_regs[CW_REG_SOC_H];
        return ESP_OK;
    }
    assert((size_t)reg + read_size <= sizeof(fake_regs));
    memcpy(read_buf, &fake_regs[reg], read_size);
    return ESP_OK;
}

int main(void)
{
    /* Uninitialized reads fail cleanly with -1. */
    reset_fake();
    assert(bsp_battery_soc() == -1 && bsp_battery_mv() == -1);

    /* Missing CW2017 chip returns ESP_ERR_NOT_FOUND and releases the I2C device handle. */
    fake_chip_present = false;
    assert(bsp_battery_init() == ESP_ERR_NOT_FOUND && s_dev == NULL && fake_rm_calls == 1);

    /* Profile mismatch writes all 80 bytes, sets UPDATE_FLAG, wakes chip, and waits for SOC. */
    reset_fake();
    fake_soc_ready_after = 3;
    assert(bsp_battery_init() == ESP_OK && s_dev == &dev_token && fake_rm_calls == 0);
    assert(memcmp(&fake_regs[CW_REG_PROFILE], s_battery_profile, CW_PROFILE_SIZE) == 0);
    assert((fake_regs[CW_REG_SOC_ALERT] & CW_UPDATE_FLAG) != 0);
    assert(fake_regs[CW_REG_CONFIG] == CW_CONFIG_ACTIVE);
    assert(fake_soc_reads == 3);
    /* Second init is idempotent. */
    unsigned delays_before = fake_delays;
    assert(bsp_battery_init() == ESP_OK && fake_delays == delays_before);

    /* Matching profile skips rewrite and activates sleeping gauge. */
    reset_fake();
    memcpy(&fake_regs[CW_REG_PROFILE], s_battery_profile, CW_PROFILE_SIZE);
    fake_regs[CW_REG_SOC_ALERT] = CW_UPDATE_FLAG;
    fake_regs[CW_REG_CONFIG] = CW_CONFIG_SLEEP;
    assert(bsp_battery_init() == ESP_OK && fake_regs[CW_REG_CONFIG] == CW_CONFIG_ACTIVE);

    /* SOC reading: valid 0..100 passes, >100 (0xFF) returns -1, I2C error returns -1. */
    fake_regs[CW_REG_SOC_H] = 0;
    assert(bsp_battery_soc() == 0);
    fake_regs[CW_REG_SOC_H] = 100;
    assert(bsp_battery_soc() == 100);
    fake_regs[CW_REG_SOC_H] = 101;
    assert(bsp_battery_soc() == -1);
    fake_fail_read_reg = CW_REG_SOC_H;
    assert(bsp_battery_soc() == -1);
    fake_fail_read_reg = -1;

    /* 14-bit voltage conversion: upper two bits masked, raw * 312.5 uV -> mV. */
    uint16_t raw = 12800; /* 12800 * 0.3125 mV = 4000 mV */
    fake_regs[CW_REG_VCELL_H] = (uint8_t)((raw >> 8) | 0xC0);
    fake_regs[CW_REG_VCELL_H + 1] = (uint8_t)(raw & 0xFF);
    assert(bsp_battery_mv() == 4000);
    fake_fail_read_reg = CW_REG_VCELL_H;
    assert(bsp_battery_mv() == -1);
    fake_fail_read_reg = -1;

    /* Timeout waiting for SOC ready rolls back device handle and returns ESP_ERR_TIMEOUT. */
    reset_fake();
    memcpy(&fake_regs[CW_REG_PROFILE], s_battery_profile, CW_PROFILE_SIZE);
    fake_regs[CW_REG_SOC_ALERT] = CW_UPDATE_FLAG;
    fake_regs[CW_REG_CONFIG] = CW_CONFIG_ACTIVE;
    fake_soc_ready_after = 100;
    assert(bsp_battery_init() == ESP_ERR_TIMEOUT && s_dev == NULL && fake_rm_calls == 1);

    /* Write failure during profile upload rolls back cleanly. */
    reset_fake();
    fake_fail_write = true;
    assert(bsp_battery_init() == ESP_FAIL && s_dev == NULL && fake_rm_calls == 1);

    puts("BSP battery driver tests: PASS");
    return 0;
}
