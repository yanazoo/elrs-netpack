#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include "msp.h"
#include "msptypes.h"

// UART to the host (elrs-netpack's XIAO ESP32-S3, or a lap timer).
// Defaults are the ESP32 Wrover-E wiring; other boards set these in
// platformio.ini (env:xiao_esp32c3: Serial1 on D7/GPIO20 RX, D6/GPIO21 TX).
// Wire: bridge RX ← host TX
//       bridge TX → host RX
//       GND       ↔ GND
#ifndef UART_RX_PIN
#define UART_RX_PIN  26
#endif
#ifndef UART_TX_PIN
#define UART_TX_PIN  27
#endif
#ifndef BRIDGE_SERIAL
#define BRIDGE_SERIAL Serial2
#endif
#define UART_BAUD    115200

// Per-packet / per-UID-switch logging. Off by default: a lap timer switches
// UID several times a second (one burst per pilot for the race clock), and
// printing three lines each time held up loop() and made the OSD late.
// Build with -DBRIDGE_VERBOSE to see them.
#ifdef BRIDGE_VERBOSE
#define VLOG(...) Serial.printf(__VA_ARGS__)
#else
#define VLOG(...) do {} while (0)
#endif

#ifndef ESPNOW_CHANNEL
#define ESPNOW_CHANNEL 1
#endif

static uint8_t sendAddress[6] = {0};
static uint8_t bindAddress[6] = {0};
static uint8_t appliedMac[6]  = {0};  // MAC currently applied via esp_wifi_set_mac
static bool    macApplied     = false;

static MSP mspFromS3;      // parse bytes arriving from XIAO over UART
static MSP mspFromEspnow;  // parse bytes arriving from backpack over ESP-NOW

// ── Sending: connectionless, every burst three times ────────────────────────
// Goggles' backpacks (ELRS VRx) accept an OSD packet when it comes from
// their bind-phrase UID. So this ESP32 takes the pilot's UID as its MAC and
// sends to the broadcast address: no ACK, no waiting, no retries (like UDP).
// Before, packets were sent to the UID itself and every one waited for the
// goggles' ACK and was retried: goggles that were off (up to 8 of them at a
// field) held up everyone else's OSD, and two goggles on one bind phrase
// ACKed at once, collided and looked like a failure.
// Instead each burst (what the host sends for one pilot: SET_SEND_UID, the
// OSD packets, display) goes out SEND_REPEATS times in a row, a few ms
// apart: one lost packet (10 % loss -> 0.1 % with three copies) is covered
// by the next copy; the host's own timed repeats cover longer dropouts.
// Every packet is idempotent (write a row, display, clear), so goggles that
// get two or three copies show the same.
#ifndef SEND_REPEATS
#define SEND_REPEATS   3
#endif
// Copies of one packet at least this far apart. ELRS 2.4 GHz TXs (one per
// pilot, right next to the goggles) hop to a new frequency every packet,
// every few ms, and about a third of the hops fall in ESP-NOW's channel 1:
// a copy a few ms later meets a different hop. A full burst takes longer
// than this anyway; a short one (a race-clock tick: row + display) waits.
#define COPY_GAP_US    3000
#define BURST_MAX      32    // packets in one burst (a full screen is ~20)
#define BURST_IDLE_MS  20    // a host that doesn't end its bursts: send after this quiet
#define SEND_CB_TIMEOUT_MS 10
#define MSP_MAX_FRAME  (MSP_PORT_INBUF_SIZE + 9)  // $X< + header(5) + payload + crc

static const uint8_t broadcastAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── Listen filter (set by the host) ──────────────────────────────────────────
// Everything this ESP32 hears on channel 1 used to go to the host: the race
// director's switch, but also every other bridge's broadcast OSD and other
// backpacks' traffic. A lap timer sends MSP_BRIDGE_SET_LISTEN with the race
// director's UID: from then on only frames *from* that UID are passed on
// (the RD's TX backpack sends from its UID), so a pilot's own DVR switch,
// heard while the bridge is on that pilot's UID, can't start the race, and
// nothing else floods the UART / TCP link. The bridge answers so the host
// knows the filter is on. Size 0 clears it (forward everything, as before,
// for hosts that never send it, e.g. RotorHazard's vrxc_elrs).
#define MSP_BRIDGE_SET_LISTEN 0x0F01
static bool    listenSet = false;
static uint8_t listenUid[6];

// Frames heard over ESP-NOW wait here for loop() to pass them to the host:
// the receive callback runs in the WiFi task and must not block on the UART.
#define RX_QUEUE_LEN 8
struct RxItem {
    uint8_t len;
    uint8_t data[250];
};
static RxItem            rxQueue[RX_QUEUE_LEN];
static volatile uint8_t  rxHead = 0, rxTail = 0;   // written by loop / the callback
static volatile uint32_t rxDropped = 0;

static bool sameUid(const uint8_t *a, const uint8_t *b)
{
    return (a[0] & ~0x01) == (b[0] & ~0x01) && memcmp(a + 1, b + 1, 5) == 0;
}

struct BurstPkt {
    uint8_t len;
    uint8_t data[MSP_MAX_FRAME];
};
static BurstPkt      burst[BURST_MAX];
static uint8_t       burstLen    = 0;
static uint32_t      burstLastMs = 0;
static volatile bool txDone      = false;

// ── helpers ──────────────────────────────────────────────────────────────────

static bool isNonZero(const uint8_t *addr)
{
    for (int i = 0; i < 6; i++) if (addr[i]) return true;
    return false;
}

static void registerBroadcastPeer()
{
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, broadcastAddress, 6);
    p.channel = ESPNOW_CHANNEL;
    p.encrypt = false;
    if (esp_now_add_peer(&p) != ESP_OK) Serial.println("[espnow] failed to add broadcast peer");
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
    // Broadcast: no ACK, so this only says the packet has left the radio.
    (void)status;
    txDone = true;
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    const uint8_t *mac = info->src_addr;
#else
static void onDataRecv(const uint8_t *mac, const uint8_t *data, int len)
{
#endif
    if (listenSet && !sameUid(mac, listenUid)) return;   // not the race director
    if (len <= 0 || len > (int)sizeof(rxQueue[0].data)) return;
    uint8_t next = (rxTail + 1) % RX_QUEUE_LEN;
    if (next == rxHead) { rxDropped = rxDropped + 1; return; }          // host link backed up
    rxQueue[rxTail].len = (uint8_t)len;
    memcpy(rxQueue[rxTail].data, data, len);
    rxTail = next;
}

// loop(): pass what was heard to the host.
static void forwardRx()
{
    while (rxHead != rxTail)
    {
        BRIDGE_SERIAL.write(rxQueue[rxHead].data, rxQueue[rxHead].len);
        rxHead = (rxHead + 1) % RX_QUEUE_LEN;
    }
}

// ── ESP-NOW init ──────────────────────────────────────────────────────────────

static void reinitEspNow()
{
    esp_now_deinit();
    if (esp_now_init() != ESP_OK) { Serial.println("[espnow] init failed"); return; }
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataRecv);
    registerBroadcastPeer();
    VLOG("[espnow] (re)initialized\n");
}

// ── Bursts ───────────────────────────────────────────────────────────────────

static void sendOne(const BurstPkt &p)
{
    txDone = false;
    if (esp_now_send(broadcastAddress, p.data, p.len) != ESP_OK) return;   // queue full: next copy
    uint32_t start = millis();
    while (!txDone && millis() - start < SEND_CB_TIMEOUT_MS) delayMicroseconds(100);
}

// Send the collected burst SEND_REPEATS times (under the MAC it was collected
// for) and start a new one.
static void flushBurst()
{
    if (burstLen == 0) return;
    for (int r = 0; r < SEND_REPEATS; r++)
    {
        uint32_t start = micros();
        for (uint8_t i = 0; i < burstLen; i++) sendOne(burst[i]);
        uint32_t took = micros() - start;
        if (r + 1 < SEND_REPEATS && took < COPY_GAP_US) delayMicroseconds(COPY_GAP_US - took);
    }
    VLOG("[espnow] burst of %u sent x%d\n", burstLen, SEND_REPEATS);
    burstLen = 0;
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

        // The previous pilot's burst is complete: send it with its MAC.
        flushBurst();
        if (isNonZero(sendAddress))
        {
            // 同じ UID なら MAC 変更 + ESP-NOW 再初期化をスキップする（毎秒の
            // レースクロックで UID が変わらないときに loop() を止めないため）。
            if (!macApplied || memcmp(sendAddress, appliedMac, 6) != 0)
            {
                // Take the pilot's UID as this ESP32's MAC: the goggles only
                // accept packets from it (and the race director's switch is
                // sent to it, so the bridge hears it while on the RD's UID).
                esp_wifi_set_mac(WIFI_IF_STA, sendAddress);
                reinitEspNow();
                memcpy(appliedMac, sendAddress, 6);
                macApplied = true;
                VLOG("[uid] set to [%d,%d,%d,%d,%d,%d]\n",
                    sendAddress[0], sendAddress[1], sendAddress[2],
                    sendAddress[3], sendAddress[4], sendAddress[5]);
            }
        }
        break;
    }
    case MSP_BRIDGE_SET_LISTEN:
    {
        flushBurst();
        listenSet = pkt->payloadSize >= 6;
        for (int i = 0; i < 6 && listenSet; i++) listenUid[i] = pkt->readByte();
        // Answer, so the host knows the filter is on (payload: 1 = on).
        mspPacket_t resp;
        resp.reset();
        resp.makeResponse();
        resp.function = MSP_BRIDGE_SET_LISTEN;
        resp.addByte(listenSet ? 1 : 0);
        MSP msp;
        uint8_t buf[MSP_MAX_FRAME];
        uint8_t size = msp.getTotalPacketSize(&resp);
        if (size <= MSP_MAX_FRAME && msp.convertToByteArray(&resp, buf)) BRIDGE_SERIAL.write(buf, size);
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
        if (size > MSP_MAX_FRAME) break;
        if (burstLen >= BURST_MAX) flushBurst();   // very long burst: send what we have
        BurstPkt &bp = burst[burstLen];
        if (!msp.convertToByteArray(pkt, bp.data)) break;
        bp.len = size;
        burstLen++;
        burstLastMs = millis();
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
    BRIDGE_SERIAL.setRxBufferSize(4096);
    BRIDGE_SERIAL.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    Serial.printf("[boot] ESP-NOW bridge (UART RX=GPIO%d TX=GPIO%d)\n", UART_RX_PIN, UART_TX_PIN);

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
    // UART → ESP-NOW: read MSP bytes from the host, parse, collect bursts
    while (BRIDGE_SERIAL.available())
    {
        uint8_t b = BRIDGE_SERIAL.read();
        if (mspFromS3.processReceivedByte(b))
        {
            mspPacket_t *pkt = mspFromS3.getReceivedPacket();
            handlePacketFromS3(pkt);
            mspFromS3.markPacketReceived();
        }
    }

    forwardRx();

    // A host that never sends the closing SET_SEND_UID: the burst ends when
    // the UART goes quiet.
    if (burstLen && millis() - burstLastMs >= BURST_IDLE_MS) flushBurst();
}
