#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HardwareSerial.h>
#include "config.h"
#include "msp.h"
#include "msptypes.h"

static WiFiServer tcpServer(TCP_PORT);
static WiFiClient tcpClient;

static MSP mspFromTcp;
static MSP mspFromUart;

static HardwareSerial uart(1);

// ── helpers ───────────────────────────────────────────────────────────────────

static void sendMspToTcp(mspPacket_t *pkt)
{
    if (!tcpClient || !tcpClient.connected()) return;
    MSP msp;
    uint8_t size = msp.getTotalPacketSize(pkt);
    uint8_t buf[size];
    if (msp.convertToByteArray(pkt, buf))
        tcpClient.write(buf, size);
}

static void sendMspToUart(mspPacket_t *pkt)
{
    MSP msp;
    uint8_t size = msp.getTotalPacketSize(pkt);
    uint8_t buf[size];
    if (msp.convertToByteArray(pkt, buf))
        uart.write(buf, size);
}

// ── MSP handlers ──────────────────────────────────────────────────────────────

static void handlePacketFromTcp(mspPacket_t *pkt)
{
    if (pkt->function == MSP_ELRS_GET_BACKPACK_VERSION)
    {
        // Respond locally with a stub version so RotorHazard handshake succeeds
        mspPacket_t resp;
        resp.reset();
        resp.makeResponse();
        resp.function    = MSP_ELRS_GET_BACKPACK_VERSION;
        resp.payloadSize = 4;
        resp.payload[0]  = 0;  // major
        resp.payload[1]  = 1;  // minor
        resp.payload[2]  = 0;  // patch
        resp.payload[3]  = 0;  // extra
        sendMspToTcp(&resp);
        return;
    }
    // Forward everything else to the ESP32 Wrover-E via UART
    sendMspToUart(pkt);
}

static void handlePacketFromUart(mspPacket_t *pkt)
{
    // Forward MSP responses/telemetry from backpack to TCP client
    sendMspToTcp(pkt);
}

// ── WiFi / mDNS setup ─────────────────────────────────────────────────────────

static void wifiConnect()
{
    Serial.printf("[wifi] connecting to %s\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000)
    {
        delay(500);
        Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED)
        Serial.printf("[wifi] connected, IP=%s\n", WiFi.localIP().toString().c_str());
    else
        Serial.println("[wifi] connection failed, will retry");
}

// ── Arduino entry points ──────────────────────────────────────────────────────

void setup()
{
    Serial.begin(115200);
    Serial.println("[boot] XIAO ESP32-S3 WiFi bridge");

    uart.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

    wifiConnect();

    if (WiFi.status() == WL_CONNECTED)
    {
        if (MDNS.begin(MDNS_HOSTNAME))
            Serial.printf("[mdns] %s.local\n", MDNS_HOSTNAME);
        MDNS.addService("_elrs", "_tcp", TCP_PORT);

        tcpServer.begin();
        Serial.printf("[tcp] listening on port %d\n", TCP_PORT);
    }

    Serial.println("[boot] ready");
}

void loop()
{
    // Reconnect WiFi if dropped
    if (WiFi.status() != WL_CONNECTED)
    {
        wifiConnect();
        return;
    }

    // Accept new TCP client if none connected
    if (!tcpClient || !tcpClient.connected())
    {
        WiFiClient c = tcpServer.available();
        if (c)
        {
            tcpClient = c;
            Serial.println("[tcp] client connected");
        }
    }

    // TCP → UART: read MSP from RotorHazard, forward to ESP32
    if (tcpClient && tcpClient.connected())
    {
        while (tcpClient.available())
        {
            uint8_t b = tcpClient.read();
            if (mspFromTcp.processReceivedByte(b))
            {
                mspPacket_t *pkt = mspFromTcp.getReceivedPacket();
                handlePacketFromTcp(pkt);
                mspFromTcp.markPacketReceived();
            }
        }
    }

    // UART → TCP: read MSP from ESP32 Wrover-E, forward to RotorHazard
    while (uart.available())
    {
        uint8_t b = uart.read();
        if (mspFromUart.processReceivedByte(b))
        {
            mspPacket_t *pkt = mspFromUart.getReceivedPacket();
            handlePacketFromUart(pkt);
            mspFromUart.markPacketReceived();
        }
    }
}
