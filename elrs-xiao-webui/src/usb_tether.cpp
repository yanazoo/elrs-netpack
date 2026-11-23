// USB tethering (see usb_tether.h).
//
// The S3's USB-OTG port is the host. A phone with "USB tethering" on shows a
// network function: RNDIS (Android's long-standing one), CDC-NCM (newer
// phones) or CDC-ECM. Its frames go in and out of an esp_netif of our own
// (Ethernet frames, DHCP client), and the AP's NAPT (main.cpp) routes the
// netpack's network out through it.
//
// Tasks: usbLib pumps the USB host library; usbCli pumps this client's
// events and every transfer's callback; usbWork opens a device, sets its
// network function up and watches the address. Control transfers wait on a
// semaphore that a callback (run by usbCli) gives, so they are only ever
// made from usbWork.
#include "usb_tether.h"
#include "log.h"

#include <string.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_mac.h"

namespace {

enum class Proto : uint8_t { None, Rndis, Ncm, Ecm };

constexpr size_t RX_BUF   = 16384;   // one bulk IN transfer (NCM: one NTB)
constexpr size_t TX_BUF   = 1600;    // one frame and its header
constexpr int    TX_COUNT = 6;
constexpr size_t CTRL_BUF = 8 + 1024;

// ── State ───────────────────────────────────────────────────────────────────

usb_host_client_handle_t g_client = nullptr;
QueueHandle_t      g_evq = nullptr;   // usbCli → usbWork: new device / gone
SemaphoreHandle_t  g_ctrlDone = nullptr;
SemaphoreHandle_t  g_lock = nullptr;  // TetherInfo
volatile bool      g_running = false;

struct Ev { uint8_t kind; uint8_t addr; usb_device_handle_t dev; };   // kind 1 new, 2 gone

usb_device_handle_t g_dev = nullptr;
Proto    g_proto = Proto::None;
int      g_ifComm = -1, g_ifData = -1, g_altData = 0;
uint8_t  g_epIn = 0, g_epOut = 0;
uint16_t g_mpsIn = 64, g_mpsOut = 64;
bool     g_claimed = false;
volatile bool g_up = false;          // frames may flow
std::atomic<int> g_inFlight{0};      // transfers submitted and not yet back

usb_transfer_t *g_ctrl = nullptr;
usb_transfer_t *g_in = nullptr;
usb_transfer_t *g_out[TX_COUNT] = {};
QueueHandle_t   g_outFree = nullptr;

// NCM: how the phone wants our blocks laid out.
uint16_t g_ncmOutDiv = 4, g_ncmOutRem = 0, g_ncmOutAlign = 4;
uint32_t g_ncmOutMax = 2048;
uint16_t g_ncmSeq = 0;
uint32_t g_rndisReq = 1;

esp_netif_t *g_netif = nullptr;
bool         g_netifStarted = false;

TetherInfo g_info = {};

void setState(TetherState s)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_info.state != s) LOG.printf("[usb] %s\n", tetherStateText(s, false));
    g_info.state = s;
    xSemaphoreGive(g_lock);
}

inline uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
inline uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
inline void wr16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
inline void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// ── The network interface ───────────────────────────────────────────────────

struct Driver { esp_netif_driver_base_t base; };
Driver g_drv;

esp_err_t drvTransmit(void *h, void *buf, size_t len);
void drvFreeRx(void *h, void *buf) { free(buf); }

esp_err_t drvPostAttach(esp_netif_t *netif, void *args)
{
    Driver *d = (Driver *)args;
    d->base.netif = netif;
    esp_netif_driver_ifconfig_t ifc = {};
    ifc.handle = d;
    ifc.transmit = drvTransmit;
    ifc.driver_free_rx_buffer = drvFreeRx;
    return esp_netif_set_driver_config(netif, &ifc);
}

void netifCreate()
{
    static esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.if_key = "USB_TETH";
    base.if_desc = "usb";
    // Below the WiFi station (100): in client mode the router stays the way out.
    base.route_prio = 50;
    esp_netif_config_t cfg = {};
    cfg.base = &base;
    cfg.stack = ESP_NETIF_NETSTACK_DEFAULT_ETH;
    g_netif = esp_netif_new(&cfg);
    g_drv.base.post_attach = drvPostAttach;
    esp_netif_attach(g_netif, &g_drv);
}

void netifUp(const uint8_t mac[6])
{
    esp_netif_set_mac(g_netif, (uint8_t *)mac);
    esp_netif_set_hostname(g_netif, "elrs-netpack");
    esp_netif_action_start(g_netif, nullptr, 0, nullptr);
    esp_netif_action_connected(g_netif, nullptr, 0, nullptr);   // DHCP client
    g_netifStarted = true;
}

void netifDown()
{
    if (!g_netifStarted) return;
    esp_netif_action_disconnected(g_netif, nullptr, 0, nullptr);
    esp_netif_action_stop(g_netif, nullptr, 0, nullptr);
    g_netifStarted = false;
}

// A frame from the phone, into the network stack (it frees the copy).
void toStack(const uint8_t *frame, size_t len)
{
    if (len < 14 || len > 1600 || !g_netifStarted) return;
    void *b = malloc(len);
    if (!b) return;
    memcpy(b, frame, len);
    g_info.rxFrames++;
    esp_netif_receive(g_netif, b, len, nullptr);
}

// ── Transfers ───────────────────────────────────────────────────────────────

void ctrlCb(usb_transfer_t *t) { xSemaphoreGive(g_ctrlDone); }

// A control transfer to the device (from usbWork only). IN: up to len
// bytes into data; *got says how many.
bool ctrl(uint8_t reqType, uint8_t req, uint16_t value, uint16_t index, uint8_t *data, uint16_t len,
          int *got = nullptr)
{
    if (len > CTRL_BUF - 8) return false;
    usb_setup_packet_t *s = (usb_setup_packet_t *)g_ctrl->data_buffer;
    s->bmRequestType = reqType;
    s->bRequest = req;
    s->wValue = value;
    s->wIndex = index;
    s->wLength = len;
    bool in = reqType & 0x80;
    if (!in && len) memcpy(g_ctrl->data_buffer + 8, data, len);
    g_ctrl->num_bytes = 8 + len;
    g_ctrl->device_handle = g_dev;
    g_ctrl->bEndpointAddress = 0;
    g_ctrl->callback = ctrlCb;
    g_ctrl->context = nullptr;
    g_ctrl->timeout_ms = 1000;
    xSemaphoreTake(g_ctrlDone, 0);
    if (usb_host_transfer_submit_control(g_client, g_ctrl) != ESP_OK) return false;
    if (xSemaphoreTake(g_ctrlDone, pdMS_TO_TICKS(2500)) != pdTRUE) return false;
    if (g_ctrl->status != USB_TRANSFER_STATUS_COMPLETED) return false;
    int n = g_ctrl->actual_num_bytes - 8;
    if (n < 0) n = 0;
    if (in && data) memcpy(data, g_ctrl->data_buffer + 8, n);
    if (got) *got = n;
    return true;
}

void parseRndis(const uint8_t *p, size_t n)
{
    size_t off = 0;
    while (off + 44 <= n) {
        uint32_t type = rd32(p + off), msgLen = rd32(p + off + 4);
        if (type != 1 || msgLen < 44 || off + msgLen > n) break;   // REMOTE_NDIS_PACKET_MSG
        uint32_t dOff = rd32(p + off + 8), dLen = rd32(p + off + 12);
        if (8 + dOff + dLen <= msgLen) toStack(p + off + 8 + dOff, dLen);
        off += msgLen;
    }
}

void parseNcm(const uint8_t *p, size_t n)
{
    if (n < 12 || memcmp(p, "NCMH", 4) != 0) return;
    uint16_t ndp = rd16(p + 10);
    for (int guard = 0; ndp && guard < 8; guard++) {
        if ((size_t)ndp + 16 > n || memcmp(p + ndp, "NCM", 3) != 0) return;
        uint16_t ndpLen = rd16(p + ndp + 4), next = rd16(p + ndp + 6);
        for (size_t e = ndp + 8; e + 4 <= (size_t)ndp + ndpLen && e + 4 <= n; e += 4) {
            uint16_t at = rd16(p + e), len = rd16(p + e + 2);
            if (!at || !len) break;
            if ((size_t)at + len <= n) toStack(p + at, len);
        }
        ndp = next;
    }
}

void inCb(usb_transfer_t *t)
{
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && g_up) {
        size_t n = t->actual_num_bytes;
        if (g_proto == Proto::Rndis) parseRndis(t->data_buffer, n);
        else if (g_proto == Proto::Ncm) parseNcm(t->data_buffer, n);
        else toStack(t->data_buffer, n);
    }
    bool again = g_up && (t->status == USB_TRANSFER_STATUS_COMPLETED || t->status == USB_TRANSFER_STATUS_TIMED_OUT ||
                          t->status == USB_TRANSFER_STATUS_OVERFLOW);
    if (again) {
        t->num_bytes = RX_BUF;
        if (usb_host_transfer_submit(t) == ESP_OK) return;
    }
    g_inFlight--;
}

void outCb(usb_transfer_t *t)
{
    g_inFlight--;
    xQueueSend(g_outFree, &t, 0);
}

esp_err_t drvTransmit(void *h, void *buf, size_t len)
{
    if (!g_up || len > 1514) return ESP_FAIL;
    usb_transfer_t *t;
    if (xQueueReceive(g_outFree, &t, 0) != pdTRUE) {   // all in flight: drop (TCP resends)
        g_info.txDropped++;
        return ESP_OK;
    }
    uint8_t *o = t->data_buffer;
    size_t total;
    if (g_proto == Proto::Rndis) {
        memset(o, 0, 44);
        wr32(o, 1);               // REMOTE_NDIS_PACKET_MSG
        wr32(o + 4, 44 + len);
        wr32(o + 8, 36);          // data offset, from byte 8
        wr32(o + 12, len);
        memcpy(o + 44, buf, len);
        total = 44 + len;
    } else if (g_proto == Proto::Ncm) {
        // NTH16 (12) | NDP16 (16: one datagram and the end) | the datagram,
        // placed as the phone asked (offset % divisor == remainder).
        size_t ndp = (12 + g_ncmOutAlign - 1) / g_ncmOutAlign * g_ncmOutAlign;
        size_t at = ndp + 16;
        while ((at % g_ncmOutDiv) != (g_ncmOutRem % g_ncmOutDiv)) at++;
        total = at + len;
        if (total > TX_BUF || total > g_ncmOutMax) { xQueueSend(g_outFree, &t, 0); return ESP_FAIL; }
        memset(o, 0, at);
        memcpy(o, "NCMH", 4);
        wr16(o + 4, 12);
        wr16(o + 6, g_ncmSeq++);
        wr16(o + 8, total);
        wr16(o + 10, ndp);
        memcpy(o + ndp, "NCM0", 4);
        wr16(o + ndp + 4, 16);
        wr16(o + ndp + 6, 0);
        wr16(o + ndp + 8, at);
        wr16(o + ndp + 10, len);
        memcpy(o + at, buf, len);
    } else {
        memcpy(o, buf, len);
        total = len;
    }
    t->num_bytes = total;
    t->flags = (total % g_mpsOut) == 0 ? USB_TRANSFER_FLAG_ZERO_PACK : 0;
    t->device_handle = g_dev;
    t->bEndpointAddress = g_epOut;
    t->callback = outCb;
    t->timeout_ms = 2000;
    g_inFlight++;
    if (usb_host_transfer_submit(t) != ESP_OK) {
        g_inFlight--;
        xQueueSend(g_outFree, &t, 0);
        g_info.txDropped++;
        return ESP_OK;
    }
    g_info.txFrames++;
    return ESP_OK;
}

// ── Finding the network function ────────────────────────────────────────────

struct Found {
    Proto   proto = Proto::None;
    int     comm = -1, data = -1, dataAlt = 0, dataWanted = -1;
    uint8_t epIn = 0, epOut = 0, macIdx = 0;
    uint16_t mpsIn = 64, mpsOut = 64;
};

Found findFunction(const usb_config_desc_t *cfg)
{
    Found f;
    const uint8_t *p = (const uint8_t *)cfg;
    size_t total = cfg->wTotalLength, off = 0;
    int curIf = -1, curAlt = 0, curCls = -1;
    uint8_t in = 0, out = 0;
    uint16_t mIn = 64, mOut = 64;
    while (off + 2 <= total) {
        uint8_t len = p[off], type = p[off + 1];
        if (len < 2 || off + len > total) break;
        const uint8_t *d = p + off;
        if (type == 0x04 && len >= 9) {   // interface
            curIf = d[2]; curAlt = d[3]; curCls = d[5];
            in = out = 0;
            uint8_t sub = d[6], prot = d[7];
            if (f.comm < 0) {
                Proto pr = Proto::None;
                if ((curCls == 0xE0 && sub == 0x01 && prot == 0x03) || (curCls == 0x02 && sub == 0x02 && prot == 0xFF) ||
                    (curCls == 0xEF && sub == 0x04 && prot == 0x01))
                    pr = Proto::Rndis;
                else if (curCls == 0x02 && sub == 0x0D) pr = Proto::Ncm;
                else if (curCls == 0x02 && sub == 0x06) pr = Proto::Ecm;
                if (pr != Proto::None) { f.proto = pr; f.comm = curIf; }
            }
        } else if (type == 0x24 && len >= 4 && f.comm >= 0 && curIf == f.comm) {   // class-specific
            if (d[2] == 0x06 && len >= 5) f.dataWanted = d[4];      // union: the data interface
            else if (d[2] == 0x0F) f.macIdx = d[3];                 // Ethernet: its MAC string
        } else if (type == 0x05 && len >= 7 && f.comm >= 0 && f.data < 0 && curCls == 0x0A &&
                   (f.dataWanted < 0 || curIf == f.dataWanted) && (d[3] & 3) == 2) {   // bulk endpoint
            uint16_t mps = rd16(d + 4) & 0x7FF;
            if (d[2] & 0x80) { in = d[2]; mIn = mps; } else { out = d[2]; mOut = mps; }
            if (in && out) {
                f.data = curIf; f.dataAlt = curAlt;
                f.epIn = in; f.epOut = out; f.mpsIn = mIn; f.mpsOut = mOut;
            }
        }
        off += len;
    }
    if (f.data < 0) f.proto = Proto::None;
    return f;
}

void productName(usb_device_handle_t dev, char *out, size_t n)
{
    out[0] = 0;
    usb_device_info_t di;
    if (usb_host_device_info(dev, &di) != ESP_OK || !di.str_desc_product) return;
    const usb_str_desc_t *s = di.str_desc_product;
    size_t k = 0;
    for (int i = 0; i < (s->bLength - 2) / 2 && k + 1 < n; i++) {
        uint16_t c = s->wData[i];
        out[k++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    out[k] = 0;
}

// ── Setting the function up ─────────────────────────────────────────────────

// RNDIS: a message, then its reply (polled: the reply may take a moment).
bool rndisCmd(uint8_t *msg, uint16_t len, uint32_t wantType, uint8_t *reply, int *replyLen)
{
    if (!ctrl(0x21, 0x00, 0, g_ifComm, msg, len)) return false;   // SEND_ENCAPSULATED_COMMAND
    for (int i = 0; i < 40; i++) {
        int n = 0;
        if (ctrl(0xA1, 0x01, 0, g_ifComm, reply, 1024, &n) && n >= 16 && rd32(reply) == wantType) {   // GET_ENCAPSULATED_RESPONSE
            *replyLen = n;
            return rd32(reply + 12) == 0;   // status: success
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    return false;
}

bool rndisSetup(uint8_t mac[6])
{
    static uint8_t m[64], r[1024];
    int n;
    // INITIALIZE (the largest transfer we take in: one bulk IN).
    memset(m, 0, 24);
    wr32(m, 2); wr32(m + 4, 24); wr32(m + 8, g_rndisReq++); wr32(m + 12, 1); wr32(m + 16, 0); wr32(m + 20, RX_BUF);
    if (!rndisCmd(m, 24, 0x80000002, r, &n)) { LOG.println("[usb] RNDIS initialize failed"); return false; }
    // QUERY the MAC address this side is to use.
    memset(m, 0, 28);
    wr32(m, 4); wr32(m + 4, 28); wr32(m + 8, g_rndisReq++); wr32(m + 12, 0x01010101);
    if (rndisCmd(m, 28, 0x80000004, r, &n)) {
        uint32_t il = rd32(r + 16), io = rd32(r + 20);
        if (il >= 6 && 8 + io + 6 <= (uint32_t)n) memcpy(mac, r + 8 + io, 6);
    }
    // SET the packet filter: frames start to flow.
    memset(m, 0, 32);
    wr32(m, 5); wr32(m + 4, 32); wr32(m + 8, g_rndisReq++); wr32(m + 12, 0x0001010E);
    wr32(m + 16, 4); wr32(m + 20, 20); wr32(m + 28, 0x2D);   // directed, all multicast, broadcast, promiscuous
    if (!rndisCmd(m, 32, 0x80000005, r, &n)) { LOG.println("[usb] RNDIS packet filter failed"); return false; }
    return true;
}

bool macFromString(uint8_t idx, uint8_t mac[6])
{
    if (!idx) return false;
    static uint8_t b[64];
    int n = 0;
    if (!ctrl(0x80, 0x06, 0x0300 | idx, 0x0409, b, sizeof b, &n) || n < 2 + 24) return false;
    for (int i = 0; i < 6; i++) {
        uint8_t v = 0;
        for (int j = 0; j < 2; j++) {
            char c = (char)b[2 + (i * 2 + j) * 2];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        mac[i] = v;
    }
    return true;
}

bool cdcSetup(const Found &f, uint8_t mac[6])
{
    // The data interface's setting 0 first (no endpoints: the function resets).
    ctrl(0x01, 0x0B, 0, g_ifData, nullptr, 0);
    if (g_proto == Proto::Ncm) {
        static uint8_t p[28];
        int n = 0;
        if (ctrl(0xA1, 0x80, 0, g_ifComm, p, 28, &n) && n >= 28) {   // GET_NTB_PARAMETERS
            g_ncmOutMax = rd32(p + 16);
            g_ncmOutDiv = rd16(p + 20) ? rd16(p + 20) : 4;
            g_ncmOutRem = rd16(p + 22);
            g_ncmOutAlign = rd16(p + 24) ? rd16(p + 24) : 4;
            uint32_t inMax = rd32(p + 4);
            if (inMax > RX_BUF) {   // ask for blocks that fit our buffer
                uint8_t v[4];
                wr32(v, RX_BUF);
                ctrl(0x21, 0x86, 0, g_ifComm, v, 4);   // SET_NTB_INPUT_SIZE
            }
            LOG.printf("[usb] NCM: in %u, out %u (div %u rem %u align %u)\n", (unsigned)inMax,
                          (unsigned)g_ncmOutMax, g_ncmOutDiv, g_ncmOutRem, g_ncmOutAlign);
        }
        if (g_ncmOutMax < 64) g_ncmOutMax = 2048;
    }
    ctrl(0x21, 0x43, 0x000E, g_ifComm, nullptr, 0);   // SET_ETHERNET_PACKET_FILTER: directed, broadcast, all multicast
    if (!ctrl(0x01, 0x0B, f.dataAlt, g_ifData, nullptr, 0)) {   // SET_INTERFACE: the endpoints come up
        LOG.println("[usb] set interface failed");
        return false;
    }
    macFromString(f.macIdx, mac);
    return true;
}

void stopTransfers()
{
    g_up = false;
    if (g_dev && g_claimed) {
        usb_host_endpoint_halt(g_dev, g_epIn);
        usb_host_endpoint_flush(g_dev, g_epIn);
        usb_host_endpoint_halt(g_dev, g_epOut);
        usb_host_endpoint_flush(g_dev, g_epOut);
    }
    for (int i = 0; i < 100 && g_inFlight > 0; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (g_dev && g_claimed) {
        usb_host_endpoint_clear(g_dev, g_epIn);
        usb_host_endpoint_clear(g_dev, g_epOut);
    }
}

// The network function stopped; the device stays open (so that its
// unplugging still comes here) unless close.
void dropDevice(bool close)
{
    stopTransfers();
    netifDown();
    if (g_dev) {
        if (g_claimed) usb_host_interface_release(g_client, g_dev, g_ifData);
        if (close) usb_host_device_close(g_client, g_dev);
    }
    g_claimed = false;
    if (close) g_dev = nullptr;
    g_proto = Proto::None;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_info.proto = "";
    g_info.product[0] = 0;
    memset(&g_info.ip, 0, sizeof g_info.ip);
    g_info.dns.addr = 0;
    xSemaphoreGive(g_lock);
}

void openDevice(uint8_t addr)
{
    if (g_dev) dropDevice(true);
    if (usb_host_device_open(g_client, addr, &g_dev) != ESP_OK) { g_dev = nullptr; return; }
    char name[40];
    productName(g_dev, name, sizeof name);
    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(g_dev, &cfg) != ESP_OK) { dropDevice(true); return; }
    Found f = findFunction(cfg);
    if (f.proto == Proto::None) {
        // The phone with tethering off (file transfer / charging only): it
        // comes back as a new device when tethering is turned on.
        LOG.printf("[usb] %s: no network function (turn USB tethering on)\n", name[0] ? name : "device");
        xSemaphoreTake(g_lock, portMAX_DELAY);
        strlcpy(g_info.product, name, sizeof g_info.product);
        xSemaphoreGive(g_lock);
        setState(TetherState::NotTether);
        return;
    }
    g_proto = f.proto;
    g_ifComm = f.comm; g_ifData = f.data; g_altData = f.dataAlt;
    g_epIn = f.epIn; g_epOut = f.epOut; g_mpsIn = f.mpsIn; g_mpsOut = f.mpsOut ? f.mpsOut : 64;
    const char *pn = f.proto == Proto::Rndis ? "RNDIS" : f.proto == Proto::Ncm ? "NCM" : "ECM";
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_info.proto = pn;
    strlcpy(g_info.product, name, sizeof g_info.product);
    xSemaphoreGive(g_lock);
    setState(TetherState::Starting);
    LOG.printf("[usb] %s: %s (comm %d, data %d alt %d, in 0x%02x out 0x%02x mps %u)\n", name, pn, g_ifComm,
                  g_ifData, g_altData, g_epIn, g_epOut, g_mpsIn);

    // Our MAC unless the phone names one: the ESP's own, locally administered.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_ETH);
    mac[0] = (mac[0] | 0x02) & 0xFE;
    bool ok = f.proto == Proto::Rndis ? true : cdcSetup(f, mac);
    if (ok && usb_host_interface_claim(g_client, g_dev, g_ifData, g_altData) != ESP_OK) {
        LOG.println("[usb] claim failed");
        ok = false;
    }
    if (ok) g_claimed = true;
    if (ok && f.proto == Proto::Rndis) ok = rndisSetup(mac);
    if (!ok) { dropDevice(false); setState(TetherState::Error); return; }

    g_ncmSeq = 0;
    g_up = true;
    netifUp(mac);
    g_in->device_handle = g_dev;
    g_in->bEndpointAddress = g_epIn;
    g_in->callback = inCb;
    g_in->num_bytes = RX_BUF;
    g_in->timeout_ms = 0;
    g_inFlight++;
    if (usb_host_transfer_submit(g_in) != ESP_OK) {
        g_inFlight--;
        dropDevice(false);
        setState(TetherState::Error);
        return;
    }
    setState(TetherState::NoAddress);
}

// ── Tasks ───────────────────────────────────────────────────────────────────

void cliEvent(const usb_host_client_event_msg_t *msg, void *arg)
{
    Ev e = {};
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) { e.kind = 1; e.addr = msg->new_dev.address; }
    else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) { e.kind = 2; e.dev = msg->dev_gone.dev_hdl; }
    else return;
    xQueueSend(g_evq, &e, 0);
}

void libTask(void *)
{
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

void cliTask(void *)
{
    for (;;) usb_host_client_handle_events(g_client, portMAX_DELAY);
}

void workTask(void *)
{
    for (;;) {
        Ev e;
        if (xQueueReceive(g_evq, &e, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (e.kind == 1) openDevice(e.addr);
            else if (e.kind == 2 && e.dev == g_dev) {
                LOG.println("[usb] device gone");
                dropDevice(true);
                setState(TetherState::Waiting);
            }
            continue;
        }
        // The phone's DHCP: up once it has given an address.
        if (g_netifStarted) {
            esp_netif_ip_info_t ip;
            esp_netif_dns_info_t dns = {};
            esp_netif_get_ip_info(g_netif, &ip);
            esp_netif_get_dns_info(g_netif, ESP_NETIF_DNS_MAIN, &dns);
            xSemaphoreTake(g_lock, portMAX_DELAY);
            g_info.ip = ip;
            g_info.dns = dns.ip.u_addr.ip4;
            xSemaphoreGive(g_lock);
            TetherState want = ip.ip.addr ? TetherState::Up : TetherState::NoAddress;
            if (g_info.state != want) {
                setState(want);
                if (want == TetherState::Up)
                    LOG.printf("[usb] address " IPSTR "  gateway " IPSTR "  dns " IPSTR "\n", IP2STR(&ip.ip),
                                  IP2STR(&ip.gw), IP2STR(&g_info.dns));
            }
        }
    }
}

}   // namespace

bool tetherBegin()
{
    if (g_running) return true;
    g_lock = xSemaphoreCreateMutex();
    g_ctrlDone = xSemaphoreCreateBinary();
    g_evq = xQueueCreate(8, sizeof(Ev));
    g_outFree = xQueueCreate(TX_COUNT, sizeof(usb_transfer_t *));
    g_info.state = TetherState::Off;
    g_info.proto = "";

    usb_host_config_t hc = {};
    hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
    esp_err_t err = usb_host_install(&hc);
    if (err != ESP_OK) {
        LOG.printf("[usb] host install failed: %s\n", esp_err_to_name(err));
        return false;
    }
    xTaskCreatePinnedToCore(libTask, "usbLib", 4096, nullptr, 5, nullptr, 0);

    usb_host_client_config_t cc = {};
    cc.is_synchronous = false;
    cc.max_num_event_msg = 8;
    cc.async.client_event_callback = cliEvent;
    cc.async.callback_arg = nullptr;
    if (usb_host_client_register(&cc, &g_client) != ESP_OK) {
        LOG.println("[usb] client register failed");
        return false;
    }
    if (usb_host_transfer_alloc(CTRL_BUF, 0, &g_ctrl) != ESP_OK || usb_host_transfer_alloc(RX_BUF, 0, &g_in) != ESP_OK)
        return false;
    for (int i = 0; i < TX_COUNT; i++) {
        if (usb_host_transfer_alloc(TX_BUF, 0, &g_out[i]) != ESP_OK) return false;
        xQueueSend(g_outFree, &g_out[i], 0);
    }
    netifCreate();
    xTaskCreatePinnedToCore(cliTask, "usbCli", 4096, nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(workTask, "usbWork", 6144, nullptr, 4, nullptr, 0);
    g_running = true;
    setState(TetherState::Waiting);
    LOG.println("[usb] host started: waiting for a phone (USB tethering)");
    return true;
}

bool tetherRunning() { return g_running; }

void tetherInfo(TetherInfo *out)
{
    if (!g_lock) { memset(out, 0, sizeof *out); out->state = TetherState::Off; out->proto = ""; return; }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    *out = g_info;
    xSemaphoreGive(g_lock);
}

esp_netif_t *tetherNetif() { return (g_running && g_info.state == TetherState::Up) ? g_netif : nullptr; }

const char *tetherStateText(TetherState s, bool ja)
{
    switch (s) {
    case TetherState::Off:       return ja ? "停止中" : "off";
    case TetherState::Waiting:   return ja ? "スマホ待ち（USB に接続してください）" : "waiting for a phone";
    case TetherState::NotTether: return ja ? "スマホの USB テザリングを ON にしてください" : "turn USB tethering on on the phone";
    case TetherState::Starting:  return ja ? "接続中…" : "starting";
    case TetherState::NoAddress: return ja ? "アドレス待ち（スマホの DHCP）" : "waiting for an address";
    case TetherState::Up:        return ja ? "接続中（インターネットへ出られます）" : "up (internet via the phone)";
    case TetherState::Error:     return ja ? "スマホのネットワーク機能を開始できません" : "could not start the phone's network";
    }
    return "";
}
