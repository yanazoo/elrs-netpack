#pragma once
// Firmware update of the netpack XIAO, from its web UI (/update):
//   * online: "check" reads the newest release's version from GitHub, then
//     "update" downloads its firmware.bin and writes it (the netpack needs
//     the internet: a router in client mode, or USB tethering in host mode);
//   * a file: a firmware.bin from the browser.
// Release assets (publish-release.yaml) keep the same names in every
// release, so .../releases/latest/download/<name> is always the newest.
#include <Arduino.h>

const char *fwVersion();
const char *fwBuild();
// Routes: GET /update (page), POST /update (file), GET /api/update (state),
// POST /api/update/check, POST /api/update/online.
// authed: the browser is logged in (else /update goes to /login, the rest
// is refused).
void fwUpdateRoutes(bool (*online)(), String (*head)(const char *title), bool (*ja)(), bool (*authed)());
