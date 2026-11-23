#pragma once
// USB tethering: a smartphone on the XIAO's USB-C port (the S3 is the USB
// host) as the netpack's way out to the internet. Android's "USB tethering"
// shows the phone as an RNDIS network card (most phones) or a CDC-NCM /
// CDC-ECM one (newer Pixels and others); the phone hands out an address
// (DHCP) and routes to its mobile data or WiFi.
//
// While it runs the USB port is the host's: no USB serial log and no
// upload over USB (hold BOOT while plugging in to upload).
#include <Arduino.h>
#include "esp_netif.h"

enum class TetherState : uint8_t {
    Off,          // not started (setting off, or not host mode)
    Waiting,      // nothing on the port
    NotTether,    // a device, but no network function (tethering off on the phone)
    Starting,     // network function found, setting it up
    NoAddress,    // up, waiting for the phone's DHCP
    Up,           // address from the phone: the way out
    Error,        // the phone's network function would not start
};

struct TetherInfo {
    TetherState state;
    const char *proto;     // "RNDIS" / "NCM" / "ECM" / ""
    char        product[40];
    esp_netif_ip_info_t ip;
    esp_ip4_addr_t      dns;
    uint32_t    rxFrames, txFrames, txDropped;
};

// Starts the USB host and the network interface (once).
bool tetherBegin();
bool tetherRunning();
void tetherInfo(TetherInfo *out);
// The phone's network, when it has given an address (else nullptr).
esp_netif_t *tetherNetif();
const char *tetherStateText(TetherState s, bool ja);
