// UART bridge: ESP32-S3 handles WiFi/TCP; ESP-NOW is delegated to an external
// ESP32 Wrover-E connected via UART (XIAO D0/D1 ↔ ESP32 GPIO26/27).
#include <stdio.h>
#include <cstring>
#include "espnow_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_app_desc.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "tasks.h"
#include "msptypes.h"
#include "msp.h"

#define UART_PORT_NUM   UART_NUM_1
#define UART_BAUD_RATE  115200
#define UART_TX_PIN     GPIO_NUM_1   // XIAO D0 → ESP32 GPIO26
#define UART_RX_PIN     GPIO_NUM_2   // XIAO D1 ← ESP32 GPIO27
#define UART_BUF_SIZE   512

static const char *TAG = "uart_bridge";

void runESPNOWServer(void *pvParameters)
{
    TaskBufferParams *buffers = (TaskBufferParams *)pvParameters;
    const esp_app_desc_t *description = esp_app_get_description();

    // UART to ESP32 Wrover-E (ESP-NOW bridge)
    uart_config_t uart_config = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART bridge ready (TX=GPIO%d RX=GPIO%d)", UART_TX_PIN, UART_RX_PIN);

    // WiFi init — same STA setup as before (needed for TCP server)
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_config_t sta_config = {};
    strncpy((char *)sta_config.sta.ssid,     CONFIG_TCP_WIFI_SSID,     sizeof(sta_config.sta.ssid));
    strncpy((char *)sta_config.sta.password, CONFIG_TCP_WIFI_PASSWORD, sizeof(sta_config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "Connecting to AP: %s", CONFIG_TCP_WIFI_SSID);

    MSP msp_tx;  // TCP → UART serialization
    MSP msp_rx;  // UART → TCP parsing

    while (1)
    {
        // TCP → UART: forward MSP packets from RotorHazard to ESP32
        size_t item_size;
        mspPacket_t *packet = (mspPacket_t *)xRingbufferReceive(buffers->read, &item_size, 0);
        if (packet != NULL)
        {
            if (packet->function == MSP_ELRS_GET_BACKPACK_VERSION)
            {
                // Handle locally — return this firmware's version string
                mspPacket_t out;
                out.reset();
                out.makeResponse();
                out.function = MSP_ELRS_GET_BACKPACK_VERSION;
                for (size_t i = 0; i < sizeof(description->version); i++)
                    out.addByte(description->version[i]);
                if (xRingbufferSend(buffers->write, &out, sizeof(mspPacket_t),
                                    pdMS_TO_TICKS(1000)) != pdTRUE)
                    ESP_LOGE(TAG, "Failed to queue version response");
            }
            else
            {
                // Forward raw MSP bytes to ESP32 over UART
                uint8_t packetSize = msp_tx.getTotalPacketSize(packet);
                uint8_t bytes[packetSize];
                if (msp_tx.convertToByteArray(packet, bytes))
                {
                    uart_write_bytes(UART_PORT_NUM, bytes, packetSize);
                    ESP_LOGI(TAG, "→ UART MSP 0x%04X (%d B)", packet->function, packetSize);
                }
            }
            vRingbufferReturnItem(buffers->read, (void *)packet);
        }

        // UART → TCP: receive MSP bytes from ESP32, parse, queue to TCP server
        uint8_t data[128];
        int len = uart_read_bytes(UART_PORT_NUM, data, sizeof(data), 0);
        for (int i = 0; i < len; i++)
        {
            if (msp_rx.processReceivedByte(data[i]))
            {
                mspPacket_t *pkt = msp_rx.getReceivedPacket();
                if (xRingbufferSend(buffers->write, pkt, sizeof(mspPacket_t),
                                    pdMS_TO_TICKS(1000)) != pdTRUE)
                    ESP_LOGE(TAG, "Failed to queue UART→TCP packet");
                msp_rx.markPacketReceived();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
