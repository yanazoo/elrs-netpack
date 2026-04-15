
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "espnow_server.h"
#include "tcp_server.h"
#include "tasks.h"
#include "msp.h"

#ifdef CONFIG_XIAO_EXTERNAL_ANTENNA
#include "driver/gpio.h"
#endif

static const char *TAG = "main";

TaskHandle_t tcpTaskHandle = NULL;
TaskHandle_t espnowTaskHandle = NULL;

RingbufHandle_t xRingReceivedSocket, xRingReceivedEspnow;

TaskBufferParams espnow_params, tcp_server_params;

extern "C" void app_main(void)
{
    // Create the buffers used for passing data across the different interfaces
    xRingReceivedSocket = xRingbufferCreateNoSplit(sizeof(mspPacket_t), 1000);
    xRingReceivedEspnow = xRingbufferCreateNoSplit(sizeof(mspPacket_t), 50);

    // Create default event loop running in background
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize TCP/IP network interface
    ESP_ERROR_CHECK(esp_netif_init());

#ifdef CONFIG_XIAO_EXTERNAL_ANTENNA
    // Enable external U.FL antenna on XIAO ESP32-S3 Sense (GPIO14 HIGH)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << 14),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)14, 1));
    ESP_LOGI(TAG, "External antenna enabled (GPIO14 HIGH)");
#endif

    // Create default WiFi STA netif
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    // Initialize WiFi driver in STA mode.
    // espnow_server task will set the MAC address and call esp_wifi_connect().
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi driver started (STA mode)");

    // Assign ESPNOW task to Core 0.
    // It reads the backpack UID from NVS, sets the STA MAC, initialises
    // ESP-NOW, and then connects to the configured WiFi AP.
    espnow_params = (TaskBufferParams){
        .write = xRingReceivedEspnow,
        .read  = xRingReceivedSocket,
        .netif = NULL};
    xTaskCreatePinnedToCore(runESPNOWServer, "ESPNOWTask", 4096,
                            (void *)&espnow_params, 10, &espnowTaskHandle, 0);

    // Assign TCP socket server to Core 1.
    // It registers for the STA-got-IP event to start mDNS, then opens
    // the TCP listening socket.
    tcp_server_params = (TaskBufferParams){
        .write = xRingReceivedSocket,
        .read  = xRingReceivedEspnow,
        .netif = sta_netif};
    xTaskCreatePinnedToCore(run_tcp_server, "SocketManagerTask", 4096,
                            (void *)&tcp_server_params, 10, &tcpTaskHandle, 1);
}
