#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;

typedef enum { I2C_ADDR_BIT_LEN_7 = 0 } i2c_addr_bit_len_t;

typedef struct {
    i2c_addr_bit_len_t dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
} i2c_device_config_t;

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                    i2c_master_dev_handle_t *dev);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *write_buf,
                              size_t write_size, int xfer_timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *write_buf,
                                      size_t write_size, uint8_t *read_buf, size_t read_size,
                                      int xfer_timeout_ms);
