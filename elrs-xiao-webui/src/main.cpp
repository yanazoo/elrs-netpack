#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HardwareSerial.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include "esp_wifi.h"
#include "config.h"
#include "led.h"
#include "msp.h"
#include "msptypes.h"


// ── TCP bridge globals ────────────────────────────────────────────────────────

static WiFiServer     tcpServer(TCP_PORT);
static WiFiClient     tcpClient;
static MSP            mspFromTcp;
static MSP            mspFromUart;
static HardwareSerial uart(1);

// ── Web / AP globals ──────────────────────────────────────────────────────────

static WebServer   webServer(80);
static Preferences prefs;
static DNSServer   dnsServer;

static bool     apModeActive        = false;
static uint32_t g_wifiLostMs        = 0;

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

// ── i18n helper ───────────────────────────────────────────────────────────────

static const char *L(const char *en, const char *ja)
{
    return g_langJa ? ja : en;
}

// ── buzzer / LED helpers ──────────────────────────────────────────────────────

static void buzzerRawOn()  { digitalWrite(BUZZER_PIN_POS, HIGH); digitalWrite(BUZZER_PIN_NEG, LOW); }
static void buzzerRawOff() { digitalWrite(BUZZER_PIN_POS, LOW);  digitalWrite(BUZZER_PIN_NEG, LOW); }

// アラーム制御（ブザーのみ — LED は updateNotifyLed() が一元管理）
static void alarmOn()  { if (g_buzzerEnabled) buzzerRawOn(); }
static void alarmOff() { buzzerRawOff(); }

// 通知ビープ（LED は analogWrite で直接制御、終了後 updateNotifyLed が自動復元）
static void beepShort()
{
    if (!g_buzzerEnabled && !g_ledEnabled) return;
    if (g_buzzerEnabled) buzzerRawOn();
    if (g_ledEnabled)    nlWrite(255);
    delay(80);
    buzzerRawOff();
    nlWrite(0);
}

static void beepDouble()
{
    if (!g_buzzerEnabled && !g_ledEnabled) return;
    for (int i = 0; i < 2; i++) {
        if (g_buzzerEnabled) buzzerRawOn();
        if (g_ledEnabled)    nlWrite(255);
        delay(80);
        buzzerRawOff();
        nlWrite(0);
        if (i == 0) delay(120);
    }
}

// TCP セッション切断警告: 長いビープ 3 回
static void beepLong3()
{
    for (int i = 0; i < 3; i++) {
        if (g_buzzerEnabled) buzzerRawOn();
        if (g_ledEnabled)    nlWrite(255);
        delay(500);
        buzzerRawOff();
        nlWrite(0);
        if (i < 2) delay(200);
    }
}

// WiFi 切断ブザーの 5 秒タイムアウト管理
static void updateWifiBuzzer()
{
    if (!g_wifiBuzzerActive) return;
    if (millis() - g_wifiBuzzerStartMs >= 5000) {
        buzzerRawOff();
        g_wifiBuzzerActive = false;
    }
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
    sendMspToUart(pkt);
}

static void handlePacketFromUart(mspPacket_t *pkt)
{
    sendMspToTcp(pkt);
}

// ── HTML helpers ──────────────────────────────────────────────────────────────

static String rssiBar()
{
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
    h += "<title>"; h += title; h += "</title>";
    h += "<style>"
         "*{box-sizing:border-box}"
         "body{background:#181818;color:#d4d4d4;font-family:sans-serif;"
         "max-width:480px;margin:40px auto;padding:0 16px}"
         "h1{font-size:1.25em;color:#f0f0f0;margin:0 0 2px}"
         "h2{font-size:1.05em;color:#aaa;margin:12px 0 4px}"
         ".topbar{display:flex;align-items:baseline;justify-content:space-between;flex-wrap:wrap}"
         ".rssi{font-size:.78em;color:#666;letter-spacing:.5px}"
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
         "</style></head><body>";
    h += "<div class='topbar'><h1>ELRS Netpack</h1>";
    h += "<span class='rssi'>"; h += rssiBar(); h += "</span></div>";
    h += "<nav>";
    h += "<a href='/wifi'>WiFi</a>";
    h += "<a href='/voltage'>"; h += L("Voltage", "電圧"); h += "</a>";
    h += "<a class='lang' href='/lang'>"; h += (g_langJa ? "EN" : "JA"); h += "</a>";
    h += "</nav><hr>";
    return h;
}

static String nvsSsid()
{
    prefs.begin("elrs", true);
    String s = prefs.getString("ssid", WIFI_SSID);
    prefs.end();
    return s;
}

static String pageWifi(const String &msg = "")
{
    String p = htmlHead(L("WiFi Settings", "WiFi設定"));
    p += "<h2>"; p += L("WiFi Settings", "WiFi設定"); p += "</h2>";
    if (msg.length()) { p += "<div class='msg'>"; p += msg; p += "</div>"; }
    p += "<form method='POST' action='/wifi'>";
    p += "<label>SSID<input type='text' name='ssid' value='";
    p += nvsSsid(); p += "'></label>";
    p += "<label>"; p += L("Password", "パスワード");
    p += "<input type='password' name='pass' placeholder='";
    p += L("(leave blank to keep)", "(変更しない場合は空欄)");
    p += "'></label>";
    p += "<button type='submit'>"; p += L("Save &amp; Connect", "保存して接続"); p += "</button>";
    p += "</form></body></html>";
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
    p += "<input type='number' name='ratio' step='0.01' min='1' value='";
    snprintf(buf, sizeof(buf), "%.2f", g_vbatRatio);
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
    p += "</form></body></html>";
    return p;
}

// ── WiFi / AP ─────────────────────────────────────────────────────────────────

static void startCaptivePortal()
{
    if (g_wifiBuzzerActive) { buzzerRawOff(); g_wifiBuzzerActive = false; }
    WiFi.mode(WIFI_AP);
    esp_wifi_set_max_tx_power(84);
    WiFi.softAP(ap_ssid, ap_password);
    apModeActive = true;
    builtinLed.blink(LED_AP_INTERVAL, LED_AP_INTERVAL);
    dnsServer.start(53, "*", WiFi.softAPIP());
    Serial.printf("[ap] IP=%s\n", WiFi.softAPIP().toString().c_str());
}

static void startNetServices()
{
    MDNS.end();
    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("_elrs", "_tcp", TCP_PORT);
        MDNS.addService("http",  "_tcp", 80);
    }
    tcpServer.begin();
    Serial.printf("[tcp] listening on port %d\n", TCP_PORT);
}

static void wifiConnect()
{
    // WiFi 切断に伴う TCP 切断はユーザー起因 → beepLong3 を鳴らさず静かにクリア
    g_tcpSessionActive = false;
    g_tcpEverConnected = false;
    tcpClient.stop();

    prefs.begin("elrs", true);
    String ssid       = prefs.getString("ssid",     WIFI_SSID);
    String pass       = prefs.getString("wifiPass", WIFI_PASSWORD);
    bool   configured = prefs.getBool("configured", false);
    prefs.end();

    if (!configured) {
        Serial.println("[wifi] not configured — starting captive portal immediately");
        startCaptivePortal();
        return;
    }

    Serial.printf("[wifi] connecting to %s\n", ssid.c_str());
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);  // 自動再接続は checkWifiState() で制御
    esp_wifi_set_max_tx_power(84);
    WiFi.begin(ssid.c_str(), pass.c_str());
    builtinLed.on();

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
        delay(200);  // 500ms → 200ms: 接続成功の検出を高速化
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        if (g_wifiBuzzerActive) { buzzerRawOff(); g_wifiBuzzerActive = false; }
        apModeActive = false;
        g_wifiLostMs = 0;
        dnsServer.stop();
        builtinLed.off();
        Serial.printf("[wifi] connected IP=%s\n", WiFi.localIP().toString().c_str());
        startNetServices();
        beepDouble();
    } else {
        Serial.println("[wifi] 59 s elapsed — starting captive portal");
        startCaptivePortal();
    }
}

static void checkWifiState()
{
    if (apModeActive) return;
    if (WiFi.status() == WL_CONNECTED) {
        if (g_wifiLostMs != 0) {
            // WiFi 復旧 → ブザー即停止
            if (g_wifiBuzzerActive) { buzzerRawOff(); g_wifiBuzzerActive = false; }
        }
        g_wifiLostMs = 0;
        return;
    }
    if (g_wifiLostMs == 0) {
        g_wifiLostMs = millis();
        WiFi.reconnect();
        Serial.println("[wifi] disconnected — reconnecting immediately");
        // WiFi 切断起因の TCP 切断 → beepLong3 を鳴らさず静かにクリア
        g_tcpSessionActive = false;
        g_tcpEverConnected = false;
        tcpClient.stop();
        // 5 秒間の警告ブザー開始（電圧アラーム中は重複を避ける）
        if (g_buzzerEnabled && !g_alarmActive) {
            buzzerRawOn();
            g_wifiBuzzerActive  = true;
            g_wifiBuzzerStartMs = millis();
        }
    }
    // 15 秒経っても復帰しなければ完全再接続シーケンスへ
    if (millis() - g_wifiLostMs >= 15000) { g_wifiLostMs = 0; wifiConnect(); }
}

// ── Web handlers ──────────────────────────────────────────────────────────────

static void handleRoot()       { webServer.send(200, "text/html", pageWifi()); }
static void handleWifiGet()    { webServer.send(200, "text/html", pageWifi()); }
static void handleVoltageGet() { webServer.send(200, "text/html", pageVoltage()); }

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
    String ssid = webServer.arg("ssid");
    String pass = webServer.arg("pass");

    if (ssid.length() == 0) {
        webServer.send(200, "text/html",
            pageWifi(L("SSID cannot be empty.", "SSIDを入力してください。")));
        return;
    }
    prefs.begin("elrs", false);
    prefs.putString("ssid", ssid);
    if (pass.length() > 0) prefs.putString("wifiPass", pass);
    prefs.putBool("configured", true);
    prefs.end();

    webServer.send(200, "text/html",
        pageWifi(L("Saved. Connecting\xe2\x80\xa6", "保存しました。接続中…")));
    beepShort();
    delay(420);
    wifiConnect();
}

static void handleVoltagePost()
{
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
    beepShort();
}

static void handleNotFound()
{
    webServer.sendHeader("Location", "/", true);
    webServer.send(302, "text/plain", "");
}

// ── Prefs / Voltage / LED ─────────────────────────────────────────────────────

static void loadPrefs()
{
    prefs.begin("elrs", true);
    g_vbatRatio     = prefs.getFloat("vbatRatio", VBAT_DEFAULT_RATIO);
    g_alarmVoltage  = prefs.getFloat("alarmV",    VBAT_DEFAULT_ALARM_V);
    g_vbatAlarmEnabled = prefs.getBool("vbatAlarmEn", false);
    g_buzzerEnabled    = prefs.getBool("buzzerEn",    true);
    g_ledEnabled       = prefs.getBool("ledEn",       true);
    g_langJa           = prefs.getBool("langJa",      true);
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
        Serial.printf("[vbat] %.2f V — alarm %s\n", g_currentVoltage, alarm ? "ON" : "OFF");
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
    if (!g_ledEnabled) { nlWrite(0); return; }

    uint32_t now = millis();

    // AP モード → 0 ↔ 255 の 500ms ゆっくり点滅
    if (apModeActive) {
        nlWrite((now % 1000 < 500) ? 255 : 0);
        return;
    }

    // WiFi 未接続 → 0 ↔ 255 の 80ms 高速点滅
    if (WiFi.status() != WL_CONNECTED) {
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
    Serial.println("[boot] XIAO ESP32-S3 WiFi bridge + Web UI");

    analogReadResolution(12);

    builtinLed.init(LED_BUILTIN_PIN, true);  // active-LOW
    pinMode(BUZZER_PIN_POS, OUTPUT);
    pinMode(BUZZER_PIN_NEG, OUTPUT);
    pinMode(LED_NOTIFY_PIN, OUTPUT);
    nlWrite(0);                              // GPIO9 初期消灯（analogWrite でPWM初期化）
    buzzerRawOff();

    // VBAT ピンのみ ADC 設定（全ピン一括は GPIO9 を入力化してしまうため不可）
    analogSetPinAttenuation(VBAT_ADC_PIN, ADC_11db);

    uart.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

    loadPrefs();
    wifiConnect();

    const char *hdrs[] = {"Referer"};
    webServer.collectHeaders(hdrs, 1);

    webServer.on("/",                    HTTP_GET,  handleRoot);
    webServer.on("/wifi",                HTTP_GET,  handleWifiGet);
    webServer.on("/wifi",                HTTP_POST, handleWifiPost);
    webServer.on("/voltage",             HTTP_GET,  handleVoltageGet);
    webServer.on("/voltage",             HTTP_POST, handleVoltagePost);
    webServer.on("/lang",                HTTP_GET,  handleLangToggle);
    webServer.on("/generate_204",        HTTP_GET,  handleRoot);
    webServer.on("/hotspot-detect.html", HTTP_GET,  handleRoot);
    webServer.on("/ncsi.txt",            HTTP_GET,  handleRoot);
    webServer.onNotFound(handleNotFound);
    webServer.begin();
    Serial.println("[web] HTTP server on port 80");
    Serial.println("[boot] ready");
}

void loop()
{
    checkWifiState();
    updateWifiBuzzer();
    if (apModeActive) dnsServer.processNextRequest();
    webServer.handleClient();
    updateLed();
    updateVoltage();
    updateNotifyLed();

    // TCP セッション切断検知
    if (g_tcpSessionActive && (!tcpClient || !tcpClient.connected())) {
        g_tcpSessionActive = false;
        Serial.println("[tcp] session lost");
        beepLong3();
    }
    // 新規クライアント受け付け
    if (!tcpClient || !tcpClient.connected()) {
        WiFiClient c = tcpServer.available();
        if (c) {
            tcpClient = c;
            g_tcpSessionActive = true;
            g_tcpEverConnected = true;
            Serial.println("[tcp] session started");
            beepDouble();
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
