// Firmware update of the netpack XIAO (see fw_update.h).
#include "fw_update.h"

#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <NetworkClientSecure.h>
#include <Update.h>
#include <WiFi.h>
#include <time.h>
#include "log.h"

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif
#ifndef FW_BUILD
#define FW_BUILD "?"
#endif

const char *fwVersion() { return FW_VERSION; }
const char *fwBuild() { return FW_BUILD; }

namespace {

const char *const kReleases = "https://github.com/yanazoo/elrs-netpack/releases";
const char *const kVerAsset = "elrs-netpack-version.txt";
const char *const kFwAsset  = "elrs-netpack-xiao_esp32s3-firmware.bin";

enum State : uint8_t { kIdle, kChecking, kChecked, kUpdating, kDone, kFailed };
const char *const kStateNames[] = {"idle", "checking", "checked", "updating", "done", "failed"};

WebServer *g_web = nullptr;
bool (*g_online)() = nullptr;
String (*g_head)(const char *) = nullptr;
bool (*g_ja)() = nullptr;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
State   state = kIdle;
uint8_t pct = 0;
char    latest[24] = "";
char    error[160] = "";
volatile bool taskRunning = false;
bool    uploadOk = false;
bool    uploadBegan = false;

bool ja() { return g_ja ? g_ja() : true; }
const char *L(const char *en, const char *jp) { return ja() ? jp : en; }

void setState(State s, const char *err = nullptr)
{
    portENTER_CRITICAL(&mux);
    state = s;
    if (err) {
        strlcpy(error, err, sizeof error);
        for (char *c = error; *c; c++)
            if (*c == '"' || *c == '\\' || (uint8_t)*c < 0x20) *c = ' ';
    } else if (s != kFailed) {
        error[0] = 0;
    }
    portEXIT_CRITICAL(&mux);
}

extern "C" const uint8_t crtBundleStart[] asm("_binary_x509_crt_bundle_start");
extern "C" const uint8_t crtBundleEnd[] asm("_binary_x509_crt_bundle_end");

void useRootStore(NetworkClientSecure &tls) { tls.setCACertBundle(crtBundleStart, crtBundleEnd - crtBundleStart); }

bool netReady(char *why, size_t n)
{
    IPAddress ip;
    if (!WiFi.hostByName("github.com", ip)) {
        snprintf(why, n, "%s", L("could not look up github.com (DNS)", "github.com の名前を引けませんでした（DNS）"));
        return false;
    }
    if (time(nullptr) < 1700000000) {
        configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
        for (int i = 0; i < 60 && time(nullptr) < 1700000000; i++) delay(250);
    }
    if (time(nullptr) < 1700000000) {
        snprintf(why, n, "%s", L("could not set the clock (NTP)", "時計を合わせられませんでした（NTP）"));
        return false;
    }
    return true;
}

void failText(char *out, size_t n, const char *what, int code, NetworkClientSecure &tls)
{
    char t[96] = "";
    tls.lastError(t, sizeof t);
    if (t[0]) snprintf(out, n, "%s (%d: %s)", what, code, t);
    else      snprintf(out, n, "%s (%d)", what, code);
    LOG.printf("[update] %s, free heap %u\n", out, (unsigned)ESP.getFreeHeap());
}

// GET `url`, following redirects one hop at a time on a new TLS client each
// (a closed client drops its root store). On 200, `url` is the final address
// and `body` (if given) the response.
int getFinal(String &url, String *body, char *why, size_t n)
{
    int code = -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt) delay(1500);
        String u = url;
        for (int hop = 0; hop < 6; hop++) {
            NetworkClientSecure tls;
            useRootStore(tls);
            HTTPClient http;
            http.setReuse(false);
            http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
            http.setTimeout(15000);
            code = http.begin(tls, u) ? http.GET() : -1;
            String next = http.getLocation();
            if (code >= 300 && code < 400 && next.startsWith("https://")) {
                http.end();
                u = next;
                continue;
            }
            if (code == 200) {
                if (body) *body = http.getString();
                http.end();
                url = u;
                return 200;
            }
            failText(why, n, L("could not reach GitHub", "GitHub に接続できませんでした"), code, tls);
            http.end();
            break;
        }
    }
    return code;
}

void endTask()
{
    taskRunning = false;
    vTaskDelete(nullptr);
}

void checkTask(void *)
{
    char e[160];
    String url = String(kReleases) + "/latest/download/" + kVerAsset, v;
    if (!netReady(e, sizeof e)) setState(kFailed, e);
    else if (getFinal(url, &v, e, sizeof e) != 200) setState(kFailed, e);
    else {
        v.trim();
        if (v.length() > 1 && v.length() < sizeof latest && v[0] == 'v') {
            portENTER_CRITICAL(&mux);
            strlcpy(latest, v.c_str(), sizeof latest);
            portEXIT_CRITICAL(&mux);
            setState(kChecked);
            LOG.printf("[update] latest release %s (this firmware %s)\n", latest, FW_VERSION);
        } else {
            setState(kFailed, L("could not read the version", "バージョンを読めませんでした"));
        }
    }
    endTask();
}

void updateTask(void *)
{
    char e[160];
    if (!netReady(e, sizeof e)) { setState(kFailed, e); endTask(); return; }
    char tag[24];
    portENTER_CRITICAL(&mux);
    strlcpy(tag, latest, sizeof tag);
    portEXIT_CRITICAL(&mux);
    httpUpdate.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    httpUpdate.rebootOnUpdate(false);
    httpUpdate.onProgress([](int cur, int total) {
        portENTER_CRITICAL(&mux);
        pct = total > 0 ? (uint8_t)((int64_t)cur * 100 / total) : 0;
        portEXIT_CRITICAL(&mux);
    });
    pct = 0;
    LOG.printf("[update] firmware %s\n", tag);
    String url = String(kReleases) + "/download/" + tag + "/" + kFwAsset;
    t_httpUpdate_return r = HTTP_UPDATE_FAILED;
    if (getFinal(url, nullptr, e, sizeof e) == 200) {
        NetworkClientSecure tls;
        useRootStore(tls);
        r = httpUpdate.update(tls, url);
        if (r != HTTP_UPDATE_OK)
            failText(e, sizeof e, (String(L("writing failed: ", "書き込みに失敗しました：")) + httpUpdate.getLastErrorString()).c_str(),
                     httpUpdate.getLastError(), tls);
    }
    if (r != HTTP_UPDATE_OK) { setState(kFailed, e); endTask(); return; }
    setState(kDone);
    LOG.println("[update] done, restarting");
    delay(2000);
    ESP.restart();
}

bool startTask(TaskFunction_t fn, const char *name)
{
    if (taskRunning) return false;
    taskRunning = true;
    if (xTaskCreatePinnedToCore(fn, name, 12288, nullptr, 1, nullptr, 0) != pdPASS) {
        taskRunning = false;
        return false;
    }
    return true;
}

String statusJson()
{
    char buf[400];
    portENTER_CRITICAL(&mux);
    State s = state;
    char lt[24], er[160];
    strlcpy(lt, latest, sizeof lt);
    strlcpy(er, error, sizeof er);
    uint8_t pc = pct;
    portEXIT_CRITICAL(&mux);
    snprintf(buf, sizeof buf, R"({"version":"%s","build":"%s","latest":"%s","state":"%s","pct":%u,"error":"%s","online":%s})",
             FW_VERSION, FW_BUILD, lt, kStateNames[s], pc, er, (g_online && g_online()) ? "true" : "false");
    return String(buf);
}

void sendJson(int code, const char *body) { g_web->send(code, "application/json", body); }

// ── The page ─────────────────────────────────────────────────────────────────

String page(const String &msg = "")
{
    String p = g_head(L("Update", "ファーム更新"));
    p += "<h2>"; p += L("Firmware", "ファーム"); p += "</h2>";
    p += "<div class='card st'>"; p += L("This firmware: ", "今のファーム："); p += FW_VERSION;
    p += "（"; p += FW_BUILD; p += "）</div>";
    if (msg.length()) { p += "<div class='msg'>"; p += msg; p += "</div>"; }

    p += "<h2>"; p += L("From the internet", "ネットから更新"); p += "</h2>";
    p += "<div class='note'>";
    p += L("Needs the internet: a router (client mode) or USB tethering (host mode).",
           "インターネットが必要です（クライアントモードのルーター、またはホストモードの USB テザリング）。");
    p += "</div>";
    p += "<button type='button' id='chk' onclick='chk()'>🔍 "; p += L("Check for the latest", "最新版を確認"); p += "</button> ";
    p += "<button type='button' id='upd' onclick='upd()' style='display:none'></button>";
    p += "<div id='ust' class='note' style='font-size:.9em;color:#bbb'></div>";

    p += "<h2>"; p += L("From a file", "ファイルから"); p += "</h2>";
    p += "<form id='ff' method='POST' action='/update' enctype='multipart/form-data'>";
    p += "<label>"; p += L("firmware.bin of elrs-xiao-webui (in the release zip, or elrs-netpack-xiao_esp32s3-firmware.bin)",
                           "elrs-xiao-webui の firmware.bin（リリースの zip の中、または elrs-netpack-xiao_esp32s3-firmware.bin）");
    p += "<input type='file' name='fw' accept='.bin'></label>";
    p += "<button type='submit' id='fb'>"; p += L("Write the file", "ファイルで更新"); p += "</button>";
    p += "</form>";
    // Progress while the file is written (the netpack writes it as it arrives).
    p += "<div id='fp' style='display:none;margin-top:12px'>"
         "<div style='height:14px;background:#2a2a2a;border-radius:7px;overflow:hidden'>"
         "<div id='fpb' style='height:100%;width:0;background:#5b9bd5;transition:width .3s'></div></div>"
         "<div id='fst' style='margin-top:6px;font-size:.95em;color:#ddd'></div></div>";
    p += "<div class='note'>";
    p += L("It restarts when done (about 30 s). Settings are kept.", "書き込みが終わると再起動します（約 30 秒）。設定はそのまま残ります。");
    p += "</div>";

    // Polls /api/update while a check or an update runs.
    p += "<script>";
    p += "const J=";
    p += ja() ? "1" : "0";
    p += ",T=(e,j)=>J?j:e,st=t=>document.getElementById('ust').textContent=t,ub=document.getElementById('upd'),cur='";
    p += FW_VERSION;
    p += "';"
         "async function poll(){for(;;){await new Promise(r=>setTimeout(r,1000));let s;"
         "try{s=await (await fetch('/api/update')).json();}catch(e){st(T('Restarting…','再起動しています…'));setTimeout(()=>location.href='/',15000);return;}"
         "if(s.state==='checking'){st(T('Checking…','確認しています…'));continue;}"
         "if(s.state==='checked'){if(s.latest===cur){st(T('Up to date: ','最新です：')+s.latest);ub.style.display='none';}"
         "else{st(T('New release: ','新しい版があります：')+s.latest);ub.textContent='⬇️ '+s.latest+T(': update',' に更新');ub.style.display='';}return;}"
         "if(s.state==='updating'){st(T('Writing… ','書き込み中… ')+s.pct+'%');continue;}"
         "if(s.state==='done'){st(T('Written. Restarting…','書き込みました。再起動します…'));continue;}"
         "if(s.state==='failed'){st(T('Failed: ','失敗：')+s.error);return;}return;}}"
         "const ff=document.getElementById('ff'),fpb=document.getElementById('fpb'),fst=t=>document.getElementById('fst').textContent=t;"
         "async function back(){for(let i=0;i<40;i++){await new Promise(r=>setTimeout(r,2000));"
         "try{const r=await fetch('/api/update',{cache:'no-store'});if(r.ok){fst(T('Done. Reloading…','完了しました。読み直します…'));setTimeout(()=>location.href='/update',800);return;}}catch(e){}}"
         "fst(T('It does not answer: reload the page.','応答がありません。ページを読み直してください。'));}"
         "ff.onsubmit=e=>{e.preventDefault();const f=ff.querySelector('input[type=file]').files[0];"
         "if(!f){alert(T('Choose a file.','ファイルを選んでください。'));return;}"
         "if(!confirm(T('Write '+f.name+'? The netpack restarts.',f.name+' を書き込みます。netpack は再起動します。よろしいですか？')))return;"
         "document.getElementById('fb').disabled=true;document.getElementById('fp').style.display='';fst(T('Writing… 0%','書き込み中… 0%'));"
         "const x=new XMLHttpRequest();x.open('POST','/update?ajax=1');"
         "x.upload.onprogress=v=>{if(v.lengthComputable){const q=Math.floor(v.loaded*100/v.total);fpb.style.width=q+'%';"
         "fst((q<100?T('Writing… ','書き込み中… '):T('Finishing… ','仕上げ中… '))+q+'%');}};"
         "x.onload=()=>{let j={};try{j=JSON.parse(x.responseText);}catch(e){}"
         "if(j.ok){fpb.style.width='100%';fst(T('Written. Restarting… (about 30 s)','書き込みました。再起動しています…（約 30 秒）'));back();}"
         "else{fst(T('Failed: ','失敗：')+(j.error||x.status));document.getElementById('fb').disabled=false;}};"
         "x.onerror=()=>{fst(T('Connection lost (it may still be restarting).','接続が切れました（再起動中の可能性があります）'));back();};"
         "const fd=new FormData();fd.append('fw',f);x.send(fd);};"
         "async function chk(){st(T('Checking…','確認しています…'));const r=await fetch('/api/update/check',{method:'POST'});"
         "if(!r.ok){const j=await r.json().catch(()=>({}));st(j.reason==='offline'?T('Not on the internet','インターネットにつながっていません'):T('Busy','実行中です'));return;}poll();}"
         "async function upd(){if(!confirm(T('Write ','')+ub.textContent.replace('⬇️ ','')+T('? The netpack restarts.','。netpack は再起動します。よろしいですか？')))return;"
         "ub.style.display='none';const r=await fetch('/api/update/online',{method:'POST'});if(!r.ok){st(T('Could not start','開始できませんでした'));return;}poll();}"
         "</script></body></html>";
    return p;
}

void handlePage() { g_web->send(200, "text/html", page()); }

void handleUpload()
{
    HTTPUpload &u = g_web->upload();
    if (u.status == UPLOAD_FILE_START) {
        LOG.printf("[update] file %s\n", u.filename.c_str());
        uploadBegan = true;
        uploadOk = !taskRunning && Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
    } else if (u.status == UPLOAD_FILE_WRITE) {
        if (uploadOk && Update.write(u.buf, u.currentSize) != u.currentSize) uploadOk = false;
    } else if (u.status == UPLOAD_FILE_END) {
        uploadOk = uploadOk && Update.end(true);
        LOG.printf("[update] %s (%u bytes)\n", uploadOk ? "ok" : Update.errorString(), (unsigned)u.totalSize);
    } else if (u.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        uploadOk = false;
    }
}

void handleUploadDone()
{
    bool ok = uploadBegan && uploadOk;
    uploadBegan = false;
    if (g_web->hasArg("ajax")) {   // the page's progress bar
        String e = ok ? "" : String(Update.hasError() ? Update.errorString() : L("no file", "ファイルがありません"));
        e.replace("\"", "'");
        g_web->send(200, "application/json", ok ? String("{\"ok\":true}") : "{\"ok\":false,\"error\":\"" + e + "\"}");
        if (ok) { delay(800); ESP.restart(); }
        return;
    }
    if (!ok) {
        g_web->send(200, "text/html", page(String(L("Update failed: ", "更新できませんでした：")) +
                                           (Update.hasError() ? Update.errorString() : L("no file", "ファイルがありません"))));
        return;
    }
    g_web->send(200, "text/html", page(L("Written. Restarting…", "書き込みました。再起動します…")));
    delay(800);
    ESP.restart();
}

}   // namespace

void fwUpdateRoutes(WebServer &web, bool (*online)(), String (*head)(const char *), bool (*jaFn)())
{
    g_web = &web;
    g_online = online;
    g_head = head;
    g_ja = jaFn;
    web.on("/update", HTTP_GET, handlePage);
    web.on("/update", HTTP_POST, handleUploadDone, handleUpload);
    web.on("/api/update", HTTP_GET, [] { g_web->send(200, "application/json", statusJson()); });
    web.on("/api/update/check", HTTP_POST, [] {
        if (!g_online()) return sendJson(409, R"({"ok":false,"reason":"offline"})");
        if (taskRunning) return sendJson(409, R"({"ok":false,"reason":"busy"})");
        setState(kChecking);
        if (!startTask(checkTask, "fwcheck")) { setState(kFailed, "start"); return sendJson(409, R"({"ok":false,"reason":"busy"})"); }
        sendJson(200, R"({"ok":true})");
    });
    web.on("/api/update/online", HTTP_POST, [] {
        if (!g_online()) return sendJson(409, R"({"ok":false,"reason":"offline"})");
        if (taskRunning || Update.isRunning()) return sendJson(409, R"({"ok":false,"reason":"busy"})");
        portENTER_CRITICAL(&mux);
        bool have = latest[0];
        portEXIT_CRITICAL(&mux);
        if (!have) return sendJson(409, R"({"ok":false,"reason":"nocheck"})");
        setState(kUpdating);
        if (!startTask(updateTask, "fwupdate")) { setState(kFailed, "start"); return sendJson(409, R"({"ok":false,"reason":"busy"})"); }
        sendJson(200, R"({"ok":true})");
    });
}
