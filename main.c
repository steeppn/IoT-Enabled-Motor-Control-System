#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "math.h"

// Networking & Storage
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "mqtt_client.h"

#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t s_wifi_event_group;

static const char *TAG = "IOT_SWEEP_SYSTEM";

// --- CONFIGURATION ---
#define RED_LED 18
#define GREEN_LED 19
#define YELLOW_LED 5
#define BTN_START_PIN 21
#define BTN_STOP_PIN 47
#define POT_ADC_CHANNEL ADC_CHANNEL_0 
#define SERVO_PIN 16

#define SERVO_MODE LEDC_LOW_SPEED_MODE
#define SERVO_CHANNEL LEDC_CHANNEL_0
#define MAX_DUTY_RES 8191 
#define MIN_PULSE 500 
#define MAX_PULSE 2400 
#define MIN_STEP_SIZE 5
#define MAX_STEP_SIZE 80

// --- NETWORK CONFIG ---
#define WOKWI_SSID "Wokwi-GUEST"
#define WOKWI_PASS ""
// Use public MQTT broker for testing
#define LOCALSTACK_URL "mqtt://test.mosquitto.org:1883"

// --- GLOBALS ---
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool is_running = false;
static bool fault_critical = false;

// --- HELPERS ---
uint32_t map_value(uint32_t x, uint32_t in_min, uint32_t in_max, uint32_t out_min, uint32_t out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

uint32_t us_to_duty(int us) {
    return (us * MAX_DUTY_RES) / 20000;
}

// --- MQTT EVENT HANDLER ---
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to LocalStack MQTT Broker!");
            esp_mqtt_client_subscribe(mqtt_client, "device/commands", 0);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Lost connection to LocalStack!");
            break;
        default:
            break;
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT); // Signal success!
    }
}

// --- WIFI INITIALIZATION ---
void wifi_init(void) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(nvs_flash_init());
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WOKWI_SSID,
            .password = WOKWI_PASS,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    ESP_LOGI(TAG, "Wi-Fi Init Finished.");
}

// --- MQTT INITIALIZATION ---
void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = LOCALSTACK_URL,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

void app_main(void)
{
    // 1. SYSTEM INIT
    wifi_init();

    ESP_LOGI(TAG, "Waiting for WiFi...");
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    mqtt_app_start();

    // 2. HARDWARE SETUP (LEDs, Buttons, ADC, PWM)
    gpio_config_t io_conf = {
        .pin_bit_mask = ((1ULL << RED_LED) | (1ULL << GREEN_LED) | (1ULL << YELLOW_LED)),
        .mode = GPIO_MODE_OUTPUT
    };
    gpio_config(&io_conf);

    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BTN_START_PIN) | (1ULL << BTN_STOP_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE
    };
    gpio_config(&btn_conf);

    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config1 = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&init_config1, &adc1_handle);
    adc_oneshot_chan_cfg_t adc_config = { .bitwidth = ADC_BITWIDTH_12, .atten = ADC_ATTEN_DB_12 };
    adc_oneshot_config_channel(adc1_handle, POT_ADC_CHANNEL, &adc_config);

    ledc_timer_config_t ledc_timer = {
        .speed_mode = SERVO_MODE, .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_13_BIT, .freq_hz = 50, .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode = SERVO_MODE, .channel = SERVO_CHANNEL, .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE, .gpio_num = SERVO_PIN, .duty = us_to_duty(MIN_PULSE), .hpoint = 0
    };
    ledc_channel_config(&ledc_channel);

    // 3. LOOP VARIABLES
    int current_pulse = MIN_PULSE;
    int direction = 1;
    float sim_temp = 25.0;
    float sim_current = 0.0;
    int log_counter = 0;

    while (1) {
        // --- BUTTON LOGIC ---
        if (!gpio_get_level(BTN_START_PIN)) {
            is_running = true;
            gpio_set_level(GREEN_LED, 1);
        }
        if (!gpio_get_level(BTN_STOP_PIN)) {
            is_running = false;
            fault_critical = false;
            gpio_set_level(RED_LED, 0);
            gpio_set_level(GREEN_LED, 0);
        }

        // --- SENSOR READING ---
        int pot_raw = 0;
        adc_oneshot_read(adc1_handle, POT_ADC_CHANNEL, &pot_raw);
        int step_size = map_value(pot_raw, 0, 4095, MIN_STEP_SIZE, MAX_STEP_SIZE);

        // --- PHYSICS SIMULATION ---
        if (is_running && !fault_critical) {
            sim_temp += (step_size / 100.0) * 0.12 - (sim_temp - 25.0) * 0.01;
            sim_current = 0.5 + ((float)step_size / MAX_STEP_SIZE) * 1.2 + ((rand() % 10) / 100.0);
            
            current_pulse += (step_size * direction);
            if (current_pulse >= MAX_PULSE || current_pulse <= MIN_PULSE) direction *= -1;

            ledc_set_duty(SERVO_MODE, SERVO_CHANNEL, us_to_duty(current_pulse));
            ledc_update_duty(SERVO_MODE, SERVO_CHANNEL);
        } else {
            if (sim_temp > 25.0) sim_temp -= 0.02;
            sim_current = 0.05;
        }

        // --- TELEMETRY (MQTT) ---
        if (log_counter++ > 50) { // Publish roughly every 1 second
            char payload[128];
            snprintf(payload, sizeof(payload),
                     "{\"status\":\"%s\",\"temp\":%.2f,\"current\":%.2f,\"speed\":%d}",
                     is_running ? "RUNNING" : "STOPPED", sim_temp, sim_current, is_running ? step_size : 0);

            if (mqtt_client != NULL) {
                esp_mqtt_client_publish(mqtt_client, "eras_esp32/telemetry", payload, 0, 1, 0);
                ESP_LOGI(TAG, "Published: %s", payload);
            }
            log_counter = 0;
        }

        // --- FAULT PROTECTION ---
        if (sim_temp >= 34.0f && sim_current >= 1.70f) {
            gpio_set_level(RED_LED, 1);
            gpio_set_level(GREEN_LED, 0);
            ledc_set_duty(SERVO_MODE, SERVO_CHANNEL, 0);
            ledc_update_duty(SERVO_MODE, SERVO_CHANNEL);
            is_running = false;
            fault_critical = true;
            ESP_LOGE(TAG, "CRITICAL FAULT: OVERHEAT/OVERCURRENT");
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}