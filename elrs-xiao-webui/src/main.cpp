#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HardwareSerial.h>
#include "web.h"
#include "mdns.h"
#include <Preferences.h>
#include <DNSServer.h>
#include "esp_wifi.h"
#include <lwip/sockets.h>
#include <Update.h>
#include "esp_netif.h"
#include "usb_tether.h"
#include "fw_update.h"
#include "qrcode.h"
#include "log.h"
#include "config.h"
#include "led.h"
#include "msp.h"
#include "msptypes.h"


// ── TCP bridge globals ────────────────────────────────────────────────────────

static WiFiServer     tcpServer(TCP_PORT);
static WiFiClient     tcpClient;
static volatile uint32_t g_mspPeer = 0;   // the MSP (TCP) peer, 0 = none
static volatile uint32_t g_lapIp   = 0;   // the last mDNS answer
// The MSP peer is FPV C5 Lap Timer: it asked the bridge to watch the
// radios' links (0x0F03), which RotorHazard's plugin never does.
static volatile bool     g_peerIsLap = false;
static MSP            mspFromTcp;
static MSP            mspFromUart;
static HardwareSerial uart(1);

// ── Web / AP globals ──────────────────────────────────────────────────────────

static Preferences prefs;
static DNSServer   dnsServer;

static bool     apModeActive        = false;
// While the setup AP is up with a network already saved, keep trying that
// network in the background (the timer's AP may simply not be up yet).
static bool     g_apRetrySta        = false;
static uint32_t g_apRetryMs         = 0;
static const uint32_t AP_RETRY_INTERVAL_MS = 15000;
static uint32_t g_wifiLostMs        = 0;
// A short drop (a missed beacon, a router's key change) is reconnected
// quietly: the buzzer and the LED tell only after WIFI_ALARM_MS without WiFi.
static const uint32_t WIFI_ALARM_MS = 5000;
static bool     g_wifiAlarmed       = false;
static volatile uint8_t g_wifiLostReason = 0;   // the last disconnect's reason (for the log)
static volatile bool g_wifiLostEvent = false; // WiFi 切断イベントフラグ（イベントタスク → loop）
// From the web handlers (AsyncTCP task) to loop(): a beep, joining the
// client network again.
static volatile bool     g_beepReq     = false;
static volatile uint32_t g_reconnectAt = 0;
// Host mode: a device left our WiFi (event task → loop). If it was FPV C5
// Lap Timer, its TCP session ends at once (keepalive alone takes ~11 s).
static volatile bool g_apLeftEvent = false;
static uint8_t       g_apLeftMac[6];
static volatile bool g_apJoinEvent = false;   // a device joined our WiFi (event task → loop)
static volatile uint32_t g_apJoinMs = 0, g_ipLogIp = 0, g_ipLogMs = 0;
static volatile bool g_ipLogEvent = false;

static bool     g_tcpSessionActive  = false;  // 現在 TCP セッション中か
static bool     g_tcpEverConnected  = false;  // 一度でも TCP 接続したか
static bool     g_wifiBuzzerActive  = false;  // WiFi 切断警告ブザー中か
static uint32_t g_wifiBuzzerStartMs = 0;

static Led      builtinLed;   // GPIO21 active-LOW (Led クラス)
// LED_NOTIFY_PIN (GPIO9) は analogWrite で PWM 輝度制御
static inline void nlWrite(uint8_t duty) { analogWrite(LED_NOTIFY_PIN, duty); }

static float    g_vbatRatio      = VBAT_DEFAULT_RATIO;
static float    g_alarmVoltage   = VBAT_DEFAULT_ALARM_V;
static float    g_currentVoltage = 0.0f;
static bool     g_alarmActive    = false;
static uint32_t g_lastVbatMs     = 0;
static bool     g_buzzerEnabled  = true;
static bool     g_ledEnabled     = true;
static bool     g_vbatAlarmEnabled = false;  // デフォルト OFF（バッテリー未接続時の誤発報防止）
static bool     g_langJa         = true;

Print *g_log = &Serial;

// Host mode: the netpack's own WiFi. The timer (client mode) and the phones
// join it; the MSP from the timer goes on to the ESP-NOW bridge as ever.
// With USB tethering, a phone on the USB port takes it out to the internet.
static uint8_t  g_netMode    = 0;       // 0 = client (joins a WiFi), 1 = host (as running)
static uint8_t  g_netModeSet = 0;       // the mode chosen on the page
static uint8_t  g_swMode     = 0;       // mode switch: 0 not used, 1 used
static int8_t   g_swOn       = -1;      // the switch at boot: 1 closed (host), 0 open, -1 not used
static bool     g_hostActive = false;   // host mode running
static String   g_hostSsid   = HOST_SSID_DEFAULT;
static String   g_hostPass   = HOST_PASS_DEFAULT;
static String   g_admPass    = ADMIN_PASS_DEFAULT;  // every page but QR asks for it
static struct { String t; uint32_t at; } g_tokens[4];   // logged-in browsers (until restart)
static uint8_t  g_tokNext    = 0;
static uint16_t g_loginMin   = 720;                  // a login lasts (minutes): 5, 30, 60, 360, 720
static const uint16_t LOGIN_CHOICES[] = {5, 30, 60, 360, 720};
static bool authed();
static uint8_t  g_hostCh     = HOST_CH_DEFAULT;
static uint8_t  g_hostPhones = HOST_PHONES_DEFAULT;   // phones / PCs besides FPV C5 Lap Timer
static uint8_t  g_lapMac[6];                          // FPV C5 Lap Timer's (learned from its TCP)
static bool     g_lapMacKnown = false;
static bool isLapTimer(const uint8_t *mac);
static bool     g_tetherEn   = false;   // USB tethering (host mode only)

// Client mode: up to 5 networks (as the timer), joined as selected or, with
// "automatic", the strongest of them in range; a fixed address if set.
static const int STA_NETS = 5;
static String    g_staSsid[STA_NETS], g_staPass[STA_NETS];
static uint8_t   g_staSel   = 0;        // the selected network
static bool      g_staAuto  = false;    // automatic: the strongest known in range
static bool      g_ipFix    = false;
static IPAddress g_ipAddr, g_ipMask(255, 255, 255, 0), g_ipGw, g_ipDns;

// ── i18n helper ───────────────────────────────────────────────────────────────

static const char *L(const char *en, const char *ja)
{
    return g_langJa ? ja : en;
}

// ── buzzer / LED helpers ──────────────────────────────────────────────────────

static void buzzerRawOn()  { digitalWrite(BUZZER_PIN_POS, HIGH); if (BUZZER_PIN_NEG >= 0) digitalWrite(BUZZER_PIN_NEG, LOW); }
static void buzzerRawOff() { digitalWrite(BUZZER_PIN_POS, LOW);  if (BUZZER_PIN_NEG >= 0) digitalWrite(BUZZER_PIN_NEG, LOW); }

// アラーム制御（ブザーのみ — LED は updateNotifyLed() が一元管理）
static void alarmOn()  { if (g_buzzerEnabled) buzzerRawOn(); }
static void alarmOff() { buzzerRawOff(); }

// ── 非ブロッキング通知ビープ ───────────────────────────────────────────────
// loop() を止めずにブザー/LED を鳴らす状態機械。
// 旧 beepShort/beepDouble/beepLong3 は delay() で最大 ~2.9 秒 loop() を停止させ、
// その間 UART が読まれず Wrover(ESP-NOW) へ渡す MSP を取りこぼしていた。
// （WiFi 弱化 → TCP 切断頻発 → beepLong3 多発 → ESP-NOW 不通の主因）
static struct {
    bool     active     = false;
    bool     phaseOn    = false;  // 現在 ON フェーズか
    uint8_t  pulsesLeft = 0;
    uint16_t onMs       = 0;
    uint16_t gapMs      = 0;
    uint32_t phaseMs    = 0;
} g_beep;

// A beep asked for while another is sounding waits its turn (a short
// queue): it used to cut the one sounding and start over, so WiFi joined
// (2 short) and FPV C5 Lap Timer connecting (1 short) a moment later ran
// together into a fast "pipipi". Played one after the other with a pause.
static const uint16_t BEEP_BETWEEN_MS = 250;   // pause between two queued beeps
static struct { uint8_t pulses; uint16_t onMs, gapMs; } g_beepQ[4];
static uint8_t  g_beepQn = 0;
static uint32_t g_beepNextAt = 0;   // millis() the next queued beep may start (0: none waiting)

static void beepPlay(uint8_t pulses, uint16_t onMs, uint16_t gapMs)
{
    g_beep.active     = true;
    g_beep.phaseOn    = true;
    g_beep.pulsesLeft = pulses;
    g_beep.onMs       = onMs;
    g_beep.gapMs      = gapMs;
    g_beep.phaseMs    = millis();
    if (g_buzzerEnabled) buzzerRawOn();
    if (g_ledEnabled)    nlWrite(255);
}

static void beepStart(uint8_t pulses, uint16_t onMs, uint16_t gapMs)
{
    if (pulses == 0) return;
    if (!g_buzzerEnabled && !g_ledEnabled) return;
    if (!g_beep.active && !g_beepQn && (int32_t)(millis() - g_beepNextAt) >= 0) {
        beepPlay(pulses, onMs, gapMs);
        return;
    }
    if (g_beepQn < 4) g_beepQ[g_beepQn++] = {pulses, onMs, gapMs};   // full: dropped
}

static void beepCancel()
{
    g_beepQn = 0;   // the waiting ones go too
    if (!g_beep.active) return;
    g_beep.active = false;
    buzzerRawOff();
    nlWrite(0);
}

// loop() 毎回呼ぶ。millis ベースで ON/OFF を進める（delay 不使用）。
static void updateBeep()
{
    uint32_t now = millis();
    if (!g_beep.active) {
        // The next one waiting, after the pause.
        if (g_beepQn && (int32_t)(now - g_beepNextAt) >= 0) {
            auto b = g_beepQ[0];
            for (int i = 1; i < g_beepQn; i++) g_beepQ[i - 1] = g_beepQ[i];
            g_beepQn--;
            beepPlay(b.pulses, b.onMs, b.gapMs);
        }
        return;
    }
    if (g_beep.phaseOn) {
        if (now - g_beep.phaseMs < g_beep.onMs) return;
        buzzerRawOff();
        nlWrite(0);
        if (--g_beep.pulsesLeft == 0) {
            g_beep.active = false;
            g_beepNextAt = (now + BEEP_BETWEEN_MS) | 1;   // a pause before anything after it
            return;
        }
        g_beep.phaseOn = false;
        g_beep.phaseMs = now;
    } else {
        if (now - g_beep.phaseMs < g_beep.gapMs) return;
        g_beep.phaseOn = true;
        g_beep.phaseMs = now;
        if (g_buzzerEnabled) buzzerRawOn();
        if (g_ledEnabled)    nlWrite(255);
    }
}

// 通知ビープ（呼び出し側はそのまま、内部は非ブロッキング化）
static inline void beepShort()  { beepStart(1, 80, 0);    }  // 短音 1 回
static inline void beepDouble() { beepStart(2, 80, 120);  }  // 短音 2 回
static inline void beepLong3()  { beepStart(5, 500, 200); }  // TCP 切断警告: 長音 5 回

// 接続受け入れ時に TCP keepalive を設定（デッドコネクション検出: 10s idle → 3s間隔 × 4回）
static void setTcpKeepalive(int fd)
{
    int yes = 1;
    setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE,  &yes, sizeof(yes));
    // 10 s idle, 4 probes 3 s apart (~22 s). Shorter dropped a good session
    // on a weak link when a few probes went missing; FPV C5 Lap Timer
    // leaving our WiFi ends it at once anyway (g_apLeftEvent), and one that
    // restarts connects again (the new connection replaces the old).
    int v = 10; setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &v, sizeof(v));
        v = 3;  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &v, sizeof(v));
        v = 4;  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &v, sizeof(v));
}

// WiFi 切断ブザーの 5 秒タイムアウト管理
static void updateWifiBuzzer()
{
    if (!g_wifiBuzzerActive) return;
    if (millis() - g_wifiBuzzerStartMs >= 5000) {
        buzzerRawOff();
        g_wifiBuzzerActive = false;
        return;
    }
    // 通知ビープがブザーを一時的に OFF にした後も、5 秒の鳴動を維持する
    if (!g_beep.active && g_buzzerEnabled) buzzerRawOn();
}

// ── MSP bridge helpers ────────────────────────────────────────────────────────

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
        mspPacket_t resp;
        resp.reset();
        resp.makeResponse();
        resp.function    = MSP_ELRS_GET_BACKPACK_VERSION;
        resp.payloadSize = 4;
        resp.payload[0]  = 10;
        resp.payload[1]  = 1;
        resp.payload[2]  = 0;
        resp.payload[3]  = 0;
        sendMspToTcp(&resp);
        return;
    }
    if (pkt->function == MSP_ELRS_BACKPACK_SET_BUZZER)
        beepShort();
    if (pkt->function == 0x0F03) g_peerIsLap = true;   // FPV C5 Lap Timer's link watch
    sendMspToUart(pkt);
}

static void handlePacketFromUart(mspPacket_t *pkt)
{
    sendMspToTcp(pkt);
}

// ── HTML helpers ──────────────────────────────────────────────────────────────

static String rssiBar()
{
    if (g_hostActive) {
        char hb[48];
        snprintf(hb, sizeof hb, "%s ch%u · %d", L("Host", "ホスト"), g_hostCh, WiFi.softAPgetStationNum());
        return String(hb);
    }
    if (apModeActive)
        return L("AP Mode", "APモード");
    if (WiFi.status() != WL_CONNECTED)
        return "--";
    int rssi = WiFi.RSSI();
    const char *bar;
    if      (rssi >= -50) bar = "▂▄▆█";
    else if (rssi >= -60) bar = "▂▄▆░";
    else if (rssi >= -70) bar = "▂▄░░";
    else                   bar = "▂░░░";
    char buf[40];
    snprintf(buf, sizeof(buf), "%s %d dBm", bar, rssi);
    return String(buf);
}

static String htmlHead(const char *title)
{
    String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    h += "<title>ELRS-NETpack — "; h += title; h += "</title>";
    h += "<style>"
         "*{box-sizing:border-box}"
         "body{background:#181818;color:#d4d4d4;font-family:sans-serif;"
         "max-width:480px;margin:40px auto;padding:0 16px}"
         "h1{font-size:1.25em;color:#f0f0f0;margin:0 0 2px}"
         "h2{font-size:1.05em;color:#aaa;margin:12px 0 4px}"
         ".topbar{display:flex;align-items:baseline;justify-content:space-between;flex-wrap:wrap}"
         ".rssi{font-size:.78em;color:#fff;letter-spacing:.5px}"
         "nav{display:flex;align-items:center;gap:10px;margin:6px 0}"
         "nav a{text-decoration:none;color:#5b9bd5;font-size:.9em}"
         "nav a:hover{color:#79b8f3}"
         ".lang{margin-left:auto;background:#242424;color:#888;"
         "border:1px solid #3a3a3a;border-radius:4px;padding:2px 10px;"
         "font-size:.78em;cursor:pointer;text-decoration:none}"
         ".lang:hover{background:#2e2e2e;color:#bbb}"
         "hr{border:none;border-top:1px solid #2a2a2a;margin:6px 0 14px}"
         "label{display:block;margin-top:14px;font-size:.88em;color:#999}"
         ".chk{display:flex;align-items:center;gap:8px;margin-top:14px;"
         "font-size:.88em;color:#999}"
         "input[type=text],input[type=password],input[type=number]"
         "{width:100%;padding:8px 10px;background:#212121;border:1px solid #383838;"
         "border-radius:5px;color:#ddd;margin-top:4px;font-size:.95em}"
         "input[type=text]:focus,input[type=password]:focus,input[type=number]:focus"
         "{outline:none;border-color:#5b9bd5}"
         "input[type=checkbox]{width:auto;accent-color:#5b9bd5}"
         "button{margin-top:18px;padding:9px 22px;background:#1a4a8a;color:#dde6f0;"
         "border:none;border-radius:5px;cursor:pointer;font-size:.95em}"
         "button:hover{background:#1f5aa8}"
         ".msg{margin-top:12px;padding:9px 12px;border-radius:5px;"
         "background:#1a2e1a;color:#7ec47e;font-size:.9em}"
         ".alarm{background:#2e1010;color:#e07070}"
         ".card{background:#212121;border:1px solid #2e2e2e;border-radius:8px;"
         "padding:14px 18px;margin:10px 0}"
         ".volt{font-size:2em;color:#f0f0f0;font-weight:300;letter-spacing:1px}"
         ".st{font-size:.86em;line-height:1.6}.st a{color:#5b9bd5}"
         ".note{font-size:.78em;color:#777;margin-top:6px;line-height:1.5}"
         "select{width:100%;padding:8px 10px;background:#212121;border:1px solid #383838;"
         "border-radius:5px;color:#ddd;margin-top:4px;font-size:.95em}"
         ".radio{display:flex;align-items:center;gap:8px;margin-top:8px;font-size:.9em}"
         ".radio input{width:auto;accent-color:#5b9bd5}"
         ".nets{width:100%;border-collapse:collapse;margin-top:8px;font-size:.88em}"
         ".nets th{color:#888;font-weight:normal;text-align:left;padding:2px 4px}"
         ".nets td{padding:2px 4px}.nets td:first-child{width:24px}"
         ".nets input[type=text],.nets input[type=password]{margin-top:0}"
         ".nets input[type=radio]{accent-color:#5b9bd5}"
         ".ipgrid{display:grid;grid-template-columns:1fr 1fr;gap:0 10px}"
         "</style></head><body>";
    h += "<div class='topbar'><h1>ELRS-NETpack</h1>";
    h += "<span class='rssi'>"; h += rssiBar(); h += "</span></div>";
    h += "<nav>";
    h += "<a href='/wifi'>WiFi</a>";
    h += "<a href='/voltage'>"; h += L("Voltage", "電圧"); h += "</a>";
    h += "<a href='/qr'>QR</a>";
    h += "<a href='/update'>"; h += L("Update", "更新"); h += "</a>";
    if (authed()) { h += "<a class='lang' href='/logout' style='margin-left:auto'>"; h += L("Log out", "ログアウト"); h += "</a>"; }
    h += "<a class='lang' href='/lang'"; if (authed()) h += " style='margin-left:0'"; h += ">"; h += (g_langJa ? "EN" : "JA"); h += "</a>";
    h += "</nav><hr>";
    return h;
}

// Text inside an HTML attribute or page. An SSID with an apostrophe
// ("John's iPhone") used to end the value='...' of the SSID field early:
// the field showed "John", and saving the form again stored that.
static String htmlEsc(const String &in)
{
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if      (c == '&')  out += "&amp;";
        else if (c == '<')  out += "&lt;";
        else if (c == '>')  out += "&gt;";
        else if (c == '"')  out += "&quot;";
        else if (c == '\'') out += "&#39;";
        else                out += c;
    }
    return out;
}

static bool anyNet()
{
    for (int i = 0; i < STA_NETS; i++)
        if (g_staSsid[i].length()) return true;
    return false;
}

// The network to join: the selected one, or (automatic) the strongest saved
// one in range (a scan, a few seconds). None in range: the selected one.
static int pickNetwork()
{
    int pick = g_staSsid[g_staSel].length() ? g_staSel : -1;
    if (g_staAuto) {
        int n = WiFi.scanNetworks(false, false);
        int best = -1, bestRssi = -1000;
        for (int i = 0; i < n; i++)
            for (int k = 0; k < STA_NETS; k++)
                if (g_staSsid[k].length() && g_staSsid[k] == WiFi.SSID(i) && WiFi.RSSI(i) > bestRssi)
                    best = k, bestRssi = WiFi.RSSI(i);
        WiFi.scanDelete();
        if (best >= 0) {
            pick = best;
            LOG.printf("[wifi] automatic: \"%s\" is the strongest (%d dBm)\n", g_staSsid[best].c_str(), bestRssi);
        } else {
            LOG.println("[wifi] automatic: no saved network in range");
        }
    }
    for (int i = 0; i < STA_NETS && pick < 0; i++)
        if (g_staSsid[i].length()) pick = i;
    return pick;
}

// Join network i (the fixed address, or DHCP).
static void staBegin(int i)
{
    if (i < 0) return;
    if (g_ipFix) {
        if (!WiFi.config(g_ipAddr, g_ipGw, g_ipMask, g_ipDns ? g_ipDns : g_ipGw)) LOG.println("[wifi] fixed address rejected, DHCP");
        else LOG.printf("[wifi] fixed address %s\n", g_ipAddr.toString().c_str());
    } else {
        WiFi.config(IPAddress((uint32_t)0), IPAddress((uint32_t)0), IPAddress((uint32_t)0));   // DHCP
    }
    // Look on every channel and take the network's strongest access point:
    // by default the first one heard is joined, the far one of a mesh or a
    // repeater as often as the near one (FPV C5 Lap Timer does the same).
    // Also used when reconnecting (WiFi.reconnect keeps it).
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    LOG.printf("[wifi] connecting to %s\n", g_staSsid[i].c_str());
    WiFi.begin(g_staSsid[i].c_str(), g_staPass[i].c_str());
}

// What the netpack is doing now: its WiFi, who sends it MSP (the timer:
// a link to its page), the devices on its WiFi, the USB tethering.
// FPV C5 Lap Timer's address, kept by loop() for the pages and the DNS
// relay (they never wait): the MSP peer, else the last answer of an mDNS
// lookup for fpv-c5-lt.local made in the background every 30 s.

static IPAddress knownLapTimer()
{
    if (g_mspPeer && g_peerIsLap) return IPAddress(g_mspPeer);
    return IPAddress(g_lapIp);   // RotorHazard sending MSP is not FPV C5 Lap Timer
}

// RotorHazard sending MSP here: which port its web page is on. 5000 is
// its default, 80 the one people set instead: 5000 is tried (a task: a
// connect that gets no answer takes a while), and if it doesn't answer the
// page is on 80. g_rhPort: 0 not known yet, else 5000 or 80.
static volatile uint32_t g_rhIp = 0;
static volatile uint16_t g_rhPort = 0;
static volatile bool     g_rhProbing = false;
static uint32_t          g_rhSeenMs = 0;

static bool portOpen(uint32_t ip, uint16_t port, int ms)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return false;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = ip;
    bool ok = false;
    if (connect(fd, (sockaddr *)&a, sizeof a) == 0) ok = true;
    else if (errno == EINPROGRESS) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(fd, &w);
        timeval tv = {ms / 1000, (ms % 1000) * 1000};
        if (select(fd + 1, nullptr, &w, nullptr, &tv) > 0) {
            int err = 0;
            socklen_t len = sizeof err;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            ok = err == 0;
        }
    }
    close(fd);
    return ok;
}

static void rhProbeTask(void *arg)
{
    uint32_t ip = (uint32_t)(uintptr_t)arg;
    uint16_t port = portOpen(ip, 5000, 1500) ? 5000 : 80;
    if (g_rhIp == ip) g_rhPort = port;
    LOG.printf("[rh] RotorHazard " IPSTR ": web page on port %u\n", IP2STR((esp_ip4_addr_t *)&ip), port);
    g_rhProbing = false;
    vTaskDelete(nullptr);
}

// From loop(): a peer that hasn't shown itself as FPV C5 Lap Timer within
// 3 s of connecting (the timer sends 0x0F03 at once) is RotorHazard.
static void probeRotorHazard()
{
    uint32_t peer = g_mspPeer;
    if (!peer || g_peerIsLap) { g_rhSeenMs = 0; return; }
    if (!g_rhSeenMs) { g_rhSeenMs = millis() | 1; return; }
    if (millis() - g_rhSeenMs < 3000 || peer == g_rhIp || g_rhProbing) return;
    g_rhIp = peer;
    g_rhPort = 0;
    g_rhProbing = true;
    if (xTaskCreate(rhProbeTask, "rhprobe", 4096, (void *)(uintptr_t)peer, 1, nullptr) != pdPASS) g_rhProbing = false;
}

static void lookUpLapTimer()
{
    static mdns_search_once_t *search = nullptr;
    static uint32_t last = 0;
    if (search) {
        mdns_result_t *res = nullptr;
        uint8_t n = 0;
        if (!mdns_query_async_get_results(search, 0, &res, &n)) return;   // still asking
        for (mdns_result_t *r = res; r; r = r->next)
            for (mdns_ip_addr_t *a = r->addr; a; a = a->next)
                if (a->addr.type == ESP_IPADDR_TYPE_V4) g_lapIp = a->addr.u_addr.ip4.addr;
        if (res) mdns_query_results_free(res);
        mdns_query_async_delete(search);
        search = nullptr;
        return;
    }
    uint32_t now = millis();
    if (g_mspPeer || apModeActive || (last && now - last < 30000)) return;
    last = now;
    search = mdns_query_async_new("fpv-c5-lt", nullptr, nullptr, MDNS_TYPE_A, 1000, 1, nullptr);
}

static String statusCard()
{
    String p = "<div class='card st'>";
    char b[96];
    {
        IPAddress lt = knownLapTimer();
        p += "<b>FPV C5 Lap Timer</b>：";
        if ((uint32_t)lt) {
            String u = "http://" + lt.toString() + "/";
            p += "<a href='"; p += u; p += "' target='_blank'>"; p += u; p += "</a>（";
            p += "<a href='"; p += u; p += "' target='_blank'>"; p += L("open", "開く"); p += "</a>）<br>";
        } else {
            p += L("not found (join it to the same WiFi)", "見つかりません（同じ WiFi につないでください）"); p += "<br>";
        }
    }
    if (g_hostActive) {
        snprintf(b, sizeof b, "%s ch%u", htmlEsc(g_hostSsid).c_str(), g_hostCh);
        p += L("Host mode: ", "ホストモード："); p += b; p += "（192.168.4.1）<br>";
    } else if (apModeActive) {
        p += L("Setup AP (the saved WiFi is not found)", "設定用 AP（保存した WiFi が見つかりません）"); p += "<br>";
    } else if (WiFi.status() == WL_CONNECTED) {
        p += L("Client mode: ", "クライアントモード："); p += htmlEsc(WiFi.SSID());
        p += "（"; p += WiFi.localIP().toString(); p += "）<br>";
    }
    if (g_mspPeer) {
        p += L("MSP: connected (", "MSP：接続中（"); p += IPAddress(g_mspPeer).toString(); p += L(")", "）"); p += "<br>";
    } else {
        p += L("MSP: not connected", "MSP：未接続"); p += "<br>";
    }
    if (g_hostActive) {
        wifi_sta_list_t sl = {};
        esp_wifi_ap_get_sta_list(&sl);
        int others = 0;
        bool lapIn = false;
        for (int i = 0; i < sl.num; i++) {
            if (isLapTimer(sl.sta[i].mac)) lapIn = true;
            else others++;
        }
        snprintf(b, sizeof b, "%s%d%s%d / %d%s", L("Devices on this WiFi: ", "この WiFi の機器："), sl.num,
                 lapIn ? L(" (FPV C5 Lap Timer + phones/PCs ", "（FPV C5 Lap Timer ＋ スマホ・PC ") : L(" (phones/PCs ", "（スマホ・PC "),
                 others, g_hostPhones, L(")", "）"));
        p += b; p += "<br>";
        esp_netif_pair_mac_ip_t pairs[HOST_MAX_STA] = {};
        int n = sl.num < HOST_MAX_STA ? sl.num : HOST_MAX_STA;
        for (int i = 0; i < n; i++) memcpy(pairs[i].mac, sl.sta[i].mac, 6);
        if (n) esp_netif_dhcps_get_clients_by_mac(WiFi.AP.netif(), n, pairs);
        for (int i = 0; i < n; i++) {
            if (pairs[i].ip.addr) snprintf(b, sizeof b, "・" IPSTR "（%d dBm）<br>", IP2STR(&pairs[i].ip), sl.sta[i].rssi);
            else snprintf(b, sizeof b, "・%s（%d dBm）<br>", L("no address yet", "アドレス未取得"), sl.sta[i].rssi);
            p += b;
        }
        if (g_tetherEn) {
            TetherInfo t;
            tetherInfo(&t);
            p += L("USB tethering: ", "USB テザリング："); p += tetherStateText(t.state, g_langJa);
            if (t.product[0]) { p += "（"; p += htmlEsc(t.product); if (t.proto[0]) { p += " / "; p += t.proto; } p += "）"; }
            if (t.state == TetherState::Up) {
                snprintf(b, sizeof b, "<br>・" IPSTR " / DNS " IPSTR, IP2STR(&t.ip.ip), IP2STR(&t.dns));
                p += b;
            }
            p += "<br>";
        }
    }
    p += L("Firmware ", "ファーム "); p += fwVersion(); p += "（"; p += fwBuild(); p += "）";
    if (fwTimerWiring()) p += L(" timerwiring ", " タイマー流用版 ");
    p += "— <a href='/update'>"; p += L("update", "更新"); p += "</a>";
    p += "</div>";
    return p;
}

static String nvsHostPassSet()
{
    return g_hostPass.length() ? L("(set; leave blank to keep)", "(設定済み。変更しない場合は空欄)") : "";
}

static String pageWifi(const String &msg = "")
{
    String p = htmlHead(L("WiFi Settings", "WiFi設定"));
    p += statusCard();
    if (msg.length()) { p += "<div class='msg'>"; p += msg; p += "</div>"; }
    p += "<form method='POST' action='/wifi'>";
    p += "<h2>"; p += L("Connection", "接続方式"); p += "</h2>";
    p += "<label class='radio'><input type='radio' name='mode' value='0'"; if (g_netModeSet == 0) p += " checked";
    p += ">"; p += L("Client: join a WiFi (FPV C5 Lap Timer's or a router's)", "クライアント：WiFi に接続（FPV C5 Lap Timer やルーター）"); p += "</label>";
    p += "<label class='radio'><input type='radio' name='mode' value='1'"; if (g_netModeSet == 1) p += " checked";
    p += ">"; p += L("Host: the netpack's own WiFi (FPV C5 Lap Timer and phones join it)", "ホスト：netpack が WiFi を出す（FPV C5 Lap Timer やスマホが接続）"); p += "</label>";
    p += "<div class='chk'><input type='checkbox' name='swMode' value='1'"; if (g_swMode) p += " checked";
    p += "><span>"; p += L("Use a mode switch", "物理スイッチを使う"); p += "</span></div><div class='note'>";
    p += L("Between " MODE_SW_LABEL " and GND: ON (connected) = host mode, OFF = client mode. Read at start-up (takes effect after a restart).",
           MODE_SW_LABEL_JA " と GND の間：ON（つながる）でホストモード、OFF でクライアントモード。起動時に読みます（切り替えは再起動後）。");
    if (g_swMode) {
        p += "<br><b>"; p += L("At this start-up: switch ", "今回の起動：スイッチ "); p += g_swOn ? "ON" : "OFF";
        p += " → "; p += g_netMode ? L("host mode", "ホストモード") : L("client mode", "クライアントモード"); p += "</b>";
    }
    p += "</div>";

    p += "<h2>"; p += L("Client", "クライアント"); p += "</h2>";
    p += "<label>"; p += L("Which network", "接続先の選び方"); p += "<select name='staAuto'>";
    p += "<option value='0'"; if (!g_staAuto) p += " selected"; p += ">"; p += L("The one selected (●)", "選んだもの（●）"); p += "</option>";
    p += "<option value='1'"; if (g_staAuto) p += " selected"; p += ">"; p += L("Automatic: the strongest saved one", "自動（登録した中で電波が一番強いもの）"); p += "</option>";
    p += "</select></label>";
    p += "<table class='nets'><tr><th>●</th><th>SSID</th><th>"; p += L("Password", "パスワード"); p += "</th></tr>";
    for (int i = 0; i < STA_NETS; i++) {
        String n = String(i);
        p += "<tr><td><input type='radio' name='sel' value='"; p += n; p += "'"; if (i == g_staSel) p += " checked"; p += "></td>";
        p += "<td><input type='text' name='s"; p += n; p += "' maxlength='32' autocomplete='off' value='"; p += htmlEsc(g_staSsid[i]); p += "'></td>";
        p += "<td><input type='password' name='p"; p += n; p += "' maxlength='63' autocomplete='new-password' placeholder='";
        p += g_staPass[i].length() ? L("set", "設定済み") : ""; p += "'></td></tr>";
    }
    p += "</table><div class='note'>";
    p += L("Up to 5. Automatic picks the strongest saved network when joining. Passwords are not shown (blank keeps it). Clear an SSID to delete it.",
           "5 つまで登録できます。自動は、接続するときに一番電波の強い接続先を選びます。パスワードは表示しません（変えないときは空欄）。SSID を消すと削除されます。");
    p += "</div>";
    p += "<label>"; p += L("Address", "IP アドレス"); p += "<select name='ipfix'>";
    p += "<option value='0'"; if (!g_ipFix) p += " selected"; p += ">"; p += L("Automatic (DHCP)", "自動（DHCP）"); p += "</option>";
    p += "<option value='1'"; if (g_ipFix) p += " selected"; p += ">"; p += L("Fixed", "固定"); p += "</option>";
    p += "</select></label>";
    {
        const char *names[] = {"ip", "mask", "gw", "dns"};
        const char *en[] = {"IP address", "Subnet mask", "Gateway", "DNS (blank: the gateway)"};
        const char *jp[] = {"IP アドレス", "サブネットマスク", "ゲートウェイ", "DNS（空欄ならゲートウェイ）"};
        IPAddress v[] = {g_ipAddr, g_ipMask, g_ipGw, g_ipDns};
        p += "<div class='ipgrid'>";
        for (int k = 0; k < 4; k++) {
            p += "<label>"; p += L(en[k], jp[k]); p += "<input type='text' name='"; p += names[k]; p += "' inputmode='decimal' value='";
            if ((uint32_t)v[k]) p += v[k].toString();
            p += "' placeholder='192.168.1.x'></label>";
        }
        p += "</div><div class='note'>";
        p += L("Fixed: the netpack always has this address on that network (all saved networks use it).",
               "固定にすると、netpack はいつもこのアドレスになります（登録したすべての接続先で同じアドレスを使います）。");
        p += "</div>";
    }

    p += "<h2>"; p += L("Host", "ホスト"); p += "</h2>";
    p += "<label>SSID<input type='text' name='hssid' autocomplete='off' maxlength='32' value='";
    p += htmlEsc(g_hostSsid); p += "'></label>";
    p += "<label>"; p += L("Password (8-63 characters)", "パスワード（8〜63 文字）");
    p += "<input type='password' name='hpass' autocomplete='new-password' maxlength='63' placeholder='";
    p += nvsHostPassSet(); p += "'></label>";
    p += "<label>"; p += L("Channel", "チャンネル"); p += "<select name='hch'>";
    for (int c = 6; c <= 11; c++) {
        p += "<option value='"; p += String(c); p += "'"; if (c == g_hostCh) p += " selected";
        p += ">"; p += String(c); p += " ch</option>";
    }
    p += "</select></label>";
    p += "<label>"; p += L("Phones / PCs (besides FPV C5 Lap Timer)", "スマホ・PC の接続台数（FPV C5 Lap Timer とは別）"); p += "<select name='hmax'>";
    for (int c = 1; c <= HOST_PHONES_MAX; c++) {
        p += "<option value='"; p += String(c); p += "'"; if (c == g_hostPhones) p += " selected";
        p += ">"; p += String(c); p += L("", " 台"); if (c == HOST_PHONES_DEFAULT) p += L(" (default)", "（初期値）"); p += "</option>";
    }
    p += "</select></label>";
    p += "<div class='note'>";
    p += L("A limit for use as a bridge: one place is always kept free for FPV C5 Lap Timer, and the phones / PCs are limited to this number (default 5, as FPV C5 Lap Timer's own host mode). "
           "One more is sent off and kept out for 30 s. 10 devices in all at most.",
           "ブリッジとして使うための制限です。FPV C5 Lap Timer の 1 台分はいつも空けておき、スマホ・PC はこの台数までにします（初期値 5 台。FPV C5 Lap Timer のホストモードと同じ）。"
           "上限を超えてつないだ機器は外し、30 秒間は入れません。WiFi 全体で最大 10 台です。");
    p += "</div>";
    p += "<div class='note'>";
    p += L("FPV C5 Lap Timer joins this WiFi in client mode (its netpack address: auto). Phones open it from its link at the top. ",
           "FPV C5 Lap Timer はクライアントモードでこの WiFi に接続します（netpack アドレスは auto）。スマホからは、上の「FPV C5 Lap Timer」のリンクで画面を開けます。");
    p += "</div>";
    p += "<div class='chk'><input type='checkbox' name='tether' value='1'"; if (g_tetherEn) p += " checked";
    p += "><span>"; p += L("USB tethering: a phone on the USB port takes this WiFi to the internet", "USB テザリング：USB につないだスマホから、この WiFi をインターネットへ"); p += "</span></div>";
    p += "<div class='note'>";
    p += L("Host mode only. Android: turn on Settings → Network → Hotspot &amp; tethering → USB tethering. While on, the USB port is not a serial port: update from the Update page (or hold BOOT while plugging in).",
           "ホストモードのときだけ動きます。Android は「設定 → ネットワーク → テザリング → USB テザリング」を ON。ON の間は USB でのシリアル・書き込みはできません（書き込みは「更新」ページ、または BOOT を押しながら USB を接続）。");
    p += "</div>";
    p += "<h2>"; p += L("Admin password", "管理者パスワード"); p += "</h2>";
    p += "<label>"; p += L("Password (blank keeps it)", "パスワード（変えないときは空欄）");
    p += "<input type='password' name='admpass' maxlength='32' autocomplete='new-password'></label>";
    p += "<label>"; p += L("A login lasts", "ログインの有効時間"); p += "<select name='loginmin'>";
    for (uint16_t m : LOGIN_CHOICES) {
        p += "<option value='"; p += String(m); p += "'"; if (m == g_loginMin) p += " selected"; p += ">";
        if (m < 60) { p += String(m); p += L(" min", " 分"); }
        else { p += String(m / 60); p += L(m == 60 ? " hour" : " hours", " 時間"); }
        if (m == 720) p += L(" (default)", "（初期値）");
        p += "</option>";
    }
    p += "</select></label>";
    p += "<div class='note'>";
    p += L("Asked for every page but QR.", "QR 以外のページを開くときに聞きます。");
    if (g_admPass == ADMIN_PASS_DEFAULT)
        p += L("<br><b style='color:#e0a050'>Still the default (admin): change it.</b>",
               "<br><b style='color:#e0a050'>初期値（admin）のままです。変更してください。</b>");
    p += "</div>";
    p += "<button type='submit'>"; p += L("Save", "保存"); p += "</button>";
    p += "</form></body></html>";
    return p;
}

// ── QR codes ──────────────────────────────────────────────────────────────────
// Made here (esp_qrcode, no internet needed) and drawn as SVG.

static String *g_qrOut = nullptr;

static void qrToSvg(esp_qrcode_handle_t q)
{
    int n = esp_qrcode_get_size(q), m = n + 8;   // a 4-module quiet zone
    String &o = *g_qrOut;
    o += "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 "; o += m; o += " "; o += m;
    o += "' shape-rendering='crispEdges' style='width:220px;height:220px;display:block;margin:8px auto'>";
    o += "<rect width='100%' height='100%' fill='#fff'/><path fill='#000' d='";
    char b[24];
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (esp_qrcode_get_module(q, x, y)) {
                snprintf(b, sizeof b, "M%d %dh1v1h-1z", x + 4, y + 4);
                o += b;
            }
    o += "'/></svg>";
}

static String qrSvg(const String &text)
{
    String out;
    g_qrOut = &out;
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = qrToSvg;
    cfg.max_qrcode_version = 10;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
    if (esp_qrcode_generate(&cfg, text.c_str()) != ESP_OK) out = "";
    g_qrOut = nullptr;
    return out;
}

// A WiFi QR code's field: \ ; , : " are escaped.
static String wifiQrEsc(const String &in)
{
    String o;
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') o += '\\';
        o += c;
    }
    return o;
}

// Login: every page but the QR page needs the admin password. A login is a
// cookie with a random token, kept 12 h (tokens are forgotten at restart).
static bool authed()
{
    String c = webServer.header("Cookie");
    int i = c.indexOf("np=");
    if (i < 0) return false;
    String t = c.substring(i + 3, i + 3 + 16);
    if (t.length() != 16) return false;
    uint32_t now = millis();
    for (auto &k : g_tokens)
        if (k.t.length() && k.t == t) {
            if (now - k.at < (uint32_t)g_loginMin * 60000UL) return true;
            k.t = "";   // expired
            return false;
        }
    return false;
}

static void toLogin()
{
    String next = webServer.uri();
    if (next == "/login") next = "/wifi";
    webServer.sendHeader("Location", "/login?next=" + next, true);
    webServer.send(302, "text/plain", "");
}

// Checks the login; false (and the browser sent to the login page) without one.
static bool needLogin()
{
    if (authed()) return false;
    toLogin();
    return true;
}

static String pageLogin(const String &msg = "")
{
    String p = htmlHead(L("Login", "ログイン"));
    String next = webServer.arg("next");
    if (!next.startsWith("/") || next.startsWith("//")) next = "/wifi";
    p += "<h2>"; p += L("Admin password", "管理者パスワード"); p += "</h2>";
    if (msg.length()) { p += "<div class='msg alarm'>"; p += msg; p += "</div>"; }
    p += "<form method='POST' action='/login'><input type='hidden' name='next' value='"; p += htmlEsc(next); p += "'>";
    p += "<label>"; p += L("Password", "パスワード"); p += "<input type='password' name='pw' autofocus></label>";
    p += "<button type='submit'>"; p += L("Log in", "ログイン"); p += "</button></form><div class='note'>";
    p += L("The QR page needs no password. Default password: admin (change it on the WiFi page).",
           "QR ページはパスワードなしで見られます。初期パスワード：admin（WiFi のページで変更してください）。");
    p += "</div></body></html>";
    return p;
}

static void handleLoginPost()
{
    String next = webServer.arg("next");
    if (!next.startsWith("/") || next.startsWith("//")) next = "/wifi";
    if (webServer.arg("pw") != g_admPass) {
        webServer.send(200, "text/html", pageLogin(L("Wrong password.", "パスワードが違います。")));
        return;
    }
    char t[17];
    snprintf(t, sizeof t, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    g_tokens[g_tokNext].t = t;
    g_tokens[g_tokNext].at = millis();
    g_tokNext = (g_tokNext + 1) % 4;
    webServer.sendHeader("Set-Cookie", String("np=") + t + "; Path=/; Max-Age=" + String((uint32_t)g_loginMin * 60) + "; HttpOnly");
    webServer.sendHeader("Location", next, true);
    webServer.send(302, "text/plain", "");
}

static void handleLogout()
{
    String c = webServer.header("Cookie");
    int i = c.indexOf("np=");
    if (i >= 0)
        for (auto &k : g_tokens)
            if (k.t.length() && k.t == c.substring(i + 3, i + 19)) k.t = "";
    webServer.sendHeader("Set-Cookie", "np=; Path=/; Max-Age=0");
    webServer.sendHeader("Location", "/qr", true);
    webServer.send(302, "text/plain", "");
}

static String pageQr()
{
    String p = htmlHead("QR");
    // Top: the timer's page, by its IP address (a phone that can't open
    // .local addresses still gets there). RotorHazard sending MSP here: its
    // page instead (port 5000, RotorHazard's own). Neither known yet:
    // fpv-c5-lt.local.
    {
        bool rh = g_mspPeer && !g_peerIsLap;
        IPAddress ip = rh ? IPAddress(g_mspPeer) : knownLapTimer();
        // RotorHazard: 5000 if it answered there, else 80; not tried yet:
        // 5000, with a link for 80 below.
        uint16_t rhPort = rh && g_rhIp == (uint32_t)ip ? g_rhPort : 0;
        bool rhOn80 = rh && rhPort == 80;
        String url = (uint32_t)ip ? "http://" + ip.toString() + (rh && !rhOn80 ? ":5000/" : "/") : String("http://fpv-c5-lt.local/");
        p += "<h2>"; p += rh ? "RotorHazard" : "FPV C5 Lap Timer"; p += "</h2>";
        p += "<div class='card st' style='text-align:center'>";
        p += qrSvg(url);
        p += "<a href='"; p += url; p += "' target='_blank'>"; p += url; p += "</a>";
        if (rh && !rhPort) {
            String u80 = "http://" + ip.toString() + "/";
            p += "<br>"; p += L("Set to port 80: ", "ポート 80 に変えている場合："); p += "<a href='"; p += u80; p += "' target='_blank'>"; p += u80; p += "</a>";
        }
        p += "<div class='note'>";
        if (!(uint32_t)ip)
            p += L("FPV C5 Lap Timer isn't found yet (its address shows here once it connects or answers on the same WiFi).",
                   "FPV C5 Lap Timer がまだ見つかりません（同じ WiFi でつながると、ここに IP アドレスで出ます）。");
        else
            p += L("Join the same WiFi first.", "先に同じ WiFi に接続してください。");
        p += "</div></div>";
    }
    // Bottom: the netpack's WiFi.
    const char *pass = g_hostPass.length() >= 8 ? g_hostPass.c_str() : HOST_PASS_DEFAULT;
    p += "<h2>"; p += L("Join the ELRS-NETpack WiFi", "ELRS-NETpack の WiFi に接続"); p += "</h2>";
    p += "<div class='card st' style='text-align:center'>";
    p += qrSvg("WIFI:T:WPA;S:" + wifiQrEsc(g_hostSsid) + ";P:" + wifiQrEsc(pass) + ";;");
    p += "SSID：<b>"; p += htmlEsc(g_hostSsid); p += "</b><br>";
    p += L("Password: ", "パスワード："); p += "<b>"; p += htmlEsc(pass); p += "</b>";
    if (!g_hostActive) {
        p += "<div class='note'>";
        p += L("The netpack is in client mode now: this WiFi is up in host mode.", "今はクライアントモードです。この WiFi は、ホストモードにしたときに出ます。");
        p += "</div>";
    }
    p += "</div></body></html>";
    return p;
}

static String pageVoltage(const String &msg = "")
{
    String p = htmlHead(L("Voltage", "電圧モニター"));
    p += "<h2>"; p += L("Battery Voltage", "バッテリー電圧"); p += "</h2>";
    p += "<div class='card'><div class='volt'>";
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f V", g_currentVoltage);
    p += buf; p += "</div></div>";
    if (g_alarmActive) { p += "<div class='msg alarm'>"; p += L("LOW VOLTAGE ALARM", "低電圧アラーム"); p += "</div>"; }
    if (msg.length())  { p += "<div class='msg'>"; p += msg; p += "</div>"; }
    p += "<form method='POST' action='/voltage'>";
    p += "<label>"; p += L("Divider ratio (e.g. 2.0 for equal resistors)", "分圧比 (例: 100kΩ×2なら2.0)");
    p += "<input type='number' name='ratio' step='0.001' min='1' value='";
    snprintf(buf, sizeof(buf), "%.3f", g_vbatRatio);
    p += buf; p += "'></label>";
    p += "<label>"; p += L("Alarm threshold (V)", "アラーム閾値 (V)");
    p += "<input type='number' name='alarm' step='0.01' min='2' max='5' value='";
    snprintf(buf, sizeof(buf), "%.2f", g_alarmVoltage);
    p += buf; p += "'></label>";
    // 電圧アラーム有効/無効
    p += "<div class='chk'><input type='checkbox' name='vbatAlarmEn' value='1'";
    if (g_vbatAlarmEnabled) p += " checked";
    p += "><span>"; p += L("Enable voltage alarm", "電圧アラームを有効にする"); p += "</span></div>";
    // ブザー ON/OFF
    p += "<div class='chk'><input type='checkbox' name='buzzerEn' value='1'";
    if (g_buzzerEnabled) p += " checked";
    p += "><span>"; p += L("Buzzer (notifications &amp; alarm)", "ブザー (通知・アラーム)"); p += "</span></div>";
    // LED ON/OFF
    p += "<div class='chk'><input type='checkbox' name='ledEn' value='1'";
    if (g_ledEnabled) p += " checked";
    p += "><span>"; p += L("LED (notifications &amp; alarm)", "LED (通知・アラーム)"); p += "</span></div>";
    p += "<button type='submit'>"; p += L("Save", "保存"); p += "</button>";
    p += "</form>";
    // Calibration: the voltage measured with a meter sets the divider ratio.
    p += "<h2>"; p += L("Calibrate", "校正"); p += "</h2>";
    p += "<div class='note'>";
    p += L("Measure the battery with a meter and enter it: the divider ratio is worked out from it and saved (resistor and ADC tolerances make the reading a little off).",
           "テスターでバッテリーの電圧を測って入れると、分圧比を計算して保存します（抵抗や ADC の誤差で、表示が少しずれることがあります）。");
    p += "</div>";
    p += "<form method='POST' action='/voltage/cal'>";
    p += "<label>"; p += L("Measured voltage (V)", "実際の電圧（V）");
    p += "<input type='number' name='actual' step='0.01' min='0.5' max='30' required></label>";
    p += "<button type='submit'>"; p += L("Calibrate", "校正する"); p += "</button>";
    p += "</form></body></html>";
    return p;
}

// ── WiFi / AP ─────────────────────────────────────────────────────────────────

static void startCaptivePortal()
{
    if (g_wifiBuzzerActive) { buzzerRawOff(); g_wifiBuzzerActive = false; }
    bool configured = anyNet();
    // With a saved network, keep the station side on (AP+STA) and go on
    // trying it: the netpack used to stay in the setup AP for good when
    // the timer's AP was not up within 30 s of boot, or was off for 60 s.
    g_apRetrySta = configured;
    WiFi.mode(configured ? WIFI_AP_STA : WIFI_AP);
    if (configured) esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);   // before it joins
    esp_wifi_set_max_tx_power(84);
    WiFi.softAP(ap_ssid, ap_password);
    // 20 MHz wide, as every WiFi here (host mode, client mode, FPV C5 Lap
    // Timer): 40 MHz covers the neighbouring channels for no gain.
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    if (configured) {
        WiFi.setAutoReconnect(false);
        staBegin(pickNetwork());
        g_apRetryMs = millis();
    }
    apModeActive = true;
    builtinLed.blink(LED_AP_INTERVAL, LED_AP_INTERVAL);
    dnsServer.start(53, "*", WiFi.softAPIP());
    LOG.printf("[ap] IP=%s\n", WiFi.softAPIP().toString().c_str());
}

static void startNetServices()
{
    MDNS.end();
    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("_elrs", "_tcp", TCP_PORT);
        MDNS.addService("http",  "_tcp", 80);
    }
    tcpServer.begin();
    LOG.printf("[tcp] listening on port %d\n", TCP_PORT);
}

// ── Host mode ─────────────────────────────────────────────────────────────────

// DNS for the devices on the netpack's WiFi (DHCP gives them 192.168.4.1):
// passed on to the phone's DNS while USB tethering is up, else answered
// "server failure" at once (no internet: the phone stops waiting).
static int g_dnsSock = -1, g_dnsUp = -1;
static struct DnsPend { uint16_t id, orig; sockaddr_in from; uint32_t ms; bool used; } g_dnsPend[16];
static uint16_t g_dnsNextId = 1;

static int udpSocket(uint16_t port)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return -1;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (sockaddr *)&a, sizeof a) < 0) { close(s); return -1; }
    fcntl(s, F_SETFL, O_NONBLOCK);
    return s;
}

// The name asked in a DNS query (lower case), or "" if it can't be read.
static String dnsQueryName(const uint8_t *buf, int n)
{
    if (n < 12 || (buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) return "";
    String name;
    int q = 12;
    while (q < n && buf[q]) {
        int len = buf[q];
        if ((len & 0xC0) || q + len + 1 >= n) return "";
        if (name.length()) name += '.';
        for (int i = 1; i <= len; i++) name += (char)tolower(buf[q + i]);
        q += len + 1;
    }
    return name;
}

static int dnsAnswer(uint8_t *buf, int n, int cap, uint32_t addr);

// Turns the query in buf into an answer: A → 192.168.4.1, anything else
// (AAAA, ...) → no records. Length of the answer, or 0.
static int dnsAnswerSelf(uint8_t *buf, int n, int cap)
{
    return dnsAnswer(buf, n, cap, (uint32_t)IPAddress(192, 168, 4, 1));
}

// The answer: A → addr; other types → no records; addr 0 → "no such name"
// (the asker then looks it up itself, by mDNS).
static int dnsAnswer(uint8_t *buf, int n, int cap, uint32_t addr)
{
    if (n < 12 || (buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) return 0;   // one question
    int q = 12;
    while (q < n && buf[q]) {
        if ((buf[q] & 0xC0) || q + buf[q] + 1 >= n) return 0;
        q += buf[q] + 1;
    }
    q += 1;                       // the name's end
    if (q + 4 > n) return 0;
    uint16_t qtype = (buf[q] << 8) | buf[q + 1], qclass = (buf[q + 2] << 8) | buf[q + 3];
    q += 4;                       // the question ends here (anything after it is dropped)
    buf[2] = 0x84 | (buf[2] & 0x01);   // response, authoritative, RD as asked
    buf[3] = addr ? 0x80 : 0x83;       // RA; no error, or NXDOMAIN
    buf[6] = buf[7] = buf[8] = buf[9] = buf[10] = buf[11] = 0;
    if (!addr || qtype != 1 || qclass != 1 || q + 16 > cap) return q;   // no records
    buf[7] = 1;                        // one answer
    uint8_t ans[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 0, 0, 0, 0};
    memcpy(ans + 12, &addr, 4);        // network order, as IPAddress holds it
    memcpy(buf + q, ans, sizeof ans);
    return q + 16;
}

static void dnsRelayBegin()
{
    if (g_dnsSock < 0) g_dnsSock = udpSocket(53);
    if (g_dnsUp < 0) g_dnsUp = udpSocket(0);
}

static void dnsRelayPoll()
{
    if (g_dnsSock < 0 || g_dnsUp < 0) return;
    uint8_t buf[512];
    uint32_t now = millis();
    for (int k = 0; k < 4; k++) {   // queries from the WiFi
        sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(g_dnsSock, buf, sizeof buf, 0, (sockaddr *)&from, &fl);
        if (n < 12) break;
        // .local names answered here: FPV C5 Lap Timer's real address (so
        // fpv-c5-lt.local opens it, not us, and works on phones without
        // mDNS), ourselves, or "no such name" (the asker uses mDNS).
        String qn = dnsQueryName(buf, n);
        if (qn.endsWith(".local")) {
            uint32_t a = 0;
            if (qn == "elrs-netpack.local") a = (uint32_t)IPAddress(192, 168, 4, 1);
            else if (qn == "fpv-c5-lt.local") a = (uint32_t)knownLapTimer();
            int m = dnsAnswer(buf, n, sizeof buf, a);
            if (m > 0) sendto(g_dnsSock, buf, m, 0, (sockaddr *)&from, fl);
            continue;
        }
        TetherInfo t;
        tetherInfo(&t);
        if (t.state != TetherState::Up) {
            // No way out: every name is us (as FPV C5 Lap Timer's access
            // point does). A computer's "is there internet?" check then gets
            // its answer at once; with "server failure" Windows kept asking
            // and sat on "checking network requirements" for a long time.
            int m = dnsAnswerSelf(buf, n, sizeof buf);
            if (m > 0) sendto(g_dnsSock, buf, m, 0, (sockaddr *)&from, fl);
            continue;
        }
        int slot = -1;
        for (int i = 0; i < 16; i++)
            if (!g_dnsPend[i].used || now - g_dnsPend[i].ms > 5000) { slot = i; break; }
        if (slot < 0) continue;
        DnsPend &d = g_dnsPend[slot];
        d.used = true; d.ms = now; d.from = from;
        d.orig = (buf[0] << 8) | buf[1];
        d.id = g_dnsNextId++;
        buf[0] = d.id >> 8; buf[1] = d.id;
        sockaddr_in up = {};
        up.sin_family = AF_INET;
        up.sin_port = htons(53);
        up.sin_addr.s_addr = t.dns.addr ? t.dns.addr : inet_addr("8.8.8.8");
        sendto(g_dnsUp, buf, n, 0, (sockaddr *)&up, sizeof up);
    }
    for (int k = 0; k < 4; k++) {   // answers from the phone's DNS
        int n = recv(g_dnsUp, buf, sizeof buf, 0);
        if (n < 12) break;
        uint16_t id = (buf[0] << 8) | buf[1];
        for (int i = 0; i < 16; i++) {
            DnsPend &d = g_dnsPend[i];
            if (!d.used || d.id != id) continue;
            buf[0] = d.orig >> 8; buf[1] = d.orig;
            sendto(g_dnsSock, buf, n, 0, (sockaddr *)&d.from, sizeof d.from);
            d.used = false;
            break;
        }
    }
}

static void startHost()
{
    if (g_wifiBuzzerActive) { buzzerRawOff(); g_wifiBuzzerActive = false; }
    dnsServer.stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    esp_wifi_set_max_tx_power(84);
    const char *pass = g_hostPass.length() >= 8 ? g_hostPass.c_str() : HOST_PASS_DEFAULT;
    // FPV C5 Lap Timer + phones / PCs, and one place to spare: the timer
    // rejoining after a drop is never turned away while a phone past the
    // limit is still being sent off (limitDevices keeps the count).
    int maxSta = g_hostPhones + 2 < HOST_MAX_STA ? g_hostPhones + 2 : HOST_MAX_STA;
    // As FPV C5 Lap Timer's access point: the address and DHCP first, then
    // the WiFi (set the other way round, DHCP restarted under a running AP).
    IPAddress ip(192, 168, 4, 1);
    WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0), IPAddress(192, 168, 4, 2), ip);   // DNS: our relay
    if (!WiFi.softAP(g_hostSsid.c_str(), pass, g_hostCh, 0, maxSta)) {
        LOG.printf("[host] ch %u refused — ch %u\n", g_hostCh, HOST_CH_DEFAULT);
        WiFi.softAP(g_hostSsid.c_str(), pass, HOST_CH_DEFAULT, 0, maxSta);
    }
    // 20 MHz wide: 40 MHz covers the neighbouring channels for no gain.
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    apModeActive = false;
    g_hostActive = true;
    builtinLed.off();
    LOG.printf("[host] WiFi %s ch %u  IP=%s\n", g_hostSsid.c_str(), g_hostCh, WiFi.softAPIP().toString().c_str());
    startNetServices();
    dnsRelayBegin();
    if (g_tetherEn) {
        // The USB port becomes the host's: the log moves to UART0 (D6).
        LOG.println("[host] USB tethering on: the log continues on D6 (UART0 TX, 115200)");
        Serial.flush();
        Serial.end();
        Serial0.begin(115200);
        g_log = &Serial0;
        if (tetherBegin()) {
            if (WiFi.AP.enableNAPT(true)) LOG.println("[host] NAPT on: the WiFi goes out through the phone");
        }
    }
    beepDouble();
}

// Host mode: at most g_hostPhones devices besides FPV C5 Lap Timer, so its
// place is always free (the access point takes g_hostPhones + 2). A device
// past the limit is sent off and kept out for 30 s (it would join again at
// once). FPV C5 Lap Timer is known by the MAC its TCP came from.
static const uint32_t HOLD_OFF_MS = 30000;
static struct { uint8_t mac[6]; uint32_t since; bool used; } g_held[4];

static void learnLapMac()
{
    // Only from FPV C5 Lap Timer's own session: RotorHazard (or anything
    // else on TCP 8080) learnt as the timer left the real one counted as a
    // phone, and sent off when the phones were at the limit.
    if (!g_hostActive || !g_peerIsLap || !tcpClient || !tcpClient.connected()) return;
    uint32_t ip = (uint32_t)tcpClient.remoteIP();
    wifi_sta_list_t sl = {};
    if (esp_wifi_ap_get_sta_list(&sl) != ESP_OK || !sl.num) return;
    esp_netif_pair_mac_ip_t pairs[HOST_MAX_STA] = {};
    int n = sl.num < HOST_MAX_STA ? sl.num : HOST_MAX_STA;
    for (int i = 0; i < n; i++) memcpy(pairs[i].mac, sl.sta[i].mac, 6);
    if (esp_netif_dhcps_get_clients_by_mac(WiFi.AP.netif(), n, pairs) != ESP_OK) return;
    for (int i = 0; i < n; i++) {
        if (pairs[i].ip.addr != ip) continue;
        if (!g_lapMacKnown || memcmp(g_lapMac, pairs[i].mac, 6)) {
            memcpy(g_lapMac, pairs[i].mac, 6);
            g_lapMacKnown = true;
            Preferences pr;   // its own handle: the web handlers use `prefs` on another task
            pr.begin("elrs", false);
            pr.putBytes("lapMac", g_lapMac, 6);
            pr.end();
            LOG.printf("[host] FPV C5 Lap Timer is " MACSTR "\n", MAC2STR(g_lapMac));
        }
        return;
    }
}

static bool isLapTimer(const uint8_t *mac) { return g_lapMacKnown && memcmp(mac, g_lapMac, 6) == 0; }

static void sendOff(const uint8_t *mac, const char *why)
{
    uint16_t aid = 0;
    if (esp_wifi_ap_get_sta_aid(mac, &aid) != ESP_OK || !aid) return;
    LOG.printf("[host] " MACSTR " sent off (%s)\n", MAC2STR(mac), why);
    esp_wifi_deauth_sta(aid);
}

static void limitDevices()
{
    static uint32_t last = 0;
    uint32_t now = millis();
    bool joined = g_apJoinEvent;
    if (!g_hostActive || (!joined && now - last < 3000)) return;
    g_apJoinEvent = false;
    last = now;
    learnLapMac();   // the session showed itself as FPV C5 Lap Timer since (0x0F03)
    wifi_sta_list_t sl = {};
    if (esp_wifi_ap_get_sta_list(&sl) != ESP_OK) return;
    // Held devices that joined again: out again while the hold lasts.
    for (int i = 0; i < sl.num; i++)
        for (auto &h : g_held)
            if (h.used && now - h.since < HOLD_OFF_MS && memcmp(h.mac, sl.sta[i].mac, 6) == 0)
                sendOff(sl.sta[i].mac, "held");
    for (auto &h : g_held)
        if (h.used && now - h.since >= HOLD_OFF_MS) h.used = false;
    // Over the limit: the newest devices go (the list is in join order).
    // Until FPV C5 Lap Timer is known (its first TCP here), phones + 1 in
    // all (it might be the newest).
    int allowed = g_lapMacKnown ? g_hostPhones : g_hostPhones + 1;
    int others = 0;
    for (int i = 0; i < sl.num; i++)
        if (!isLapTimer(sl.sta[i].mac) && ++others > allowed) {
            int slot = 0;
            for (int k = 0; k < 4; k++) {
                if (!g_held[k].used) { slot = k; break; }
                if ((int32_t)(g_held[k].since - g_held[slot].since) < 0) slot = k;
            }
            memcpy(g_held[slot].mac, sl.sta[i].mac, 6);
            g_held[slot].since = now;
            g_held[slot].used = true;
            sendOff(sl.sta[i].mac, "more phones / PCs than set");
        }
}

static void wifiConnect()
{
    // WiFi 切断に伴う TCP 切断はユーザー起因 → beepLong3 を鳴らさず静かにクリア
    g_tcpSessionActive = false;
    g_tcpEverConnected = false;
    g_apRetrySta = false;   // this call decides afresh
    tcpClient.stop();
    beepCancel();  // この後の接続待ちはブロッキングなので、鳴りっぱなしを防ぐ

    bool configured = anyNet();

    if (g_netMode == 1) {
        startHost();
        return;
    }

    if (!configured) {
        LOG.println("[wifi] not configured — starting captive portal immediately");
        startCaptivePortal();
        return;
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    // 20 MHz wide, set before joining: as host mode and FPV C5 Lap Timer.
    // A router on 40 MHz is joined on its 20 MHz part; 40 MHz would also
    // cover the neighbouring channels (towards ESP-NOW's ch 1 and the
    // pilots' ELRS) for speed the netpack doesn't need.
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    // No modem sleep as a client: dozing between beacons, the station misses
    // some and the router (or the ESP) takes it for gone (a short drop that
    // used to set off the WiFi warning while the WiFi was fine). FPV C5 Lap
    // Timer does the same.
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(false);  // 自動再接続は checkWifiState() で制御
    // Older routers (WPA only): the library refuses below WPA2 by default.
    WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);
    esp_wifi_set_max_tx_power(84);
    staBegin(pickNetwork());
    builtinLed.on();

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
        delay(200);  // 500ms → 200ms: 接続成功の検出を高速化
        LOG.print('.');
    }
    LOG.println();

    if (WiFi.status() == WL_CONNECTED) {
        if (g_wifiBuzzerActive) { buzzerRawOff(); g_wifiBuzzerActive = false; }
        apModeActive = false;
        g_wifiLostMs = 0;
        dnsServer.stop();
        builtinLed.off();
        LOG.printf("[wifi] connected IP=%s  AP %s ch %d %d dBm\n", WiFi.localIP().toString().c_str(),
                   WiFi.BSSIDstr().c_str(), WiFi.channel(), WiFi.RSSI());
        startNetServices();
        beepDouble();
    } else {
        LOG.println("[wifi] 59 s elapsed — starting captive portal");
        startCaptivePortal();
    }
}

// Setup AP is up: join the saved network as soon as it is there, then
// drop the AP. Retries pause while someone is on the setup AP (a scan
// would disturb the setup page).
static void checkApRetry()
{
    if (!g_apRetrySta) return;
    if (WiFi.status() == WL_CONNECTED) {
        dnsServer.stop();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        apModeActive = false;
        g_apRetrySta = false;
        g_wifiLostMs = 0;
        builtinLed.off();
        LOG.printf("[wifi] joined from the setup AP  IP=%s\n", WiFi.localIP().toString().c_str());
        startNetServices();
        beepDouble();
        return;
    }
    uint32_t now = millis();
    if (now - g_apRetryMs < AP_RETRY_INTERVAL_MS) return;
    g_apRetryMs = now;
    if (WiFi.softAPgetStationNum() > 0) return;
    LOG.println("[wifi] setup AP: retrying the saved network");
    if (g_staAuto) staBegin(pickNetwork());
    else WiFi.reconnect();
}

static void checkWifiState()
{
    if (g_hostActive) return;
    if (apModeActive) { checkApRetry(); return; }

    if (WiFi.status() == WL_CONNECTED) {
        if (g_wifiLostMs != 0) {
            // WiFi 復旧 → ブザー即停止 + TCP サーバー・mDNS 再起動
            if (g_wifiBuzzerActive) { buzzerRawOff(); g_wifiBuzzerActive = false; }
            LOG.printf("[wifi] reconnected after %lu ms  IP=%s\n", (unsigned long)(millis() - g_wifiLostMs),
                       WiFi.localIP().toString().c_str());
            g_wifiLostMs = 0;
            startNetServices();
            if (g_wifiAlarmed) beepDouble();   // a short drop passes without a sound
            g_wifiAlarmed = false;
        }
        return;
    }

    uint32_t now = millis();
    static uint32_t lastReconnectMs = 0;

    if (g_wifiLostMs == 0) {
        g_wifiLostMs    = now;
        lastReconnectMs = now;
        LOG.printf("[wifi] disconnected (reason %u) — reconnecting\n", g_wifiLostReason);
        // WiFi 切断起因の TCP 切断は静かにクリア
        g_tcpSessionActive = false;
        g_tcpEverConnected = false;
        tcpClient.stop();
        g_wifiAlarmed = false;
        WiFi.reconnect();
        return;
    }

    // Still without WiFi after WIFI_ALARM_MS: now the warning (5 s of
    // buzzer, the LED blinking fast; not over the voltage alarm).
    if (!g_wifiAlarmed && now - g_wifiLostMs >= WIFI_ALARM_MS) {
        g_wifiAlarmed = true;
        LOG.println("[wifi] still disconnected: warning");
        if (g_buzzerEnabled && !g_alarmActive) {
            buzzerRawOn();
            g_wifiBuzzerActive  = true;
            g_wifiBuzzerStartMs = now;
        }
    }

    // 60 秒経過 → AP モード
    if (now - g_wifiLostMs >= 60000) {
        if (g_wifiBuzzerActive) { buzzerRawOff(); g_wifiBuzzerActive = false; }
        g_wifiLostMs = 0;
        g_wifiAlarmed = false;
        LOG.println("[wifi] 60 s without connection — starting captive portal");
        startCaptivePortal();
        return;
    }

    // 10 秒ごとに再接続を試みる
    if (now - lastReconnectMs >= 10000) {
        lastReconnectMs = now;
        LOG.println("[wifi] retrying reconnect");
        WiFi.reconnect();
    }
}

// ── Web handlers ──────────────────────────────────────────────────────────────

// The first page: the QR codes in host mode (no password), else WiFi.
static void handleRoot()
{
    if (g_hostActive) { webServer.send(200, "text/html", pageQr()); return; }
    if (needLogin()) return;
    webServer.send(200, "text/html", pageWifi());
}
static void handleWifiGet()    { if (needLogin()) return; webServer.send(200, "text/html", pageWifi()); }
static void handleVoltageGet() { if (needLogin()) return; webServer.send(200, "text/html", pageVoltage()); }

static void handleLangToggle()
{
    g_langJa = !g_langJa;
    prefs.begin("elrs", false);
    prefs.putBool("langJa", g_langJa);
    prefs.end();
    String ref = webServer.header("Referer");
    webServer.sendHeader("Location", ref.length() ? ref : "/", true);
    webServer.send(302, "text/plain", "");
}

static void handleWifiPost()
{
    if (needLogin()) return;
    uint8_t mode   = webServer.arg("mode") == "1" ? 1 : 0;    // the page's choice
    int     sw     = webServer.hasArg("swMode") ? 1 : 0;
    String  hssid  = webServer.arg("hssid");
    String  hpass  = webServer.arg("hpass");
    int     hch    = webServer.arg("hch").toInt();
    int     hmax   = webServer.arg("hmax").toInt();
    if (hmax < 1 || hmax > HOST_PHONES_MAX) hmax = HOST_PHONES_DEFAULT;
    bool    tether = webServer.hasArg("tether");
    hssid.trim();
    if (hch < 6 || hch > 11) hch = HOST_CH_DEFAULT;

    // The client's networks: a cleared SSID deletes the network; a blank
    // password keeps the saved one.
    String ns[STA_NETS], np[STA_NETS];
    bool anyNew = false;
    for (int i = 0; i < STA_NETS; i++) {
        ns[i] = webServer.arg("s" + String(i));
        ns[i].trim();
        String pw = webServer.arg("p" + String(i));
        np[i] = ns[i].length() ? (pw.length() ? pw : g_staPass[i]) : String();
        if (ns[i].length()) anyNew = true;
    }
    int sel = webServer.arg("sel").toInt();
    if (sel < 0 || sel >= STA_NETS || !ns[sel].length())
        for (sel = 0; sel < STA_NETS - 1 && !ns[sel].length(); sel++) {}
    bool staAuto = webServer.arg("staAuto") == "1";
    bool ipFix = webServer.arg("ipfix") == "1";
    IPAddress ip, mask, gw, dns;
    if (ipFix) {
        String d = webServer.arg("dns");
        d.trim();
        if (!ip.fromString(webServer.arg("ip")) || !mask.fromString(webServer.arg("mask")) || !gw.fromString(webServer.arg("gw")) ||
            (d.length() && !dns.fromString(d)) || !(uint32_t)ip) {
            webServer.send(200, "text/html", pageWifi(L("Fixed address: enter the IP address, subnet mask and gateway (like 192.168.1.50).",
                                                         "固定アドレス：IP アドレス・サブネットマスク・ゲートウェイを入れてください（例 192.168.1.50）。")));
            return;
        }
    }
    if (mode == 0 && !sw && !anyNew) {
        webServer.send(200, "text/html", pageWifi(L("SSID cannot be empty.", "SSIDを入力してください。")));
        return;
    }
    if (mode == 1 && hssid.length() == 0) {
        webServer.send(200, "text/html", pageWifi(L("The host SSID cannot be empty.", "ホストの SSID を入力してください。")));
        return;
    }
    if (hpass.length() && (hpass.length() < 8 || hpass.length() > 63)) {
        webServer.send(200, "text/html", pageWifi(L("The host password needs 8-63 characters.", "ホストのパスワードは 8〜63 文字にしてください。")));
        return;
    }
    // A change of mode, of the host's WiFi or of tethering: restart (the
    // WiFi and the USB port start afresh). The client's network alone:
    // join it now, as before.
    // With the mode switch set, the switch decides the mode (at start-up):
    // the page's choice is only kept for when it's taken off.
    uint8_t runMode = sw ? g_netMode : mode;
    bool restart = sw != g_swMode || runMode != g_netMode || tether != g_tetherEn ||
                   (runMode == 1 && (hssid != g_hostSsid || hpass.length() || hch != g_hostCh || hmax != g_hostPhones));
    prefs.begin("elrs", false);
    for (int i = 0; i < STA_NETS; i++) {
        g_staSsid[i] = ns[i];
        g_staPass[i] = np[i];
        prefs.putString(("ss" + String(i)).c_str(), ns[i]);
        prefs.putString(("sp" + String(i)).c_str(), np[i]);
    }
    g_staSel = sel;
    g_staAuto = staAuto;
    g_ipFix = ipFix;
    if (ipFix) { g_ipAddr = ip; g_ipMask = mask; g_ipGw = gw; g_ipDns = dns; }
    prefs.putBool("nets", true);   // the 5 networks are saved (the old single one moved)
    prefs.putUChar("staSel", sel);
    prefs.putBool("staAuto", staAuto);
    prefs.putBool("ipFix", ipFix);
    prefs.putUInt("ip", (uint32_t)g_ipAddr);
    prefs.putUInt("mask", (uint32_t)g_ipMask);
    prefs.putUInt("gw", (uint32_t)g_ipGw);
    prefs.putUInt("dns", (uint32_t)g_ipDns);
    prefs.putBool("configured", anyNet());
    prefs.putUChar("netMode", mode);
    prefs.putUChar("swMode", sw);
    {
        uint16_t lm = webServer.arg("loginmin").toInt();
        for (uint16_t m : LOGIN_CHOICES)
            if (m == lm) { g_loginMin = lm; prefs.putUShort("loginMin", lm); }
    }
    String adp = webServer.arg("admpass");
    if (adp.length()) { prefs.putString("admPass", adp); g_admPass = adp; }
    prefs.putString("hSsid", hssid.length() ? hssid : g_hostSsid);
    if (hpass.length()) prefs.putString("hPass", hpass);
    prefs.putUChar("hCh", hch);
    prefs.putUChar("hMax", hmax);
    prefs.putBool("tether", tether);
    prefs.end();
    g_netModeSet = mode;
    g_netMode = runMode;
    if (hssid.length()) g_hostSsid = hssid;
    if (hpass.length()) g_hostPass = hpass;
    g_hostCh = hch;
    g_hostPhones = hmax;

    if (restart) {
        String p = htmlHead(L("WiFi Settings", "WiFi設定"));
        p += "<div class='msg'>";
        if (sw) {
            p += L("Saved. Restarting: the mode switch decides host or client mode.",
                   "保存しました。再起動します。ホストモードかクライアントモードかは物理スイッチで決まります。");
        } else {
            p += mode == 1 ? L("Saved. Restarting in host mode: join the WiFi ", "保存しました。ホストモードで再起動します。WiFi ")
                           : L("Saved. Restarting in client mode.", "保存しました。クライアントモードで再起動します。");
            if (mode == 1) { p += htmlEsc(g_hostSsid); p += L(" and open http://192.168.4.1", " に接続して http://192.168.4.1 を開いてください"); }
        }
        p += "</div></body></html>";
        webServer.send(200, "text/html", p);
        g_beepReq = true;
        webServer.restartSoon(800);
        return;
    }
    if (runMode == 1) {   // host mode, nothing of it changed
        webServer.send(200, "text/html", pageWifi(L("Saved.", "保存しました。")));
        g_beepReq = true;
        return;
    }
    webServer.send(200, "text/html",
        pageWifi(L("Saved. Connecting\xe2\x80\xa6", "保存しました。接続中…")));
    g_beepReq = true;
    g_reconnectAt = (millis() + 420) | 1;   // loop() joins the network (not in the handler)
}

// The divider ratio from a voltage measured with a meter: a fresh reading
// of the ADC pin (the same averaging as updateVoltage), ratio = measured /
// pin voltage.
static void handleVoltageCal()
{
    if (needLogin()) return;
    float actual = webServer.arg("actual").toFloat();
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(VBAT_ADC_PIN);
    float pinV = (sum / 16.0f) / 1000.0f;
    String msg;
    if (actual < 0.5f || actual > 30.0f) {
        msg = L("Enter the measured voltage (0.5-30 V).", "実際の電圧を入れてください（0.5〜30 V）。");
    } else if (pinV < 0.2f) {
        msg = L("No voltage on the divider pin (check the wiring): not calibrated.",
                "分圧回路のピンに電圧がありません（配線を確認してください）。校正していません。");
    } else {
        float ratio = actual / pinV;
        if (ratio < 1.0f || ratio > 10.0f) {
            char b[160];
            snprintf(b, sizeof b, "%s %.3f", L("The ratio would be out of range (1-10), not calibrated:",
                                               "分圧比が範囲（1〜10）を外れるため、校正していません："), ratio);
            msg = b;
        } else {
            g_vbatRatio = ratio;
            g_currentVoltage = pinV * ratio;
            prefs.begin("elrs", false);
            prefs.putFloat("vbatRatio", g_vbatRatio);
            prefs.end();
            char b[160];
            snprintf(b, sizeof b, "%s %.3f", L("Calibrated: divider ratio", "校正しました：分圧比"), ratio);
            msg = b;
            LOG.printf("[vbat] calibrated: %.2f V measured, pin %.3f V, ratio %.3f\n", actual, pinV, ratio);
            g_beepReq = true;
        }
    }
    webServer.send(200, "text/html", pageVoltage(msg));
}

static void handleVoltagePost()
{
    if (needLogin()) return;
    String ratioStr = webServer.arg("ratio");
    String alarmStr = webServer.arg("alarm");

    float newRatio = ratioStr.length() ? ratioStr.toFloat() : g_vbatRatio;
    float newAlarm = alarmStr.length() ? alarmStr.toFloat() : g_alarmVoltage;
    if (newRatio < 1.0f) newRatio = 1.0f;
    if (newAlarm < 2.0f) newAlarm = 2.0f;

    g_vbatRatio        = newRatio;
    g_alarmVoltage     = newAlarm;
    g_vbatAlarmEnabled = webServer.hasArg("vbatAlarmEn");
    g_buzzerEnabled    = webServer.hasArg("buzzerEn");
    g_ledEnabled       = webServer.hasArg("ledEn");

    // アラームが無効になったら即解除
    if (!g_vbatAlarmEnabled && g_alarmActive) {
        g_alarmActive = false;
        alarmOff();
    }

    prefs.begin("elrs", false);
    prefs.putFloat("vbatRatio",   g_vbatRatio);
    prefs.putFloat("alarmV",      g_alarmVoltage);
    prefs.putBool("vbatAlarmEn",  g_vbatAlarmEnabled);
    prefs.putBool("buzzerEn",     g_buzzerEnabled);
    prefs.putBool("ledEn",        g_ledEnabled);
    prefs.end();

    webServer.send(200, "text/html",
        pageVoltage(L("Settings saved.", "設定を保存しました。")));
    g_beepReq = true;
}

static void handleNotFound()
{
    webServer.sendHeader("Location", "/", true);
    webServer.send(302, "text/plain", "");
}

// ── Prefs / Voltage / LED ─────────────────────────────────────────────────────

// The mode switch, read once at boot: it decides host or client mode over
// the page's choice. Changing it later takes effect at the next restart.
static void readModeSwitch()
{
    if (!g_swMode) return;
    pinMode(MODE_SW_PIN, INPUT_PULLUP);
    delay(20);
    int low = 0;
    for (int i = 0; i < 5; i++) { low += digitalRead(MODE_SW_PIN) == LOW; delay(5); }
    bool closed = low >= 3;       // D4 to GND: host mode
    g_swOn = closed;
    g_netMode = closed ? 1 : 0;
    LOG.printf("[boot] mode switch %s → %s mode\n", closed ? "ON" : "OFF", g_netMode ? "host" : "client");
}

static void loadPrefs()
{
    prefs.begin("elrs", true);
    g_vbatRatio     = prefs.getFloat("vbatRatio", VBAT_DEFAULT_RATIO);
    g_alarmVoltage  = prefs.getFloat("alarmV",    VBAT_DEFAULT_ALARM_V);
    g_vbatAlarmEnabled = prefs.getBool("vbatAlarmEn", false);
    g_buzzerEnabled    = prefs.getBool("buzzerEn",    true);
    g_ledEnabled       = prefs.getBool("ledEn",       true);
    g_langJa           = prefs.getBool("langJa",      true);
    g_netMode          = prefs.getUChar("netMode",    0) == 1 ? 1 : 0;
    g_netModeSet       = g_netMode;
    g_swMode           = prefs.getUChar("swMode",     0);
    g_swMode = g_swMode ? 1 : 0;
    g_admPass          = prefs.getString("admPass",   ADMIN_PASS_DEFAULT);
    if (!g_admPass.length()) g_admPass = ADMIN_PASS_DEFAULT;
    g_loginMin = 720;
    {
        uint16_t lm = prefs.getUShort("loginMin", 720);
        for (uint16_t m : LOGIN_CHOICES)
            if (m == lm) g_loginMin = lm;
    }   // (test builds had 1 / 2 for A / B contacts: both "used")
    g_hostSsid         = prefs.getString("hSsid",     HOST_SSID_DEFAULT);
    g_hostPass         = prefs.getString("hPass",     HOST_PASS_DEFAULT);
    g_hostCh           = prefs.getUChar("hCh",        HOST_CH_DEFAULT);
    g_tetherEn         = prefs.getBool("tether",      false);
    if (g_hostCh < 6 || g_hostCh > 11) g_hostCh = HOST_CH_DEFAULT;
    g_hostPhones       = prefs.getUChar("hMax",       HOST_PHONES_DEFAULT);
    if (g_hostPhones < 1 || g_hostPhones > HOST_PHONES_MAX) g_hostPhones = HOST_PHONES_DEFAULT;
    g_lapMacKnown      = prefs.getBytes("lapMac", g_lapMac, 6) == 6;
    if (!g_hostSsid.length()) g_hostSsid = HOST_SSID_DEFAULT;
    if (prefs.getBool("nets", false)) {
        for (int i = 0; i < STA_NETS; i++) {
            g_staSsid[i] = prefs.getString(("ss" + String(i)).c_str(), "");
            g_staPass[i] = prefs.getString(("sp" + String(i)).c_str(), "");
        }
    } else if (prefs.getBool("configured", false)) {
        // Before v2.3: one network ("ssid" / "wifiPass"); it becomes the first.
        g_staSsid[0] = prefs.getString("ssid", WIFI_SSID);
        g_staPass[0] = prefs.getString("wifiPass", WIFI_PASSWORD);
    }
    g_staSel  = prefs.getUChar("staSel", 0);
    if (g_staSel >= STA_NETS) g_staSel = 0;
    g_staAuto = prefs.getBool("staAuto", false);
    g_ipFix   = prefs.getBool("ipFix", false);
    g_ipAddr  = IPAddress(prefs.getUInt("ip", 0));
    g_ipMask  = IPAddress(prefs.getUInt("mask", (uint32_t)IPAddress(255, 255, 255, 0)));
    g_ipGw    = IPAddress(prefs.getUInt("gw", 0));
    g_ipDns   = IPAddress(prefs.getUInt("dns", 0));
    prefs.end();
}

static void updateVoltage()
{
    if (millis() - g_lastVbatMs < 1000) return;
    g_lastVbatMs = millis();

    // analogReadMilliVolts uses built-in ADC calibration for accurate reading
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(VBAT_ADC_PIN);
    float pinV = (sum / 8.0f) / 1000.0f;  // mV → V
    g_currentVoltage = pinV * g_vbatRatio;

    bool alarm = g_vbatAlarmEnabled
                 && g_currentVoltage >= 0.5f
                 && g_currentVoltage < g_alarmVoltage;
    if (alarm != g_alarmActive) {
        g_alarmActive = alarm;
        if (alarm) alarmOn(); else alarmOff();
        LOG.printf("[vbat] %.2f V — alarm %s\n", g_currentVoltage, alarm ? "ON" : "OFF");
    }
}

static void updateLed()
{
    builtinLed.handleLed(millis());
    if (!apModeActive) builtinLed.off();
}

// LED_NOTIFY_PIN 輝度制御（analogWrite PWM）
// Priority: AP点滅 > 切断点滅 > アラーム > ハートビート > 消灯
// ※ g_ledEnabled=false のみ完全消灯。それ以外は NL_MIN を下限に IC 給電を維持
static void updateNotifyLed()
{
    if (g_beep.active) return;           // 通知ビープ中は beeper が LED を制御
    if (!g_ledEnabled) { nlWrite(0); return; }

    uint32_t now = millis();

    // AP モード → 0 ↔ 255 の 500ms ゆっくり点滅
    if (apModeActive) {
        nlWrite((now % 1000 < 500) ? 255 : 0);
        return;
    }

    // WiFi 未接続（5 秒以上）→ 0 ↔ 255 の 80ms 高速点滅（ホストモードは自分の WiFi なので対象外）
    if (!g_hostActive && g_wifiLostMs && g_wifiAlarmed) {
        nlWrite((now % 160 < 80) ? 255 : 0);
        return;
    }

    // TCP セッション切断（一度接続後に切れた場合）→ 300ms 中速点滅
    if (!g_tcpSessionActive && g_tcpEverConnected) {
        nlWrite((now % 600 < 300) ? 255 : 0);
        return;
    }

    // 電圧アラーム → 全輝度点灯
    if (g_alarmActive) { nlWrite(255); return; }

    // 通常（STA 接続中）→ PWM ハートビート（2 秒周期・ダブルパルス、うっすら発光）
    // 第1拍: 0-100ms 立ち上がり(0→80) → 100-350ms フェードアウト(80→0)
    // 第2拍: 450-550ms 立ち上がり(0→80) → 550-800ms フェードアウト(80→0)
    // 休止 : 800-2000ms（消灯）
    static const uint8_t HB_PEAK = 10;
    uint32_t phase = now % 2000;
    uint8_t  bri   = 0;
    if      (phase < 100)  bri = (uint8_t)(phase          * HB_PEAK / 100);
    else if (phase < 350)  bri = (uint8_t)((350 - phase)  * HB_PEAK / 250);
    else if (phase < 450)  bri = 0;
    else if (phase < 550)  bri = (uint8_t)((phase - 450)  * HB_PEAK / 100);
    else if (phase < 800)  bri = (uint8_t)((800 - phase)  * HB_PEAK / 250);
    else                   bri = 0;
    nlWrite(bri);
}

// ── Arduino entry points ──────────────────────────────────────────────────────

void setup()
{
    Serial.begin(115200);
    LOG.printf("[boot] XIAO ESP32-S3 WiFi bridge + Web UI  %s (%s)\n", fwVersion(), fwBuild());

    analogReadResolution(12);

    builtinLed.init(LED_BUILTIN_PIN, true);  // active-LOW
    pinMode(BUZZER_PIN_POS, OUTPUT);
    if (BUZZER_PIN_NEG >= 0) pinMode(BUZZER_PIN_NEG, OUTPUT);
    pinMode(LED_NOTIFY_PIN, OUTPUT);
    nlWrite(0);                              // GPIO9 初期消灯（analogWrite でPWM初期化）
    buzzerRawOff();

    // VBAT ピンのみ ADC 設定（全ピン一括は GPIO9 を入力化してしまうため不可）
    analogSetPinAttenuation(VBAT_ADC_PIN, ADC_11db);

    // RX バッファを拡張（既定 256B）。loop() が一時的に詰まっても
    // Wrover からのテレメトリ MSP を取りこぼさないようにする。
    uart.setRxBufferSize(1024);
    uart.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

    loadPrefs();
    readModeSwitch();

    // WiFi 切断を即座に検知してフラグを立てる（loop() 先頭で FIN 送信）
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
        g_apJoinEvent = true;
        g_apJoinMs = millis();
    }, ARDUINO_EVENT_WIFI_AP_STACONNECTED);
    // How long a device waited for its address (a slow "checking network
    // requirements" on a PC shows here).
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
        g_ipLogIp = info.wifi_ap_staipassigned.ip.addr;
        g_ipLogMs = millis() - g_apJoinMs;
        g_ipLogEvent = true;
    }, ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED);

    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
        memcpy(g_apLeftMac, info.wifi_ap_stadisconnected.mac, 6);
        g_apLeftEvent = true;
    }, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);

    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
        g_wifiLostReason = info.wifi_sta_disconnected.reason;
        g_wifiLostEvent = true;
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    wifiConnect();

    webServer.on("/",                    HTTP_GET,  handleRoot);
    webServer.on("/wifi",                HTTP_GET,  handleWifiGet);
    webServer.on("/wifi",                HTTP_POST, handleWifiPost);
    webServer.on("/voltage",             HTTP_GET,  handleVoltageGet);
    webServer.on("/qr",                  HTTP_GET,  [] { webServer.send(200, "text/html", pageQr()); });   // no password
    webServer.on("/login",               HTTP_GET,  [] { webServer.send(200, "text/html", pageLogin()); });
    webServer.on("/login",               HTTP_POST, handleLoginPost);
    webServer.on("/logout",              HTTP_GET,  handleLogout);
    webServer.on("/voltage",             HTTP_POST, handleVoltagePost);
    webServer.on("/voltage/cal",         HTTP_POST, handleVoltageCal);
    webServer.on("/lang",                HTTP_GET,  handleLangToggle);
    // Firmware update: from the internet (a router, or USB tethering) or a file.
    fwUpdateRoutes(
        [] { return (!g_hostActive && !apModeActive && WiFi.status() == WL_CONNECTED) || tetherNetif() != nullptr; },
        htmlHead, [] { return g_langJa; }, authed);
    webServer.on("/generate_204",        HTTP_GET,  handleRoot);
    webServer.on("/hotspot-detect.html", HTTP_GET,  handleRoot);
    webServer.on("/ncsi.txt",            HTTP_GET,  handleRoot);
    webServer.on("/favicon.ico",         HTTP_GET,  [] { webServer.send(204, "text/plain", ""); });
    webServer.onNotFound(handleNotFound);
    webServer.begin();
    LOG.println("[web] HTTP server on port 80");
    LOG.println("[boot] ready");
}

void loop()
{
    // WiFi 切断イベントを最速で処理 — WiFi がまだ部分的に生きている間に FIN を送信
    if (g_wifiLostEvent) {
        g_wifiLostEvent = false;
        tcpClient.stop();
        LOG.println("[tcp] closed — WiFi lost");
    }

    checkWifiState();
    updateBeep();
    updateWifiBuzzer();
    if (apModeActive) dnsServer.processNextRequest();
    if (g_hostActive) dnsRelayPoll();
    limitDevices();
    if (g_ipLogEvent) {
        g_ipLogEvent = false;
        LOG.printf("[host] address " IPSTR " given %lu ms after joining\n", IP2STR((esp_ip4_addr_t *)&g_ipLogIp),
                   (unsigned long)g_ipLogMs);
    }
    webServer.loop();
    if (g_beepReq) { g_beepReq = false; beepShort(); }
    if (g_reconnectAt && (int32_t)(millis() - g_reconnectAt) >= 0) { g_reconnectAt = 0; wifiConnect(); }
    g_mspPeer = (tcpClient && tcpClient.connected()) ? (uint32_t)tcpClient.remoteIP() : 0;
    probeRotorHazard();
    if (g_hostActive || WiFi.status() == WL_CONNECTED) lookUpLapTimer();
    updateLed();
    updateVoltage();
    updateNotifyLed();

    // Host mode: FPV C5 Lap Timer left our WiFi → its session is over now
    // (the address its lease had is the one its TCP came from).
    if (g_apLeftEvent) {
        g_apLeftEvent = false;
        if (g_hostActive && g_tcpSessionActive && tcpClient && tcpClient.connected()) {
            esp_netif_pair_mac_ip_t pair = {};
            memcpy(pair.mac, g_apLeftMac, 6);
            if (esp_netif_dhcps_get_clients_by_mac(WiFi.AP.netif(), 1, &pair) == ESP_OK &&
                pair.ip.addr && pair.ip.addr == (uint32_t)tcpClient.remoteIP()) {
                LOG.println("[tcp] FPV C5 Lap Timer left the WiFi");
                tcpClient.stop();   // → "session lost" below: buzzer and LED
            }
        }
    }

    // TCP セッション切断検知（FPV C5 Lap Timer との接続が切れた）→ 長音 5 回、LED 中速点滅（再接続まで）
    if (g_tcpSessionActive && (!tcpClient || !tcpClient.connected())) {
        g_tcpSessionActive = false;
        tcpClient.stop();   // TCP リソース解放 → サーバーが即座に新規接続を受け付けられるように
        LOG.println("[tcp] session lost");
        beepLong3();
    }
    // 新規クライアント受け付け。接続中でも新しい接続を優先する:
    // the timer rebooted or rejoined, and its old connection is dead even
    // if it still looks open here (keepalive needs ~11 s to notice).
    {
        WiFiClient c = tcpServer.accept();
        if (c) {
            if (tcpClient && tcpClient.connected()) {
                tcpClient.stop();
                LOG.println("[tcp] replaced by a new connection");
            }
            setTcpKeepalive(c.fd());
            tcpClient = c;
            // A frame the old connection left half read must not swallow
            // the start of the new one (its first frame, the version
            // request, went missing and the host's connect check failed).
            mspFromTcp.markPacketReceived();
            g_tcpSessionActive = true;
            g_tcpEverConnected = true;
            LOG.println("[tcp] session started");
            g_peerIsLap = (uint32_t)c.remoteIP() == g_lapIp;   // until it shows itself
            learnLapMac();
            beepShort();
        }
    }

    if (tcpClient && tcpClient.connected()) {
        while (tcpClient.available()) {
            uint8_t b = tcpClient.read();
            if (mspFromTcp.processReceivedByte(b)) {
                mspPacket_t *pkt = mspFromTcp.getReceivedPacket();
                handlePacketFromTcp(pkt);
                mspFromTcp.markPacketReceived();
            }
        }
    }

    while (uart.available()) {
        uint8_t b = uart.read();
        if (mspFromUart.processReceivedByte(b)) {
            mspPacket_t *pkt = mspFromUart.getReceivedPacket();
            handlePacketFromUart(pkt);
            mspFromUart.markPacketReceived();
        }
    }
}
