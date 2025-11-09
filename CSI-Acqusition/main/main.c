#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <unistd.h>
#include <arpa/inet.h>
#include "ping/ping_sock.h"

#define LEN_MAC_ADDR 20
#define PING_INTERVAL_MS 50
#define DEVICE_NAME "PROTO/ESP/1"
struct timeval tv;
static const char *TAG = "CSI_MQTT";
static esp_mqtt_client_handle_t mqtt_client;
static bool mqtt_connected = false;
static bool wifi_connected = false;
static bool mqtt_status = true;
static char payload[1024];
static const uint8_t target_mac[6] = {0x??, 0x??, 0x??, 0x??, 0x??, 0x??};
static wifi_csi_info_t latest_csi_info;
static int number = 0;
static const char *PUBLISHER = DEVICE_NAME;
unsigned long startTimestamp, currentTimestamp;

typedef struct {
    unsigned frame_ctrl:16;
    unsigned duration_id:16;
    uint8_t addr1[6];
    uint8_t addr2[6];
    uint8_t addr3[6];
    unsigned seq_ctrl:16;
    uint8_t addr4[6];
} wifi_ieee80211_mac_hdr_t;

typedef struct {
    wifi_ieee80211_mac_hdr_t hdr;
    uint8_t payload[0];
} wifi_ieee80211_packet_t;

void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronized");
}

// NTP 초기화 함수
void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "kr.pool.ntp.org");
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
}

void obtain_time(void)
{
    initialize_sntp();
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 30;

    while (timeinfo.tm_year < (2023 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    if (timeinfo.tm_year < (2023 - 1900)) {
        ESP_LOGE(TAG, "Failed to synchronize time");
    } else {
        ESP_LOGI(TAG, "Time synchronization successful");
    }
    setenv("TZ", "KST-9", 1);
    tzset();
}


static void mqtt_event_handler(void *handler_args, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        char payload_mqtt[128];
        snprintf(payload_mqtt, sizeof(payload_mqtt), "%.*s", event->data_len, event->data);

        char *received_pub = strtok(payload_mqtt, "/");
        char *received_status = strtok(NULL, "/");

        if (received_pub && strcmp(received_pub, PUBLISHER) == 0) {
            if (received_status && strcmp(received_status, "On") == 0) {
                // mqtt_status = true;
                ESP_LOGI(TAG, "MQTT Status: ON");
                char msg[32] = {0,};
                strcpy(msg, "Message processed");
                int len = strlen(msg);
                msg[len] = '\0';
            } else if (received_status && strcmp(received_status, "Off") == 0) {
                // mqtt_status = false;
                ESP_LOGI(TAG, "MQTT Status: OFF");
                char msg[32] = {0,};
                strcpy(msg, "Message processed");
                int len = strlen(msg);
                msg[len] = '\0';
            } else {
                ESP_LOGW(TAG, "Unknown status received: %s", received_status);
            }
        } else {
            ESP_LOGW(TAG, "PUB does not match: %s", received_pub);
        }
        break;
    default:
        // ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void wifi_csi_cb(void *ctx, wifi_csi_info_t *info) {
    if (!info || info->len == 0) return;

    const wifi_ieee80211_packet_t *packet = (wifi_ieee80211_packet_t *)info->buf;
    int frame_ctrl = packet->hdr.frame_ctrl;

    // 프레임 타입과 서브타입 추출
    int frame_type = (frame_ctrl & 0x000C) >> 2;     // 프레임 타입 (2비트)
    int frame_subtype = (frame_ctrl & 0x00F0) >> 4;  // 프레임 서브타입 (4비트)

    // === Frequency measurement logic ===
    static int packet_count = 0;
    static int64_t start_time_us = 0; // Use int64_t for microsecond timestamp

    int64_t now_us = esp_timer_get_time(); // Get current time in microseconds

    // Initialize first timestamp
    if (start_time_us == 0) {
        start_time_us = now_us;
    }

    // Count only matched frames
    //if (frame_type == 1 && frame_subtype == 6 && memcmp(info->mac, target_mac, sizeof(target_mac)) == 0) {
    if (memcmp(info->mac, target_mac, sizeof(target_mac)) == 0) {

        packet_count++;

        // Calculate elapsed time in microseconds
        int64_t elapsed_us = now_us - start_time_us;

        // Report frequency every 1 second (1,000,000 microseconds)
        if (elapsed_us >= 1000000) { 
            // Calculate frequency (packets per second)
            float frequency = (float)packet_count / (elapsed_us / 1000000.0f);

            ESP_LOGI(TAG, "CSI Packet Frequency: %.2f packets/sec | Frame Type: %d, Subtype: %d",
                     frequency, frame_type, frame_subtype);
            
            // Reset timer and counter for the next interval
            start_time_us = now_us;
            packet_count = 0;
        }

        // Log frame type and subtype (consider moving this out of ISR if performance is critical)
        // This part will print for every matched packet, which can be frequent.
        switch (frame_type) {
            case 0:
                //ESP_LOGD(TAG, "Frame Type: Management, Subtype: %d", frame_subtype); // Changed to DEBUG log
                break;
            case 1:
                //ESP_LOGD(TAG, "Frame Type: Control, Subtype: %d", frame_subtype); // Changed to DEBUG log
                break;
            case 2:
                //ESP_LOGD(TAG, "Frame Type: Data, Subtype: %d", frame_subtype); // Changed to DEBUG log
                break;
            default:
                //ESP_LOGD(TAG, "Frame Type: Unknown, Subtype: %d", frame_subtype); // Changed to DEBUG log
                break;
        }

        // Save CSI data (assuming latest_csi_info is global and intended to be updated frequently)
        // If this struct is used by another task, you would need a mutex here as discussed previously.
        // For this self-contained callback, it's fine.
        latest_csi_info = *info;
    }
}



static void csi_task(void *pvParameters) {
    // esp_ping_handle_t ping_handle = setup_ping_session();
    startTimestamp = tv.tv_sec * 1000 + tv.tv_usec / 1000;
    
    while (true) {
        if (!mqtt_connected) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        memset(payload, 0, sizeof(payload));
        gettimeofday(&tv, NULL);
        time_t nowtime = tv.tv_sec;
        struct tm *nowtm = localtime(&nowtime);
        currentTimestamp = tv.tv_sec * 1000 + tv.tv_usec / 1000;

        int year = (nowtm->tm_year + 1900) % 100;
        int month = nowtm->tm_mon + 1;
        int day = nowtm->tm_mday;
        int hour = nowtm->tm_hour;
        int minute = nowtm->tm_min;
        int second = nowtm->tm_sec;
        int milliseconds = tv.tv_usec / 1000;

        snprintf(timestamp, sizeof(timestamp), "%02d%02d%02d%02d%02d%02d%03d", ...);
        int len = snprintf(payload, sizeof(payload),
        "CSI data: mac=" MACSTR ", ... , len=%d, time=%s\nCSI values: ",
        MAC2STR(latest_csi_info.mac), latest_csi_info.len, timestamp);
        for (int i = 4; i < latest_csi_info.len; ++i) {
            len += snprintf(payload + len, sizeof(payload) - len, "%d ", latest_csi_info.buf[i]);
        }
        esp_mqtt_client_publish(mqtt_client, PUBLISHER, payload, len, 1, 0);

        char timestamp[64];
        snprintf(timestamp, sizeof(timestamp), "%02d%02d%02d%02d%02d%02d%03d",
                 year, month, day, hour, minute, second, milliseconds);

        
        int len = snprintf(payload, sizeof(payload),
                        "CSI data: mac=" MACSTR ", number=%d, rssi=%d, channel=%d, rate=%d, sig_mode=%d, mcs=%d, bandwidth=%d, smoothing=%d, not_sounding=%d, aggregation=%d, stbc=%d, fec_coding=%d, sgi=%d, leng=%d, time=%s\nCSI values: ",
                        MAC2STR(latest_csi_info.mac), number, latest_csi_info.rx_ctrl.rssi, latest_csi_info.rx_ctrl.channel, latest_csi_info.rx_ctrl.rate, latest_csi_info.rx_ctrl.sig_mode, latest_csi_info.rx_ctrl.mcs, latest_csi_info.rx_ctrl.cwb,
                        latest_csi_info.rx_ctrl.smoothing, latest_csi_info.rx_ctrl.not_sounding, latest_csi_info.rx_ctrl.aggregation, latest_csi_info.rx_ctrl.stbc, latest_csi_info.rx_ctrl.fec_coding, latest_csi_info.rx_ctrl.sgi, latest_csi_info.len, timestamp);

        for (int i = 4; i < latest_csi_info.len; ++i) {
            len += snprintf(payload + len, sizeof(payload) - len, "%d ", latest_csi_info.buf[i]);
        }
        
        if(mqtt_status)
            esp_mqtt_client_publish(mqtt_client, PUBLISHER, payload, len, 1, 0);
        

        vTaskDelay(pdMS_TO_TICKS(25));
        // printf("%s\n", payload);
    }
    // esp_ping_stop(ping_handle);
    // esp_ping_delete_session(ping_handle);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        ESP_LOGI(TAG, "Disconnected from the AP, retrying...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "Connected to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip_str[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "Got IP: %s", ip_str);
        wifi_connected = true;
        wifi_ap_record_t ap_info;
        esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Connected to AP: %s", ap_info.ssid);
            ESP_LOGI(TAG, "AP MAC Address: " MACSTR, MAC2STR(ap_info.bssid));
        } else {
            ESP_LOGE(TAG, "Failed to get AP info: %s", esp_err_to_name(err));
        }
    }
}


static void wifi_init(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_netif_create_default_wifi_sta();

    wifi_config_t sta_config = {
        .sta = {
            .ssid = "???",
            .password = "???",
        },
    };

    //ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_csi_config_t csi_config = {
        .lltf_en = 1,
        .htltf_en = 0,
        .stbc_htltf2_en = 0,
        .ltf_merge_en = 0,
        .channel_filter_en = 0,
        .manu_scale = 0,
        .shift = 0,
    };

    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(wifi_csi_cb, NULL));
}

static void mqtt_app_start(void) {
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt:??",
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);

    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    
    while (true) {
        esp_err_t err = esp_mqtt_client_start(mqtt_client);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Successfully connected to MQTT broker");
            mqtt_connected = true;
            break;
        } else {
            ESP_LOGE(TAG, "Failed to connect to MQTT broker, retrying...");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

void app_main(void) {
    wifi_init();
    obtain_time();
    while (!wifi_connected) {
        ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    mqtt_app_start();
    
    xTaskCreate(csi_task, "csi_task", 4096, NULL, 5, NULL);
}
