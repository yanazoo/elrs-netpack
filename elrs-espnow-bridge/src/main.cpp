#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "msp.h"
#include "msptypes.h"

// UART to ESP32-S3 XIAO
// Wire: ESP32 GPIO26(RX) ← XIAO D0(GPIO1/TX)
//       ESP32 GPIO27(TX) → XIAO D1(GPIO2/RX)
//       GND              ↔ GND
#define UART_RX_PIN  26
#define UART_TX_PIN  27
#define UART_BAUD    115200

#ifndef ESPNOW_CHANNEL
#define ESPNOW_CHANNEL 1
#endif

static uint8_t sendAddress[6] = {0};
static uint8_t bindAddress[6] = {0};

static MSP mspFromS3;      // parse bytes arriving from XIAO over UART
static MSP mspFromEspnow;  // parse bytes arriving from backpack over ESP-NOW

// ── helpers ──────────────────────────────────────────────────────────────────

static bool isNonZero(const uint8_t *addr)
{
    for (int i = 0; i < 6; i++) if (addr[i]) return true;
    return false;
}

static void registerPeer(const uint8_t *addr)
{
    esp_now_del_peer(addr);
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, addr, 6);
    p.channel = ESPNOW_CHANNEL;
    p.encrypt  = false;
    if (esp_now_add_peer(&p) == ESP_OK)
        Serial.printf("[espnow] peer registered [%d,%d,%d,%d,%d,%d]\n",
            addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    else
        Serial.println("[espnow] failed to add peer");
}

static void reinitEspNow();

// ── ESP-NOW callbacks ─────────────────────────────────────────────────────────

static void onDataSent(const uint8_t *mac, esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS)
        Serial.println("[espnow] send FAILED");
}

static void onDataRecv(const uint8_t *mac, const uint8_t *data, int len)
{
    // Forward raw bytes to XIAO; it will parse them as MSP
    Serial2.write(data, len);
}

// ── ESP-NOW init ──────────────────────────────────────────────────────────────

static void reinitEspNow()
{
    esp_now_deinit();
    if (esp_now_init() != ESP_OK) { Serial.println("[espnow] init failed"); return; }
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataRecv);
    Serial.println("[espnow] (re)initialized");
}

// ── MSP packet handler (packets received from XIAO via UART) ─────────────────

static void handlePacketFromS3(mspPacket_t *pkt)
{
    switch (pkt->function)
    {
    case MSP_ELRS_SET_SEND_UID:
    {
        uint8_t mode = pkt->readByte();
        memset(sendAddress, 0, 6);
        if (mode == 0x01)
        {
            for (int i = 0; i < 6; i++) sendAddress[i] = pkt->readByte();
            sendAddress[0] &= ~0x01;  // clear multicast bit
        }
        else
        {
            memcpy(sendAddress, bindAddress, 6);
        }

        if (isNonZero(sendAddress))
        {
            // Change this ESP32's WiFi MAC to impersonate the TX backpack
            esp_wifi_set_mac(WIFI_IF_STA, sendAddress);
            reinitEspNow();
            registerPeer(sendAddress);
            Serial.printf("[uid] set to [%d,%d,%d,%d,%d,%d]\n",
                sendAddress[0], sendAddress[1], sendAddress[2],
                sendAddress[3], sendAddress[4], sendAddress[5]);
        }
        break;
    }
    default:
    {
        // Forward all other MSP packets via ESP-NOW to the backpack
        if (!isNonZero(sendAddress))
        {
            Serial.printf("[espnow] no peer yet, drop MSP 0x%04X\n", pkt->function);
            break;
        }
        MSP msp;
        uint8_t size = msp.getTotalPacketSize(pkt);
        uint8_t buf[size];
        if (msp.convertToByteArray(pkt, buf))
        {
            esp_err_t err = esp_now_send(sendAddress, buf, size);
            if (err != ESP_OK)
                Serial.printf("[espnow] send error 0x%X for MSP 0x%04X\n", err, pkt->function);
            else
                Serial.printf("[espnow] sent MSP 0x%04X (%d B)\n", pkt->function, size);
        }
        break;
    }
    }
}

// ── Arduino entry points ──────────────────────────────────────────────────────

void setup()
{
    Serial.begin(115200);
    Serial2.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    Serial.println("[boot] ESP32 Wrover-E ESP-NOW bridge");

    // WiFi in STA mode but NOT connected to any AP — ESP-NOW only
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[wifi] channel fixed to %d\n", ESPNOW_CHANNEL);

    reinitEspNow();
    Serial.println("[boot] ready");
}

void loop()
{
    // UART → ESP-NOW: read MSP bytes from XIAO, parse, send via ESP-NOW
    while (Serial2.available())
    {
        uint8_t b = Serial2.read();
        if (mspFromS3.processReceivedByte(b))
        {
            mspPacket_t *pkt = mspFromS3.getReceivedPacket();
            handlePacketFromS3(pkt);
            mspFromS3.markPacketReceived();
        }
    }
}
