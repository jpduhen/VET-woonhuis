#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "rfid_rc522.h"

static const char *TAG = "vet-proef";

// RC522 via de gedeelde VSPI/SPI3-bus uit het ontwerp.
#define RFID_MISO_GPIO 19
#define RFID_MOSI_GPIO 23
#define RFID_SCLK_GPIO 18
#define RFID_CS_GPIO   13
#define RFID_RST_GPIO  (-1) // RST van de RC522 verbinden met 3,3 V

// WS2812B/NeoPixel-achtige RGB-strip.
#define LED_DATA_GPIO 27
#define LED_COUNT     24
#define LED_BRIGHTNESS 32 // 0..255; bewust laag voor de proefvoeding

typedef enum {
    LED_STATE_IDLE,
    LED_STATE_CARD_PRESENT,
    LED_STATE_CARD_REMOVED,
} led_state_t;

static led_strip_handle_t led_strip;

static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile led_state_t requested_led_state = LED_STATE_IDLE;

static uint8_t scale_brightness(uint8_t value)
{
    return (uint8_t)(((uint16_t)value * LED_BRIGHTNESS) / 255U);
}

static void set_pixel(uint32_t index, uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_ERROR_CHECK(led_strip_set_pixel(
        led_strip,
        index,
        scale_brightness(red),
        scale_brightness(green),
        scale_brightness(blue)));
}

static void request_led_state(led_state_t state)
{
    portENTER_CRITICAL(&state_lock);
    requested_led_state = state;
    portEXIT_CRITICAL(&state_lock);
}

static led_state_t get_led_state(void)
{
    portENTER_CRITICAL(&state_lock);
    led_state_t state = requested_led_state;
    portEXIT_CRITICAL(&state_lock);
    return state;
}

static void rfid_poll_task(void *arg)
{
    (void)arg;
    bool was_present = false;
    uint8_t previous_uid[4] = {0};

    while (true) {
        bool present = false;
        uint8_t uid[4] = {0};
        esp_err_t err = rfid_rc522_poll(uid, &present);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "RFID-pollfout: %s", esp_err_to_name(err));
        } else if (present && (!was_present || memcmp(uid, previous_uid, 4) != 0)) {
            ESP_LOGI(TAG, "RFID-kaart gedetecteerd, UID: %02X:%02X:%02X:%02X",
                     uid[0], uid[1], uid[2], uid[3]);
            memcpy(previous_uid, uid, 4);
            request_led_state(LED_STATE_CARD_PRESENT);
        } else if (!present && was_present) {
            ESP_LOGI(TAG, "RFID-kaart verwijderd");
            request_led_state(LED_STATE_CARD_REMOVED);
        }

        was_present = present;
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void led_animation_task(void *arg)
{
    (void)arg;
    uint32_t position = 0;
    uint32_t removed_frames = 0;
    led_state_t previous_state = LED_STATE_IDLE;

    while (true) {
        led_state_t state = get_led_state();

        if (state != previous_state) {
            position = 0;
            if (state == LED_STATE_CARD_REMOVED) {
                removed_frames = 8; // circa 0,8 seconde rood knipperen
            }
            previous_state = state;
        }

        ESP_ERROR_CHECK(led_strip_clear(led_strip));

        if (state == LED_STATE_CARD_PRESENT) {
            // Blauw spoor: kaart aanwezig / later te gebruiken voor teruglevering.
            set_pixel(position, 0, 80, 255);
            set_pixel((position + LED_COUNT - 1U) % LED_COUNT, 0, 20, 64);
        } else if (state == LED_STATE_CARD_REMOVED && removed_frames > 0) {
            // Kort rood signaal bij verwijderen.
            if ((removed_frames % 2U) == 0U) {
                for (uint32_t i = 0; i < LED_COUNT; ++i) {
                    set_pixel(i, 255, 0, 0);
                }
            }
            --removed_frames;
            if (removed_frames == 0) {
                request_led_state(LED_STATE_IDLE);
            }
        } else {
            // Groen stand-by-spoor: hardware actief, nog geen kaart aanwezig.
            set_pixel(position, 0, 255, 0);
            set_pixel((position + LED_COUNT - 1U) % LED_COUNT, 0, 48, 0);
        }

        ESP_ERROR_CHECK(led_strip_refresh(led_strip));
        position = (position + 1U) % LED_COUNT;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void init_led_strip(void)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = LED_DATA_GPIO,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };

    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = false,
        },
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    ESP_ERROR_CHECK(led_strip_clear(led_strip));
    ESP_LOGI(TAG, "LED-strip gereed: %d pixels op GPIO%d", LED_COUNT, LED_DATA_GPIO);
}

static void init_rfid(void)
{
    const rfid_rc522_config_t config = {
        .host = SPI3_HOST,
        .miso_gpio = RFID_MISO_GPIO,
        .mosi_gpio = RFID_MOSI_GPIO,
        .sclk_gpio = RFID_SCLK_GPIO,
        .cs_gpio = RFID_CS_GPIO,
        .clock_speed_hz = 1000 * 1000,
    };
    ESP_ERROR_CHECK(rfid_rc522_init(&config));

    ESP_LOGI(TAG,
             "RC522 gereed: SCK=%d MOSI=%d MISO=%d CS=%d, SPI=1 MHz",
             RFID_SCLK_GPIO, RFID_MOSI_GPIO, RFID_MISO_GPIO, RFID_CS_GPIO);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Start proefopstelling met 1 RC522 en 24 RGB-leds");
    init_led_strip();
    init_rfid();

    BaseType_t task_created = xTaskCreate(
        led_animation_task,
        "led_animation",
        3072,
        NULL,
        5,
        NULL);
    ESP_ERROR_CHECK(task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    task_created = xTaskCreate(
        rfid_poll_task,
        "rfid_poll",
        3072,
        NULL,
        5,
        NULL);
    ESP_ERROR_CHECK(task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
