#pragma once
// The web server: ESPAsyncWebServer (as FPV C5 Lap Timer), several
// connections at once. Arduino's WebServer took one at a time and waited
// up to 5 s for a request on each: a browser's spare connection (opened
// ahead, used later or never) held every page up for seconds.
//
// The pages keep a WebServer-like interface: the handlers run one at a
// time on the AsyncTCP task, and the request being answered is `req`.
// Anything slow or radio-changing is left to loop() (restartSoon, the
// flags in main.cpp), never done in a handler.
#include <ESPAsyncWebServer.h>
#include <vector>

class Web {
public:
    AsyncWebServerRequest *req = nullptr;

    String arg(const String &name) const;
    bool hasArg(const String &name) const;
    String header(const char *name) const;
    String uri() const;
    IPAddress remoteIP() const;
    void sendHeader(const String &key, const String &value, bool = false);
    void send(int code, const char *type, const String &body);

    // A page: fn answers webServer.req.
    void on(const char *path, WebRequestMethod method, void (*fn)());
    void onNotFound(void (*fn)());
    void onUpload(const char *path, void (*done)(), ArUploadHandlerFunction upload);
    void begin();

    // Restart after the answer has gone out (from loop()).
    void restartSoon(uint32_t ms);
    void loop();

private:
    AsyncWebServer server_{80};
    std::vector<std::pair<String, String>> headers_;
    uint32_t restartAt_ = 0;
};

extern Web webServer;
