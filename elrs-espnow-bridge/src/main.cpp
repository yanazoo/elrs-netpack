#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
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
static uint8_t appliedMac[6]  = {0};  // MAC currently applied via esp_wifi_set_mac
static bool    macApplied     = false;

static MSP mspFromS3;      // parse bytes arriving from XIAO over UART
static MSP mspFromEspnow;  // parse bytes arriving from backpack over ESP-NOW

// ── ESP-NOW TX queue with retry ──────────────────────────────────────────────
// esp_now_send は fire-and-forget で、送信失敗（ACK 無し）は捨てられていた。
// レースクロックは毎秒再送されるが、ラップタイムは 1 回きりの送信なので、
// 一瞬の電波干渉や ESP-NOW 再初期化と重なるとそのラップは二度と表示されない。
// → 1 パケットずつ送信し、送信コールバックで失敗を検知したらリトライする。
#define TX_QUEUE_LEN   16
#define TX_MAX_TRIES   3
#define TX_CB_TIMEOUT_MS 50   // コールバック消失時（reinit と重なった等）の保険
#define MSP_MAX_FRAME  (MSP_PORT_INBUF_SIZE + 9)  // $X< + header(5) + payload + crc

struct TxItem {
    uint8_t addr[6];
    uint8_t len;
    uint8_t tries;
    uint8_t data[MSP_MAX_FRAME];
};

static TxItem        txQueue[TX_QUEUE_LEN];
static uint8_t       txHead     = 0;   // next item to send
static uint8_t       txCount    = 0;
static bool          txInFlight = false;
static uint32_t      txSentMs   = 0;
static volatile bool txDone     = false;
static volatile bool txOk       = false;

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
// コールバックのシグネチャは ESP-IDF / Arduino-ESP32 のバージョンで変化した:
//   recv: const uint8_t* mac  →  const esp_now_recv_info_t*  (Arduino 3.x / IDF 5.x)
//   send: const uint8_t* mac  →  const wifi_tx_info_t*       (IDF 5.4+)
// どちらも第1引数（MAC/送信情報）は未使用なので、型だけ合わせる。
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
static void onDataSent(const wifi_tx_info_t * /*info*/, esp_now_send_status_t status)
#else
static void onDataSent(const uint8_t * /*mac*/, esp_now_send_status_t status)
#endif
{
    // Only one packet is ever in flight (see pumpTx), so this result
    // unambiguously belongs to txQueue[txHead].
    txOk   = (status == ESP_NOW_SEND_SUCCESS);
    txDone = true;
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onDataRecv(const esp_now_recv_info_t * /*info*/, const uint8_t *data, int len)
#else
static void onDataRecv(const uint8_t * /*mac*/, const uint8_t *data, int len)
#endif
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

// ── ESP-NOW TX queue ──────────────────────────────────────────────────────────

static bool enqueueTx(const uint8_t *addr, const uint8_t *data, uint8_t len)
{
    if (len > MSP_MAX_FRAME) return false;
    if (txCount >= TX_QUEUE_LEN)
    {
        Serial.println("[espnow] tx queue full, dropping packet");
        return false;
    }
    TxItem &it = txQueue[(txHead + txCount) % TX_QUEUE_LEN];
    memcpy(it.addr, addr, 6);
    memcpy(it.data, data, len);
    it.len   = len;
    it.tries = 0;
    txCount++;
    return true;
}

// loop() 毎回呼ぶ。1 パケットずつ送信し、失敗したら TX_MAX_TRIES まで再送。
static void pumpTx()
{
    if (txInFlight)
    {
        if (!txDone)
        {
            if (millis() - txSentMs < TX_CB_TIMEOUT_MS) return;
            txOk = false;  // callback lost (e.g. reinit while in flight)
        }
        txInFlight = false;
        TxItem &it = txQueue[txHead];
        if (txOk || it.tries >= TX_MAX_TRIES)
        {
            if (!txOk)
                Serial.printf("[espnow] send FAILED after %d tries\n", it.tries);
            txHead = (txHead + 1) % TX_QUEUE_LEN;
            txCount--;
        }
        // else: leave item at head — retried below
    }

    if (txCount == 0) return;

    TxItem &it = txQueue[txHead];
    it.tries++;
    txDone   = false;
    txOk     = false;
    txSentMs = millis();
    if (esp_now_send(it.addr, it.data, it.len) == ESP_OK)
    {
        txInFlight = true;
    }
    else if (it.tries >= TX_MAX_TRIES)
    {
        // Immediate API error (e.g. peer not registered) — give up on this one
        Serial.println("[espnow] send API error, dropping packet");
        txHead = (txHead + 1) % TX_QUEUE_LEN;
        txCount--;
    }
}

// UID 切り替え前にキューを掃く。切り替えで peer/MAC が変わると
// 残っていた前パイロット宛のパケットが送信不能になるため。
static void drainTxQueue(uint32_t timeoutMs)
{
    uint32_t start = millis();
    while (txCount > 0 && millis() - start < timeoutMs)
    {
        pumpTx();
        delay(1);
    }
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
            // 同じ UID なら MAC 変更 + ESP-NOW 再初期化をスキップする。
            // 以前は SET_SEND_UID のたびに再初期化しており（1 ラップで最大 4 回、
            // レースクロックで毎秒）、その間 loop() が止まり UART 溢れや
            // 送信ロストの原因になっていた。
            if (!macApplied || memcmp(sendAddress, appliedMac, 6) != 0)
            {
                drainTxQueue(60);
                // Change this ESP32's WiFi MAC to impersonate the TX backpack
                esp_wifi_set_mac(WIFI_IF_STA, sendAddress);
                reinitEspNow();
                registerPeer(sendAddress);
                memcpy(appliedMac, sendAddress, 6);
                macApplied = true;
                Serial.printf("[uid] set to [%d,%d,%d,%d,%d,%d]\n",
                    sendAddress[0], sendAddress[1], sendAddress[2],
                    sendAddress[3], sendAddress[4], sendAddress[5]);
            }
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
        uint8_t buf[MSP_MAX_FRAME];
        if (size <= MSP_MAX_FRAME && msp.convertToByteArray(pkt, buf))
            enqueueTx(sendAddress, buf, size);
        break;
    }
    }
}

// ── Arduino entry points ──────────────────────────────────────────────────────

void setup()
{
    Serial.begin(115200);
    // RX バッファを拡張（既定 256B）。ESP-NOW 再初期化などで loop() が
    // 一時的に止まっても、XIAO からの OSD バースト（1 ラップ 300B 超）を
    // 取りこぼさないようにする。XIAO 側と同じ対策。
    Serial2.setRxBufferSize(2048);
    Serial2.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    Serial.println("[boot] ESP32 Wrover-E ESP-NOW bridge");

    // WiFi in STA mode but NOT connected to any AP — ESP-NOW only
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_max_tx_power(84);  // 84 * 0.25 = 21 dBm (max for ESP32)
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[wifi] channel fixed to %d\n", ESPNOW_CHANNEL);

    reinitEspNow();
    Serial.println("[boot] ready");
}

void loop()
{
    // UART → ESP-NOW: read MSP bytes from XIAO, parse, queue for ESP-NOW
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

    pumpTx();
}
