#include "web.h"

Web webServer;

String Web::arg(const String &name) const
{
    if (!req) return String();
    const AsyncWebParameter *p = req->getParam(name, true);   // a form's field
    if (!p) p = req->getParam(name);                          // or the query
    return p ? p->value() : String();
}

bool Web::hasArg(const String &name) const { return req && (req->hasParam(name, true) || req->hasParam(name)); }

String Web::header(const char *name) const
{
    const AsyncWebHeader *h = req ? req->getHeader(name) : nullptr;
    return h ? h->value() : String();
}

String Web::uri() const { return req ? req->url() : String(); }

IPAddress Web::remoteIP() const { return req && req->client() ? req->client()->remoteIP() : IPAddress(); }

void Web::sendHeader(const String &key, const String &value, bool) { headers_.push_back({key, value}); }

void Web::send(int code, const char *type, const String &body)
{
    if (!req) return;
    AsyncWebServerResponse *r = req->beginResponse(code, type, body);
    for (auto &h : headers_) r->addHeader(h.first, h.second);
    headers_.clear();
    req->send(r);
}

void Web::on(const char *path, WebRequestMethod method, void (*fn)())
{
    server_.on(AsyncURIMatcher::exact(path), method, [this, fn](AsyncWebServerRequest *r) {
        req = r;
        headers_.clear();
        fn();
        req = nullptr;
    });
}

void Web::onNotFound(void (*fn)())
{
    server_.onNotFound([this, fn](AsyncWebServerRequest *r) {
        req = r;
        headers_.clear();
        fn();
        req = nullptr;
    });
}

void Web::onUpload(const char *path, void (*done)(), ArUploadHandlerFunction upload)
{
    server_.on(AsyncURIMatcher::exact(path), HTTP_POST,
               [this, done](AsyncWebServerRequest *r) {
                   req = r;
                   headers_.clear();
                   done();
                   req = nullptr;
               },
               upload);
}

void Web::begin() { server_.begin(); }

void Web::restartSoon(uint32_t ms) { restartAt_ = (millis() + ms) | 1; }

void Web::loop()
{
    if (restartAt_ && (int32_t)(millis() - restartAt_) >= 0) {
        delay(100);
        ESP.restart();
    }
}
