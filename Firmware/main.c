/*
 * Smart Energy Meter - ESP32, ESP-IDF
 *
 * Simulation build: AC signals are synthetic (amplitude set by two
 * potentiometers on GPIO34/GPIO35). V_PER_COUNT and I_PER_COUNT are
 * placeholders pending calibration on hardware.
 *
 * Before running:
 *   - Set FIREBASE_URL to your own Realtime Database address (ending in
 *     /readings.json).
 *   - Set WIFI_SSID / WIFI_PASS. In the Wokwi simulator use "Wokwi-GUEST"
 *     with an empty password.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#define FIREBASE_URL   "YOUR Data base/readings.json"
#define WIFI_SSID      "YOUR WIFI NAME"
#define WIFI_PASS      "YOUR PASSWORD"
#define SEND_EVERY_N   5

#define ADC_MAX        4095.0f
#define ADC_MID        2048.0f
#define N_SAMPLES      200
#define MAX_AMP_COUNTS 2000.0f
#define V_PER_COUNT    0.1625f
#define I_PER_COUNT    0.0050f
#define PHASE_DEG      30.0f
#define PI_F           3.14159265f

#define WIFI_CONNECTED_BIT BIT0

static adc_oneshot_unit_handle_t adc1;
static EventGroupHandle_t wifi_events;
static int v_samples[N_SAMPLES];
static int i_samples[N_SAMPLES];

static float compute_mean(const int *s, int n) {
    float sum = 0.0f;
    for (int k = 0; k < n; k++) sum = sum + s[k];
    return sum / n;
}

static float compute_rms(const int *s, int n) {
    float mean = compute_mean(s, n);
    float sum_sq = 0.0f;
    for (int k = 0; k < n; k++) {
        float d = s[k] - mean;
        sum_sq = sum_sq + d * d;
    }
    return sqrtf(sum_sq / n);
}

static float compute_real_power(const int *v, const int *i, int n) {
    float mv = compute_mean(v, n);
    float mi = compute_mean(i, n);
    float sum = 0.0f;
    for (int k = 0; k < n; k++) {
        sum = sum + ((v[k] - mv) * V_PER_COUNT) * ((i[k] - mi) * I_PER_COUNT);
    }
    return sum / n;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_and_wait(void) {
    wifi_events = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    wifi_config_t wc = {
        .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS, .channel = 6 },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();

    printf("Connecting to WiFi...\n");
    xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    printf("WiFi connected, got IP\n");
}

static void send_reading(float vrms, float irms, float p, float pf, float e_wh) {
    char body[220];
    snprintf(body, sizeof(body),
             "{\"vrms\":%.1f,\"irms\":%.2f,\"p\":%.1f,\"pf\":%.2f,\"e_wh\":%.4f,"
             "\"ts\":{\".sv\":\"timestamp\"}}",
             vrms, irms, p, pf, e_wh);

    esp_http_client_config_t cfg = {
        .url = FIREBASE_URL,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        printf("Firebase HTTP status: %d\n", esp_http_client_get_status_code(client));
    } else {
        printf("Firebase error: %s\n", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

static void meter_task(void *arg) {
    float energy_wh = 0.0f;
    int64_t t_prev = esp_timer_get_time();
    int loop_count = 0;

    while (1) {
        int potI, potV;
        adc_oneshot_read(adc1, ADC_CHANNEL_6, &potI);
        adc_oneshot_read(adc1, ADC_CHANNEL_7, &potV);
        float ampI = potI / ADC_MAX * MAX_AMP_COUNTS;
        float ampV = potV / ADC_MAX * MAX_AMP_COUNTS;

        float phase = PHASE_DEG * PI_F / 180.0f;
        for (int k = 0; k < N_SAMPLES; k++) {
            float theta = 2.0f * PI_F * k / N_SAMPLES;
            v_samples[k] = (int)(ADC_MID + ampV * sinf(theta));
            i_samples[k] = (int)(ADC_MID + ampI * sinf(theta - phase));
        }

        float vrms   = compute_rms(v_samples, N_SAMPLES) * V_PER_COUNT;
        float irms   = compute_rms(i_samples, N_SAMPLES) * I_PER_COUNT;
        float p_real = compute_real_power(v_samples, i_samples, N_SAMPLES);
        float s_app  = vrms * irms;
        float pf     = (s_app > 0.01f) ? p_real / s_app : 0.0f;

        int64_t t_now = esp_timer_get_time();
        energy_wh = energy_wh + p_real * ((t_now - t_prev) / 1e6f / 3600.0f);
        t_prev = t_now;

        printf("Vrms=%6.1f V | Irms=%5.2f A | P=%7.1f W | S=%7.1f VA | PF=%.2f | E=%.4f Wh\n",
               vrms, irms, p_real, s_app, pf, energy_wh);

        if (++loop_count >= SEND_EVERY_N) {
            loop_count = 0;
            send_reading(vrms, irms, p_real, pf, energy_wh);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&unit_cfg, &adc1);
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten    = ADC_ATTEN_DB_12,
    };
    adc_oneshot_config_channel(adc1, ADC_CHANNEL_6, &chan_cfg);
    adc_oneshot_config_channel(adc1, ADC_CHANNEL_7, &chan_cfg);

    wifi_init_and_wait();
    xTaskCreate(meter_task, "meter", 8192, NULL, 5, NULL);
}