
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "espnow_server.h"
#include "tcp_server.h"
#include "tasks.h"
#include "msp.h"

#ifdef CONFIG_XIAO_EXTERNAL_ANTENNA
#include "driver/gpio.h"
#endif

TaskHandle_t tcpTaskHandle = NULL;
TaskHandle_t espnowTaskHandle = NULL;

RingbufHandle_t xRingReceivedSocket, xRingReceivedEspnow;

TaskBufferParams espnow_params, tcp_server_params;

extern "C" void app_main(void)
{
    // Create the buffers used for passing data across the different interfaces
    xRingReceivedSocket = xRingbufferCreateNoSplit(sizeof(mspPacket_t), 1000);
    xRingReceivedEspnow = xRingbufferCreateNoSplit(sizeof(mspPacket_t), 50);

    // Create default event loop that running in background
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize TCP/IP network interface aka the esp-netif (should be called only once in application)
    ESP_ERROR_CHECK(esp_netif_init());

#ifdef CONFIG_XIAO_EXTERNAL_ANTENNA
    // Drive GPIO14 HIGH to activate the external U.FL antenna on XIAO ESP32-S3 Sense
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << 14),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)14, 1));
#endif

    // Use both cores of the ESP32 for handling interfaces.
    // Assign ESPNOW to Core 0
    espnow_params = (TaskBufferParams){
        .write = xRingReceivedEspnow,
        .read = xRingReceivedSocket};
    xTaskCreatePinnedToCore(runESPNOWServer, "ESPNOWTask", 4096, (void *)&espnow_params, 10, &espnowTaskHandle, 0);

    // Assign TCP Socket server to Core 1
    tcp_server_params = (TaskBufferParams){
        .write = xRingReceivedSocket,
        .read = xRingReceivedEspnow};
    xTaskCreatePinnedToCore(run_tcp_server, "SocketManagerTask", 4096, (void *)&tcp_server_params, 10, &tcpTaskHandle, 1);
}
