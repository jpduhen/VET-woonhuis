#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

typedef struct {
    spi_host_device_t host;
    gpio_num_t miso_gpio;
    gpio_num_t mosi_gpio;
    gpio_num_t sclk_gpio;
    gpio_num_t cs_gpio;
    int clock_speed_hz;
} rfid_rc522_config_t;

/** Initialiseer één MFRC522 op een eigen SPI-bus. */
esp_err_t rfid_rc522_init(const rfid_rc522_config_t *config);

/**
 * Zoek een ISO14443A-kaart en lees de vierbyte-UID.
 *
 * @param uid buffer van minimaal vier bytes
 * @param present wordt true als een geldige kaart is gelezen
 */
esp_err_t rfid_rc522_poll(uint8_t uid[4], bool *present);
