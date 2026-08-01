/*
 * Capstone Integration — App 1 (setup/blink/web) + App 3 (ISR/bottom-half)
 *
 * Theme: Avionics
 *
 * What's integrated:
 *   - Core 1: blink_task (1 Hz beacon, vTaskDelayUntil) + two bottom-half
 *     tasks fed by a button ISR on GPIO 18 (App 3's RADAR-contact pattern).
 *   - Core 0: Wi-Fi (Wokwi-GUEST) + HTTP server reporting BOTH the beacon
 *     state and live ISR-latency telemetry as JSON.
 *   - App 2's background-load fixture was intentionally left out per scope.
 *
 * Signaling paths (both kept, for your WCET/latency writeup):
 *   - Binary semaphore  -> btn_task_sem   (can lose presses if not drained)
 *   - Task notification -> btn_task_notif (one-to-one, typically lower latency)
 *
 * Fault injection idea for your demo video:
 *   Comment out portYIELD_FROM_ISR(higher_woken) in button_isr() and show
 *   the reported latency jump (bottom-half now waits for the next tick
 *   instead of running immediately on ISR exit).
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "nvs_flash.h"

/* ---------- Configuration ---------- */
#define LED_GPIO          GPIO_NUM_2
#define BLINK_PERIOD_MS   1000          /* 1 Hz beacon toggle */
#define HTTP_PORT         80

#define BUTTON_GPIO       GPIO_NUM_18   /* RADAR-contact button, active-low */
#define ISR_PULSE_GPIO    GPIO_NUM_19   /* scope this for ISR entry/exit */
#define DEBOUNCE_US       200

#define WIFI_SSID         "Wokwi-GUEST"
#define WIFI_PASS         ""             /* Wokwi virtual AP is open */

#define CONFIG_LOG_DEFAULT_LEVEL_INFO 1
#define CONFIG_LOG_MAXIMUM_LEVEL  5

static const char *TAG = "app1_app3";

/* ---------- Shared state: beacon (App 1) ---------- */
static volatile bool led_on = false;
static volatile uint32_t toggle_count = 0;

/* ---------- Shared state: ISR / bottom-half (App 3) ---------- */
static SemaphoreHandle_t btn_sem;
static TaskHandle_t      task_notif_handle;

static volatile int64_t  isr_entry_time_us;
static volatile int64_t  last_edge_us;
static volatile uint32_t presses_observed;
static volatile uint32_t radar_hit_count;
static volatile uint64_t latency_last_sem_us;
static volatile uint64_t latency_max_sem_us;
static volatile uint64_t latency_last_notif_us;
static volatile uint64_t latency_max_notif_us;

/* ============================================================
 *  Blink task — runs on Core 1. Drift-free via vTaskDelayUntil.
 * ============================================================ */
static void blink_task(void *arg)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(BLINK_PERIOD_MS);

    for (;;) {
        led_on = !led_on;
        toggle_count++;
        gpio_set_level(LED_GPIO, led_on);
        ESP_LOGI(TAG, "[Avionics] beacon = %s (toggle #%lu)",
                 led_on ? "Online" : "Offline", (unsigned long)toggle_count);

        vTaskDelayUntil(&last_wake, period);
    }
}

/* ============================================================
 *  ISR — button press = simulated RADAR contact event.
 * ============================================================ */
static void IRAM_ATTR button_isr(void *arg)
{
    int64_t now = esp_timer_get_time();

    if (now - last_edge_us < DEBOUNCE_US) return;
    last_edge_us = now;

    gpio_set_level(ISR_PULSE_GPIO, 1);

    isr_entry_time_us = now;
    presses_observed++;

    BaseType_t higher_woken = pdFALSE;

    xSemaphoreGiveFromISR(btn_sem, &higher_woken);
    vTaskNotifyGiveFromISR(task_notif_handle, &higher_woken);

    gpio_set_level(ISR_PULSE_GPIO, 0);
    portYIELD_FROM_ISR(higher_woken);
}

/* ---------- Bottom-half: semaphore path ---------- */
static void btn_task_sem(void *arg)
{
    for (;;) {
        if (xSemaphoreTake(btn_sem, portMAX_DELAY) == pdTRUE) {
            int64_t wake = esp_timer_get_time();
            int64_t lat = wake - isr_entry_time_us;
            latency_last_sem_us = (uint64_t)lat;
            if ((uint64_t)lat > latency_max_sem_us) latency_max_sem_us = (uint64_t)lat;

            radar_hit_count++;
            ESP_LOGI(TAG, "[sem][RADAR] contact #%lu  lat=%lld us (max=%llu)",
                     (unsigned long)presses_observed,
                     (long long)lat,
                     (unsigned long long)latency_max_sem_us);
        }
    }
}

/* ---------- Bottom-half: notification path ---------- */
static void btn_task_notif(void *arg)
{
    for (;;) {
        uint32_t count = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (count == 0) continue;

        int64_t wake = esp_timer_get_time();
        int64_t lat = wake - isr_entry_time_us;
        latency_last_notif_us = (uint64_t)lat;
        if ((uint64_t)lat > latency_max_notif_us) latency_max_notif_us = (uint64_t)lat;

        ESP_LOGI(TAG, "[notif][RADAR] contact #%lu  lat=%lld us (max=%llu) notif_count=%lu",
                 (unsigned long)presses_observed,
                 (long long)lat,
                 (unsigned long long)latency_max_notif_us,
                 (unsigned long)count);
    }
}

/* ============================================================
 *  HTTP handlers
 * ============================================================ */
static esp_err_t handle_state(httpd_req_t *req)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"on\":%s,\"toggles\":%lu,"
        "\"presses\":%lu,\"radar_hits\":%lu,"
        "\"lat_sem_last_us\":%llu,\"lat_sem_max_us\":%llu,"
        "\"lat_notif_last_us\":%llu,\"lat_notif_max_us\":%llu}",
        led_on ? "true" : "false",
        (unsigned long)toggle_count,
        (unsigned long)presses_observed,
        (unsigned long)radar_hit_count,
        (unsigned long long)latency_last_sem_us,
        (unsigned long long)latency_max_sem_us,
        (unsigned long long)latency_last_notif_us,
        (unsigned long long)latency_max_notif_us);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static esp_err_t handle_root(httpd_req_t *req)
{
    static const char html[] =
        "<!DOCTYPE html>"
        "<html lang=\"en\"><head>"
        "<meta charset=\"utf-8\">"
        "<title>Avionics monitor</title>"
        "<style>"
        "  body { font-family: -apple-system, sans-serif; background: #FAFAF5; "
        "         color: #1A1A1A; padding: 2rem; }"
        "  h1 { color: #6B4F09; border-bottom: 3px solid #FFC904; "
        "       display: inline-block; padding-bottom: 4px; }"
        "  .panel { margin-top: 1.5rem; padding: 1rem 1.5rem; border: 1px solid #E5DCC3;"
        "           border-radius: 8px; background: #FFFFFF; }"
        "  .status { font-size: 2.4em; font-weight: 700; margin: 0.5rem 0; "
        "           transition: color 120ms ease; }"
        "  .status.online  { color: #1B6A2E; }"
        "  .status.offline { color: #B81829; }"
        "  .meta { color: #6B4F09; font-variant-numeric: tabular-nums; }"
        "  .dot { display:inline-block; width: 0.6em; height: 0.6em; "
        "         border-radius: 50%; margin-right: 0.4em; "
        "         vertical-align: middle; transition: background 120ms ease; }"
        "  .dot.online  { background: #1B6A2E; }"
        "  .dot.offline { background: #B81829; }"
        "  .flash { transition: background 100ms ease; }"
        "  .flash.hit { background: #FFF3B0; }"
        "  table { border-collapse: collapse; margin-top: 0.5rem; }"
        "  td { padding: 2px 12px 2px 0; }"
        "</style></head>"
        "<body>"
        "<h1>Avionics monitor</h1>"

        "<div class=\"panel\">"
        "<p>UAV-01 Beacon status:</p>"
        "<div id=\"status\" class=\"status off\">"
        "  <span id=\"dot\" class=\"dot off\"></span><span id=\"label\">--</span>"
        "</div>"
        "<p class=\"meta\">Toggles since boot: <span id=\"count\">0</span></p>"
        "</div>"

        "<div class=\"panel flash\" id=\"radarPanel\">"
        "<p>RADAR contact button (GPIO 18) &mdash; bottom-half latency:</p>"
        "<table>"
        "<tr><td>Contacts observed (ISR)</td><td id=\"presses\">0</td></tr>"
        "<tr><td>Contacts serviced (bottom-half)</td><td id=\"hits\">0</td></tr>"
        "<tr><td>Semaphore path latency (last / max)</td>"
        "    <td><span id=\"semLast\">0</span> / <span id=\"semMax\">0</span> &micro;s</td></tr>"
        "<tr><td>Notification path latency (last / max)</td>"
        "    <td><span id=\"notifLast\">0</span> / <span id=\"notifMax\">0</span> &micro;s</td></tr>"
        "</table>"
        "</div>"

        "<p class=\"meta\">Polling at 4 Hz via <code>/state</code> JSON endpoint.</p>"
        "<script>"
        "let lastPresses = 0;"
        "async function poll(){"
        "  try{"
        "    const r = await fetch('/state',{cache:'no-store'});"
        "    const s = await r.json();"
        "    const cls = s.on ? 'Online' : 'Offline';"
        "    document.getElementById('status').className = 'status ' + cls;"
        "    document.getElementById('dot').className = 'dot ' + cls;"
        "    document.getElementById('label').textContent = s.on ? 'Online' : 'Offline';"
        "    document.getElementById('count').textContent = s.toggles;"
        "    document.getElementById('presses').textContent = s.presses;"
        "    document.getElementById('hits').textContent = s.radar_hits;"
        "    document.getElementById('semLast').textContent = s.lat_sem_last_us;"
        "    document.getElementById('semMax').textContent = s.lat_sem_max_us;"
        "    document.getElementById('notifLast').textContent = s.lat_notif_last_us;"
        "    document.getElementById('notifMax').textContent = s.lat_notif_max_us;"
        "    if (s.presses !== lastPresses) {"
        "      lastPresses = s.presses;"
        "      const p = document.getElementById('radarPanel');"
        "      p.classList.add('hit');"
        "      setTimeout(() => p.classList.remove('hit'), 200);"
        "    }"
        "  }catch(e){/* ignore transient network blips */}"
        "}"
        "setInterval(poll, 250);"
        "poll();"
        "</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.core_id = 0;                    /* networking on Core 0 */
    cfg.task_priority = 5;
    cfg.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) == ESP_OK) {
        httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = handle_root, .user_ctx = NULL };
        httpd_register_uri_handler(server, &root);

        httpd_uri_t state = { .uri = "/state", .method = HTTP_GET, .handler = handle_state, .user_ctx = NULL };
        httpd_register_uri_handler(server, &state);

        ESP_LOGI(TAG, "HTTP server started on port %d", HTTP_PORT);
    } else {
        ESP_LOGE(TAG, "HTTP server failed to start");
    }
    return server;
}

/* ---------- Wi-Fi event handler ---------- */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        start_webserver();
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ============================================================
 *  app_main
 * ============================================================ */
void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "==== App1+App3 [Avionics] starting — beacon + RADAR ISR ====");

    /* Wi-Fi + HTTP on Core 0 */
    wifi_init_sta();

    /* Signaling primitives + bottom-half tasks BEFORE the ISR is installed,
     * so task_notif_handle is valid the instant an interrupt can fire. */
    btn_sem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(btn_task_sem,   "btn_sem",   4096, NULL, 12, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(btn_task_notif, "btn_notif", 4096, NULL, 12,
                            &task_notif_handle, APP_CPU_NUM);

    /* Beacon task, Core 1, lower priority than the interrupt response path. */
    xTaskCreatePinnedToCore(blink_task, "blink", 2048, NULL, 5, NULL, APP_CPU_NUM);

    /* Button + scope-pulse GPIO configuration. */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&btn_cfg);

    gpio_config_t pulse_cfg = {
        .pin_bit_mask = 1ULL << ISR_PULSE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pulse_cfg);
    gpio_set_level(ISR_PULSE_GPIO, 0);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);

    ESP_LOGI(TAG, "Press button GPIO %d to simulate a RADAR contact. Scope GPIO %d for ISR timing.",
             BUTTON_GPIO, ISR_PULSE_GPIO);

    /* app_main returns; both cores keep running the tasks we created. */
}
