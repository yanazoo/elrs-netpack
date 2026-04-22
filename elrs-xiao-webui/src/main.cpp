#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HardwareSerial.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include "config.h"
#include "msp.h"
#include "msptypes.h"

// ── TCP bridge globals (unchanged from elrs-xiao-bridge) ─────────────────────

static WiFiServer    tcpServer(TCP_PORT);
static WiFiClient    tcpClient;
static MSP           mspFromTcp;
static MSP           mspFromUart;
static HardwareSerial uart(1);

// ── Web / AP globals ──────────────────────────────────────────────────────────

static WebServer   webServer(80);
static Preferences prefs;
static DNSServer   dnsServer;

static bool     apModeActive     = false;
static uint32_t g_wifiLostMs     = 0;
static uint32_t g_ledBlinkMs     = 0;

static float    g_vbatRatio      = VBAT_DEFAULT_RATIO;
static float    g_alarmVoltage   = VBAT_DEFAULT_ALARM_V;
static float    g_currentVoltage = 0.0f;
static bool     g_alarmActive    = false;
static uint32_t g_lastVbatMs     = 0;
static bool     g_buzzerEnabled  = true;

// ── helpers ───────────────────────────────────────────────────────────────────

static void ledOn()     { digitalWrite(LED_NOTIFY_PIN, HIGH); }
static void ledOff()    { digitalWrite(LED_NOTIFY_PIN, LOW);  }
static void buzzerOn()  { digitalWrite(BUZZER_PIN_POS, HIGH); digitalWrite(BUZZER_PIN_NEG, LOW); ledOn(); }
static void buzzerOff() { digitalWrite(BUZZER_PIN_POS, LOW);  digitalWrite(BUZZER_PIN_NEG, LOW); ledOff(); }

static void beepShort()
{
    if (!g_buzzerEnabled || g_alarmActive) return;
    buzzerOn();  delay(80);  buzzerOff();
}

static void beepDouble()
{
    if (!g_buzzerEnabled || g_alarmActive) return;
    buzzerOn();  delay(80);  buzzerOff();
    delay(120);
    buzzerOn();  delay(80);  buzzerOff();
}

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
        resp.payload[0]  = 0;
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

static String htmlHead(const char *title)
{
    String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    h += "<title>"; h += title; h += "</title>";
    h += "<style>"
         "body{font-family:sans-serif;max-width:480px;margin:40px auto;padding:0 16px}"
         "h1{font-size:1.3em;color:#333}"
         "nav a{margin-right:12px;text-decoration:none;color:#0070f3}"
         "label{display:block;margin-top:12px;font-size:.9em;color:#555}"
         "input[type=text],input[type=password],input[type=number]"
         "{width:100%;padding:8px;box-sizing:border-box;border:1px solid #ccc;border-radius:4px;margin-top:4px}"
         "button{margin-top:16px;padding:10px 24px;background:#0070f3;color:#fff;"
         "border:none;border-radius:4px;cursor:pointer;font-size:1em}"
         ".msg{margin-top:12px;padding:8px;border-radius:4px;background:#e8f5e9;color:#2e7d32}"
         ".alarm{background:#ffebee;color:#c62828}"
         "</style></head><body>";
    h += "<h1>ELRS Netpack</h1>";
    h += "<nav><a href='/wifi'>WiFi</a><a href='/voltage'>Voltage</a></nav><hr>";
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
    String p = htmlHead("WiFi Settings");
    p += "<h2>WiFi Settings</h2>";
    if (msg.length()) {
        p += "<div class='msg'>"; p += msg; p += "</div>";
    }
    p += "<form method='POST' action='/wifi'>";
    p += "<label>SSID<input type='text' name='ssid' value='";
    p += nvsSsid();
    p += "'></label>";
    p += "<label>Password<input type='password' name='pass' placeholder='(leave blank to keep)'></label>";
    p += "<button type='submit'>Save &amp; Connect</button></form>";
    p += "</body></html>";
    return p;
}

static String pageVoltage(const String &msg = "")
{
    String p = htmlHead("Voltage Monitor");
    p += "<h2>Battery Voltage</h2>";
    p += "<p>Current: <strong>";
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f V", g_currentVoltage);
    p += buf;
    p += "</strong></p>";
    if (g_alarmActive) p += "<div class='msg alarm'>LOW VOLTAGE ALARM</div>";
    if (msg.length()) { p += "<div class='msg'>"; p += msg; p += "</div>"; }
    p += "<form method='POST' action='/voltage'>";
    p += "<label>Divider ratio (e.g. 2.0 for two equal resistors)"
         "<input type='number' name='ratio' step='0.01' min='1' value='";
    snprintf(buf, sizeof(buf), "%.2f", g_vbatRatio);
    p += buf;
    p += "'></label>";
    p += "<label>Alarm threshold (V)"
         "<input type='number' name='alarm' step='0.01' min='2' max='5' value='";
    snprintf(buf, sizeof(buf), "%.2f", g_alarmVoltage);
    p += buf;
    p += "'></label>";
    p += "<label style='flex-direction:row;align-items:center;gap:8px'>"
         "<input type='checkbox' name='buzzerEn' value='1'";
    if (g_buzzerEnabled) p += " checked";
    p += "> Notification sounds (WiFi / lap / save)</label>";
    p += "<button type='submit'>Save</button></form>";
    p += "</body></html>";
    return p;
}

// ── WiFi / AP ─────────────────────────────────────────────────────────────────

static void startCaptivePortal()
{
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid, ap_password);
    apModeActive = true;
    dnsServer.start(53, "*", WiFi.softAPIP());
    Serial.printf("[ap] IP=%s — captive portal active\n",
                  WiFi.softAPIP().toString().c_str());
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
    prefs.begin("elrs", true);
    String ssid = prefs.getString("ssid",     WIFI_SSID);
    String pass = prefs.getString("wifiPass", WIFI_PASSWORD);
    prefs.end();

    Serial.printf("[wifi] connecting to %s\n", ssid.c_str());
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    digitalWrite(LED_BUILTIN_PIN, LOW);   // active-LOW: on during connect attempt

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 59000) {
        delay(500);
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        apModeActive = false;
        g_wifiLostMs = 0;
        dnsServer.stop();
        digitalWrite(LED_BUILTIN_PIN, HIGH);  // off on success
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
    if (WiFi.status() == WL_CONNECTED) {
        if (apModeActive) {
            apModeActive = false;
            g_wifiLostMs = 0;
            dnsServer.stop();
            WiFi.mode(WIFI_STA);
            digitalWrite(LED_BUILTIN_PIN, HIGH);
            startNetServices();
        } else {
            g_wifiLostMs = 0;
        }
        return;
    }

    if (g_wifiLostMs == 0) g_wifiLostMs = millis();

    if (!apModeActive && millis() - g_wifiLostMs >= 60000) {
        Serial.println("[wifi] 60 s without connection — reconnecting");
        wifiConnect();
    }
}

// ── Web handlers ──────────────────────────────────────────────────────────────

static void handleRoot()
{
    webServer.send(200, "text/html", pageWifi());
}

static void handleWifiGet()
{
    webServer.send(200, "text/html", pageWifi());
}

static void handleWifiPost()
{
    String ssid = webServer.arg("ssid");
    String pass = webServer.arg("pass");

    if (ssid.length() == 0) {
        webServer.send(200, "text/html", pageWifi("SSID cannot be empty."));
        return;
    }

    prefs.begin("elrs", false);
    prefs.putString("ssid", ssid);
    if (pass.length() > 0) prefs.putString("wifiPass", pass);
    prefs.end();

    beepShort();
    webServer.send(200, "text/html", pageWifi("Saved. Connecting…"));
    delay(500);
    wifiConnect();
}

static void handleVoltageGet()
{
    webServer.send(200, "text/html", pageVoltage());
}

static void handleVoltagePost()
{
    String ratioStr = webServer.arg("ratio");
    String alarmStr = webServer.arg("alarm");

    float newRatio = ratioStr.length() ? ratioStr.toFloat() : g_vbatRatio;
    float newAlarm = alarmStr.length() ? alarmStr.toFloat() : g_alarmVoltage;

    if (newRatio < 1.0f) newRatio = 1.0f;
    if (newAlarm < 2.0f) newAlarm = 2.0f;

    g_vbatRatio      = newRatio;
    g_alarmVoltage   = newAlarm;
    g_buzzerEnabled  = webServer.hasArg("buzzerEn");

    prefs.begin("elrs", false);
    prefs.putFloat("vbatRatio", g_vbatRatio);
    prefs.putFloat("alarmV",    g_alarmVoltage);
    prefs.putBool("buzzerEn",   g_buzzerEnabled);
    prefs.end();

    webServer.send(200, "text/html", pageVoltage("Settings saved."));
    beepShort();
}

static void handleNotFound()
{
    webServer.sendHeader("Location", "/", true);
    webServer.send(302, "text/plain", "");
}

// ── Voltage / buzzer ──────────────────────────────────────────────────────────

static void loadPrefs()
{
    prefs.begin("elrs", true);
    g_vbatRatio    = prefs.getFloat("vbatRatio", VBAT_DEFAULT_RATIO);
    g_alarmVoltage = prefs.getFloat("alarmV",    VBAT_DEFAULT_ALARM_V);
    g_buzzerEnabled = prefs.getBool("buzzerEn",  true);
    prefs.end();
}

static void updateVoltage()
{
    if (millis() - g_lastVbatMs < 1000) return;
    g_lastVbatMs = millis();

    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) sum += analogRead(VBAT_ADC_PIN);
    float pinV = (sum / 8.0f / VBAT_ADC_RESOLUTION) * VBAT_VREF;
    g_currentVoltage = pinV * g_vbatRatio;

    bool alarm = (g_currentVoltage < g_alarmVoltage);
    if (alarm != g_alarmActive) {
        g_alarmActive = alarm;
        if (alarm) buzzerOn(); else buzzerOff();
        Serial.printf("[vbat] %.2f V — alarm %s\n",
                      g_currentVoltage, alarm ? "ON" : "OFF");
    }
}

// ── LED ───────────────────────────────────────────────────────────────────────

static void updateLed()
{
    if (!apModeActive) {
        digitalWrite(LED_BUILTIN_PIN, HIGH);  // off (active-LOW)
        return;
    }
    if (millis() - g_ledBlinkMs >= LED_AP_INTERVAL) {
        g_ledBlinkMs = millis();
        digitalWrite(LED_BUILTIN_PIN, !digitalRead(LED_BUILTIN_PIN));
    }
}

// ── Arduino entry points ──────────────────────────────────────────────────────

void setup()
{
    Serial.begin(115200);
    Serial.println("[boot] XIAO ESP32-S3 WiFi bridge + Web UI");

    analogReadResolution(12);

    pinMode(LED_BUILTIN_PIN, OUTPUT);
    digitalWrite(LED_BUILTIN_PIN, HIGH);  // off

    pinMode(BUZZER_PIN_POS, OUTPUT);
    pinMode(BUZZER_PIN_NEG, OUTPUT);
    pinMode(LED_NOTIFY_PIN, OUTPUT);
    buzzerOff();

    uart.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

    loadPrefs();
    wifiConnect();

    // Web server routes (always active, works in both STA and AP mode)
    webServer.on("/",                    HTTP_GET,  handleRoot);
    webServer.on("/wifi",                HTTP_GET,  handleWifiGet);
    webServer.on("/wifi",                HTTP_POST, handleWifiPost);
    webServer.on("/voltage",             HTTP_GET,  handleVoltageGet);
    webServer.on("/voltage",             HTTP_POST, handleVoltagePost);
    // Captive portal probes
    webServer.on("/generate_204",        HTTP_GET,  handleRoot);  // Android
    webServer.on("/hotspot-detect.html", HTTP_GET,  handleRoot);  // iOS
    webServer.on("/ncsi.txt",            HTTP_GET,  handleRoot);  // Windows
    webServer.onNotFound(handleNotFound);
    webServer.begin();
    Serial.println("[web] HTTP server on port 80");

    Serial.println("[boot] ready");
}

void loop()
{
    checkWifiState();
    if (apModeActive) dnsServer.processNextRequest();
    webServer.handleClient();
    updateLed();
    updateVoltage();

    // Accept new TCP client if none connected
    if (!tcpClient || !tcpClient.connected()) {
        WiFiClient c = tcpServer.available();
        if (c) {
            tcpClient = c;
            Serial.println("[tcp] client connected");
        }
    }

    // TCP -> UART: RotorHazard -> ESP32 Wrover-E
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

    // UART -> TCP: ESP32 Wrover-E -> RotorHazard
    while (uart.available()) {
        uint8_t b = uart.read();
        if (mspFromUart.processReceivedByte(b)) {
            mspPacket_t *pkt = mspFromUart.getReceivedPacket();
            handlePacketFromUart(pkt);
            mspFromUart.markPacketReceived();
        }
    }
}
