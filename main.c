#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h" // main rtos
#include "freertos/task.h" // specific rtos task-related functions
#include "driver/gpio.h" // all functions to control pins
#include "driver/ledc.h" // functions to handle PWM
#include "esp_adc/adc_oneshot.h" // analog to digital converted - oneshot
#include "esp_log.h" // log functions for debugging
#include <math.h> // Required for rand()

// WiFi and MQTT
#include "mqtt_client.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"

// --- LEDS ----
#define RED_LED             18
#define GREEN_LED           19
#define YELLOW_LED          5

// --- PINS ---
#define BTN_START_PIN       21
#define BTN_STOP_PIN        47
#define POT_ADC_CHANNEL     ADC_CHANNEL_0  // GPIO 1
#define SERVO_PIN           16

// --- SERVO CONFIG ---
#define SERVO_TIMER         LEDC_TIMER_0
#define SERVO_MODE          LEDC_LOW_SPEED_MODE
#define SERVO_CHANNEL       LEDC_CHANNEL_0
#define SERVO_FREQ_HZ       50
#define SERVO_RES           LEDC_TIMER_13_BIT
#define MAX_DUTY_RES        8191 // 2^13 - 1 

// Servo Pulse Widths (Microseconds)
#define MIN_PULSE           500  // 0 degrees
#define MAX_PULSE           2400 // 180 degrees

// --- LOGIC CONSTANTS ---
#define MIN_STEP_SIZE       5    // Slowest speed
#define MAX_STEP_SIZE       80   // Fastest speed

static const char *TAG = "AUTO_SWEEP";

// Helper: Map function // normalize, scale, shift
uint32_t map_value(uint32_t x, uint32_t in_min, uint32_t in_max, uint32_t out_min, uint32_t out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// Helper: Convert Microseconds to PWM Duty
uint32_t us_to_duty(int us) {
  return (us * MAX_DUTY_RES) / 20000; // 20000us = 20ms period (50Hz)
}

// Handles the connection "handshake"
static esp_mqtt_client_handle_t mqtt_client;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to LocalStack MQTT!");
            // Subscribe to a command topic so we can stop the motor from the cloud
            esp_mqtt_client_subscribe(mqtt_client, "device/commands", 0);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Message received: %.*s", event->data_len, event->data);
            // Logic to parse "STOP" or "START" would go here
            break;
        default:
            break;
    }
}

void app_main(void)
{
  // SETUP LEDs
  gpio_config_t led_conf = {
    .pin_bit_mask = ((1ULL << RED_LED) | (1ULL << GREEN_LED) | (1ULL << YELLOW_LED)),
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE
  };
  gpio_config(&led_conf);

  // 1. SETUP BUTTONS
  gpio_config_t btn_conf = {
    .pin_bit_mask = (1ULL << BTN_START_PIN) | (1ULL << BTN_STOP_PIN),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE, // Critical for buttons connecting to GND
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE
  };
  gpio_config(&btn_conf);

  // 2. SETUP ADC (Potentiometer)
  adc_oneshot_unit_handle_t adc1_handle;
  adc_oneshot_unit_init_cfg_t init_config1 = { .unit_id = ADC_UNIT_1 };
  adc_oneshot_new_unit(&init_config1, &adc1_handle);

  adc_oneshot_chan_cfg_t adc_config = {
    .bitwidth = ADC_BITWIDTH_12,
    .atten = ADC_ATTEN_DB_12,
  };
  adc_oneshot_config_channel(adc1_handle, POT_ADC_CHANNEL, &adc_config);

  // 3. SETUP SERVO PWM
  ledc_timer_config_t ledc_timer = {
    .speed_mode = SERVO_MODE,
    .timer_num  = SERVO_TIMER,
    .duty_resolution = SERVO_RES,
    .freq_hz    = SERVO_FREQ_HZ,
    .clk_cfg    = LEDC_AUTO_CLK
  };
  ledc_timer_config(&ledc_timer);

  ledc_channel_config_t ledc_channel = {
    .speed_mode = SERVO_MODE,
    .channel    = SERVO_CHANNEL,
    .timer_sel  = SERVO_TIMER,
    .intr_type  = LEDC_INTR_DISABLE,
    .gpio_num   = SERVO_PIN,
    .duty       = us_to_duty(MIN_PULSE),
    .hpoint     = 0
  };
  ledc_channel_config(&ledc_channel);

  ESP_LOGI(TAG, "System Ready. Press GREEN to Start, RED to Stop.");

  // --- VARIABLES ---
  bool is_running = false;          // false = STOPPED, true = RUNNING
  int current_pulse = MIN_PULSE;    // Current servo position (us)
  int direction = 1;                // 1 = moving up, -1 = moving down

  // --- SENSOR SIMULATION VARIABLES ---
  float sim_temp = 25.0;       // Current temperature (°C)
  float sim_current = 0.0;     // Current draw (Amps)
  const float AMBIENT_TEMP = 25.0;
  bool fault_critical = false;

  while (1) {
    // --- 1. READ BUTTONS ---
    // !gpio_get_level because buttons connect to GND (Low = Pressed)
    if (!gpio_get_level(BTN_START_PIN)) {
      is_running = true;
      gpio_set_level(GREEN_LED, 1);
      ESP_LOGI(TAG, "Status: RUNNING");
    }
    if (!gpio_get_level(BTN_STOP_PIN)) {
      is_running = false;
      fault_critical = false;
      gpio_set_level(RED_LED, 0);
      gpio_set_level(GREEN_LED, 0);
      ESP_LOGI(TAG, "Status: STOPPED");
    }

    // --- 2. READ POTENTIOMETER & CALCULATE SPEED ---
    int pot_raw = 0;
    adc_oneshot_read(adc1_handle, POT_ADC_CHANNEL, &pot_raw);
    int step_size = map_value(pot_raw, 0, 4095, MIN_STEP_SIZE, MAX_STEP_SIZE);

    // --- 3. PHYSICS & MOTOR LOGIC ---
    if (is_running) {
      // A. Heat Calculation
      // The faster it moves, the more heat it generates
      float heat_gain = (step_size / 100.0) * 0.12;
      float heat_loss = (sim_temp - AMBIENT_TEMP) * 0.01;
      sim_temp += (heat_gain - heat_loss);

      // B. Current Calculation
      // Base (0.5A) + Load (speed) + Random Jitter
      float jitter = ((rand() % 10) / 100.0);
      sim_current = 0.5 + ((float)step_size / MAX_STEP_SIZE) * 1.2 + jitter;

      // C. Update Servo Position
      current_pulse += (step_size * direction);

      // D. Handle Turning Points (Back and Forth)
      if (current_pulse >= MAX_PULSE) {
        current_pulse = MAX_PULSE;
        direction = -1; // Reverse
      } else if (current_pulse <= MIN_PULSE) {
        current_pulse = MIN_PULSE;
        direction = 1;  // Forward
      }

      // E. Move Servo
      if (fault_critical == false){
        ledc_set_duty(SERVO_MODE, SERVO_CHANNEL, us_to_duty(current_pulse));
        ledc_update_duty(SERVO_MODE, SERVO_CHANNEL);
      }

    } else {
      // Cooling Logic: When stopped, return to 25C slowly
      if (sim_temp > AMBIENT_TEMP) {
        sim_temp -= 0.02;
      }
      // Idle current
      sim_current = 0.05 + ((rand() % 5) / 1000.0);
    }

    // --- TELEMETRY LOGGING ---
    static int log_counter = 0;
    if (log_counter++ > 25) {
      // Create a buffer for our JSON string
      char telemetry_payload[128];

      // Format the data into a clean JSON object
      snprintf(telemetry_payload, sizeof(telemetry_payload),
               "{\"status\":\"%s\",\"temp\":%.2f,\"current\":%.2f,\"speed\":%d}",
               is_running ? "RUNNING" : "STOPPED",
               sim_temp,
               sim_current,
               is_running ? step_size : 0);

      // PUBLISH TO LOCALSTACK
      if (mqtt_client != NULL) {
          esp_mqtt_client_publish(mqtt_client, "device/telemetry", telemetry_payload, 0, 1, 0);
      }
      
      log_counter = 0;
    }

    if (sim_temp >= 34.0f && sim_current >= 1.70f){
      gpio_set_level(RED_LED, 1);
      gpio_set_level(GREEN_LED, 0);
      ledc_set_duty(SERVO_MODE, SERVO_CHANNEL, 0);
      ledc_update_duty(SERVO_MODE, SERVO_CHANNEL);
      is_running = false;
      fault_critical = true;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}