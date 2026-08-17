#include "rfid_rc522.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rc522-local";
static spi_device_handle_t device;

enum {
    REG_COMMAND = 0x01,
    REG_COM_IRQ = 0x04,
    REG_DIV_IRQ = 0x05,
    REG_ERROR = 0x06,
    REG_FIFO_DATA = 0x09,
    REG_FIFO_LEVEL = 0x0A,
    REG_CONTROL = 0x0C,
    REG_BIT_FRAMING = 0x0D,
    REG_MODE = 0x11,
    REG_TX_CONTROL = 0x14,
    REG_TX_ASK = 0x15,
    REG_T_MODE = 0x2A,
    REG_T_PRESCALER = 0x2B,
    REG_T_RELOAD_H = 0x2C,
    REG_T_RELOAD_L = 0x2D,
    REG_CRC_RESULT_H = 0x21,
    REG_CRC_RESULT_L = 0x22,
    REG_VERSION = 0x37,
};

enum {
    CMD_IDLE = 0x00,
    CMD_CALC_CRC = 0x03,
    CMD_TRANSCEIVE = 0x0C,
    CMD_SOFT_RESET = 0x0F,
    PICC_WUPA = 0x52,
    PICC_ANTICOLL_CL1 = 0x93,
};

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = {(uint8_t)((reg << 1U) & 0x7EU), value};
    spi_transaction_t transaction = {
        .length = 16,
        .tx_buffer = tx,
    };
    return spi_device_polling_transmit(device, &transaction);
}

static esp_err_t read_reg(uint8_t reg, uint8_t *value)
{
    uint8_t tx[2] = {(uint8_t)(((reg << 1U) & 0x7EU) | 0x80U), 0};
    uint8_t rx[2] = {0};
    spi_transaction_t transaction = {
        .length = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    ESP_RETURN_ON_ERROR(spi_device_polling_transmit(device, &transaction), TAG,
                        "SPI read failed");
    *value = rx[1];
    return ESP_OK;
}

static esp_err_t set_bits(uint8_t reg, uint8_t mask)
{
    uint8_t value;
    ESP_RETURN_ON_ERROR(read_reg(reg, &value), TAG, "register read failed");
    return write_reg(reg, value | mask);
}

static esp_err_t clear_bits(uint8_t reg, uint8_t mask)
{
    uint8_t value;
    ESP_RETURN_ON_ERROR(read_reg(reg, &value), TAG, "register read failed");
    return write_reg(reg, value & (uint8_t)~mask);
}

static esp_err_t transceive(const uint8_t *tx, size_t tx_length,
                            uint8_t *rx, size_t rx_capacity,
                            size_t *rx_length, uint8_t *rx_last_bits)
{
    ESP_RETURN_ON_ERROR(write_reg(REG_COMMAND, CMD_IDLE), TAG, "idle failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_COM_IRQ, 0x7F), TAG, "IRQ clear failed");
    ESP_RETURN_ON_ERROR(set_bits(REG_FIFO_LEVEL, 0x80), TAG, "FIFO flush failed");

    for (size_t i = 0; i < tx_length; ++i) {
        ESP_RETURN_ON_ERROR(write_reg(REG_FIFO_DATA, tx[i]), TAG, "FIFO write failed");
    }

    ESP_RETURN_ON_ERROR(write_reg(REG_COMMAND, CMD_TRANSCEIVE), TAG,
                        "transceive start failed");
    ESP_RETURN_ON_ERROR(set_bits(REG_BIT_FRAMING, 0x80), TAG, "StartSend failed");

    uint8_t irq = 0;
    for (int timeout = 0; timeout < 40; ++timeout) {
        ESP_RETURN_ON_ERROR(read_reg(REG_COM_IRQ, &irq), TAG, "IRQ read failed");
        if ((irq & 0x31U) != 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_RETURN_ON_ERROR(clear_bits(REG_BIT_FRAMING, 0x80), TAG, "StartSend clear failed");

    if ((irq & 0x01U) != 0 || (irq & 0x30U) == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t error;
    ESP_RETURN_ON_ERROR(read_reg(REG_ERROR, &error), TAG, "error register read failed");
    if ((error & 0x1BU) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t fifo_count;
    uint8_t control;
    ESP_RETURN_ON_ERROR(read_reg(REG_FIFO_LEVEL, &fifo_count), TAG, "FIFO count failed");
    ESP_RETURN_ON_ERROR(read_reg(REG_CONTROL, &control), TAG, "control read failed");

    size_t count = fifo_count;
    if (count > rx_capacity) {
        count = rx_capacity;
    }
    for (size_t i = 0; i < count; ++i) {
        ESP_RETURN_ON_ERROR(read_reg(REG_FIFO_DATA, &rx[i]), TAG, "FIFO read failed");
    }

    *rx_length = count;
    *rx_last_bits = control & 0x07U;
    return ESP_OK;
}

static esp_err_t calculate_crc(const uint8_t *data, size_t length, uint8_t result[2])
{
    ESP_RETURN_ON_ERROR(write_reg(REG_COMMAND, CMD_IDLE), TAG, "CRC idle failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_DIV_IRQ, 0x04), TAG, "CRC IRQ clear failed");
    ESP_RETURN_ON_ERROR(set_bits(REG_FIFO_LEVEL, 0x80), TAG, "CRC FIFO flush failed");
    for (size_t i = 0; i < length; ++i) {
        ESP_RETURN_ON_ERROR(write_reg(REG_FIFO_DATA, data[i]), TAG, "CRC FIFO write failed");
    }
    ESP_RETURN_ON_ERROR(write_reg(REG_COMMAND, CMD_CALC_CRC), TAG, "CRC start failed");

    uint8_t irq = 0;
    for (int timeout = 0; timeout < 50; ++timeout) {
        ESP_RETURN_ON_ERROR(read_reg(REG_DIV_IRQ, &irq), TAG, "CRC IRQ read failed");
        if ((irq & 0x04U) != 0) {
            ESP_RETURN_ON_ERROR(write_reg(REG_COMMAND, CMD_IDLE), TAG, "CRC stop failed");
            ESP_RETURN_ON_ERROR(read_reg(REG_CRC_RESULT_L, &result[0]), TAG,
                                "CRC result failed");
            ESP_RETURN_ON_ERROR(read_reg(REG_CRC_RESULT_H, &result[1]), TAG,
                                "CRC result failed");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t select_and_halt(const uint8_t uid_with_bcc[5])
{
    uint8_t select_frame[9] = {PICC_ANTICOLL_CL1, 0x70};
    memcpy(&select_frame[2], uid_with_bcc, 5);
    ESP_RETURN_ON_ERROR(calculate_crc(select_frame, 7, &select_frame[7]), TAG,
                        "select CRC failed");

    uint8_t sak_response[3];
    size_t sak_length = 0;
    uint8_t last_bits = 0;
    ESP_RETURN_ON_ERROR(transceive(select_frame, sizeof(select_frame), sak_response,
                                   sizeof(sak_response), &sak_length, &last_bits),
                        TAG, "select failed");
    if (sak_length != 3 || last_bits != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t halt_frame[4] = {0x50, 0x00};
    ESP_RETURN_ON_ERROR(calculate_crc(halt_frame, 2, &halt_frame[2]), TAG,
                        "halt CRC failed");

    uint8_t ignored[1];
    size_t ignored_length = 0;
    esp_err_t err = transceive(halt_frame, sizeof(halt_frame), ignored,
                               sizeof(ignored), &ignored_length, &last_bits);
    // Een correct uitgevoerde HALT geeft bewust geen antwoord.
    return err == ESP_ERR_NOT_FOUND ? ESP_OK : err;
}

esp_err_t rfid_rc522_init(const rfid_rc522_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is null");

    const spi_bus_config_t bus_config = {
        .mosi_io_num = config->mosi_gpio,
        .miso_io_num = config->miso_gpio,
        .sclk_io_num = config->sclk_gpio,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = 16,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(config->host, &bus_config, SPI_DMA_DISABLED),
                        TAG, "SPI bus init failed");

    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = config->clock_speed_hz,
        .mode = 0,
        .spics_io_num = config->cs_gpio,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(config->host, &device_config, &device),
                        TAG, "SPI device init failed");

    ESP_RETURN_ON_ERROR(write_reg(REG_COMMAND, CMD_SOFT_RESET), TAG, "reset failed");
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_RETURN_ON_ERROR(write_reg(REG_T_MODE, 0x8D), TAG, "timer config failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_T_PRESCALER, 0x3E), TAG, "timer config failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_T_RELOAD_H, 0x00), TAG, "timer config failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_T_RELOAD_L, 30), TAG, "timer config failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_TX_ASK, 0x40), TAG, "ASK config failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_MODE, 0x3D), TAG, "mode config failed");
    ESP_RETURN_ON_ERROR(set_bits(REG_TX_CONTROL, 0x03), TAG, "antenna enable failed");

    uint8_t version;
    ESP_RETURN_ON_ERROR(read_reg(REG_VERSION, &version), TAG, "version read failed");
    ESP_LOGI(TAG, "MFRC522 VersionReg=0x%02X", version);
    if (version == 0x00 || version == 0xFF) {
        ESP_LOGW(TAG, "Onverwachte versie; controleer voeding en SPI-bedrading");
    }
    return ESP_OK;
}

esp_err_t rfid_rc522_poll(uint8_t uid[4], bool *present)
{
    ESP_RETURN_ON_FALSE(uid != NULL && present != NULL, ESP_ERR_INVALID_ARG,
                        TAG, "invalid argument");
    *present = false;

    // WUPA gebruikt zeven geldige bits en wekt ook een eerder gestopte kaart.
    ESP_RETURN_ON_ERROR(write_reg(REG_BIT_FRAMING, 0x07), TAG, "framing failed");
    const uint8_t request[] = {PICC_WUPA};
    uint8_t atqa[2];
    size_t atqa_length = 0;
    uint8_t last_bits = 0;
    esp_err_t err = transceive(request, sizeof(request), atqa, sizeof(atqa),
                               &atqa_length, &last_bits);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_RESPONSE) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "REQA failed");
    if (atqa_length != 2 || last_bits != 0) {
        return ESP_OK;
    }

    // Anticollision cascade level 1. De proef ondersteunt vierbyte-UID's.
    ESP_RETURN_ON_ERROR(write_reg(REG_BIT_FRAMING, 0x00), TAG, "framing failed");
    const uint8_t anticollision[] = {PICC_ANTICOLL_CL1, 0x20};
    uint8_t response[5];
    size_t response_length = 0;
    err = transceive(anticollision, sizeof(anticollision), response, sizeof(response),
                     &response_length, &last_bits);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_RESPONSE) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "anticollision failed");
    if (response_length != 5 || last_bits != 0 || response[0] == 0x88) {
        return ESP_OK;
    }

    if ((uint8_t)(response[0] ^ response[1] ^ response[2] ^ response[3]) != response[4]) {
        return ESP_ERR_INVALID_CRC;
    }

    memcpy(uid, response, 4);
    ESP_RETURN_ON_ERROR(select_and_halt(response), TAG, "select/halt failed");
    *present = true;
    return ESP_OK;
}
