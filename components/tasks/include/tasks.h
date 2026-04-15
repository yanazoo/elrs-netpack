#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "esp_netif.h"

struct TaskBufferParams {
    RingbufHandle_t write;
    RingbufHandle_t read;
    esp_netif_t    *netif;  // STA netif for mDNS (tcp_server); NULL for espnow_server
};
