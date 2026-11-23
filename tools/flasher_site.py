"""Builds the web flasher site (GitHub Pages) from a release's build outputs:
docs/flasher/index.html, plus per device a folder of .bin files and an
ESP Web Tools manifest whose offsets come from that build's partitions.bin.
devices.json lists the same offsets for the page's "write your own files"
part.

    python3 tools/flasher_site.py <site dir> <release tag> [--full <dir>]

--full also writes one image per device (everything at its offset, to be
written at 0x0) into <dir>, for esptool or other flashers.

Run from the repository root after building (publish-release.yaml). Only the
regions below are written, never the NVS: a plain install keeps the
settings (ESP Web Tools asks whether to erase first).
"""
import glob
import json
import os
import shutil
import struct
import sys

# Second-stage bootloader offset per chip.
BOOTLOADER_AT = {"ESP32": 0x1000, "ESP32-S3": 0x0, "ESP32-C3": 0x0, "ESP32-C5": 0x2000}


def partitions(path):
    """(type, subtype, offset, size, label) of each entry of a partitions.bin."""
    out = []
    data = open(path, "rb").read()
    for i in range(0, len(data), 32):
        e = data[i:i + 32]
        if len(e) < 32 or e[:2] != b"\xaa\x50":
            break
        ptype, sub, off, size = struct.unpack("<BBII", e[2:12])
        out.append((ptype, sub, off, size, e[12:28].split(b"\0")[0].decode()))
    return out


def find(parts, ptype, subtypes):
    for p in parts:
        if p[0] == ptype and p[1] in subtypes:
            return p[2]
    raise SystemExit("partition type %d subtype %s not found" % (ptype, subtypes))


def boot_app0():
    """Arduino's otadata image that selects app0 (where firmware.bin goes):
    after an online update the netpack may run from app1."""
    hits = glob.glob(os.path.expanduser("~/.platformio/packages/framework-arduinoespressif32*/tools/partitions/boot_app0.bin"))
    if not hits:
        raise SystemExit("boot_app0.bin not found")
    return hits[0]


def device(site, key, name, chip, build, full_name):
    """Copies a device's files into site/<key>/; returns its description."""
    parts = partitions(os.path.join(build, "partitions.bin"))
    app = find(parts, 0, (0x00, 0x10))           # factory or ota_0
    ota = find(parts, 1, (0x00,))                 # otadata
    files = [("bootloader.bin", os.path.join(build, "bootloader.bin"), BOOTLOADER_AT[chip]),
             ("partitions.bin", os.path.join(build, "partitions.bin"), 0x8000),
             ("otadata.bin", boot_app0(), ota),
             ("firmware.bin", os.path.join(build, "firmware.bin"), app)]
    os.makedirs(os.path.join(site, key), exist_ok=True)
    out_parts = []
    for fname, src, off in files:
        shutil.copy(src, os.path.join(site, key, fname))
        out_parts.append({"path": "%s/%s" % (key, fname), "offset": off})
    return {"key": key, "name": name, "chip": chip, "parts": out_parts, "files": files, "full": full_name}


def full_image(dev, path):
    """Every part at its offset (0xFF between), to be written at 0x0."""
    end = max(off + os.path.getsize(src) for _, src, off in dev["files"])
    img = bytearray(b"\xff" * end)
    for _, src, off in dev["files"]:
        data = open(src, "rb").read()
        img[off:off + len(data)] = data
    with open(path, "wb") as f:
        f.write(img)


def main():
    args = sys.argv[1:]
    full_dir = None
    if "--full" in args:
        i = args.index("--full")
        full_dir = args[i + 1]
        del args[i:i + 2]
    site, tag = args[0], args[1]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.makedirs(site, exist_ok=True)
    shutil.copy(os.path.join(root, "docs", "flasher", "index.html"), os.path.join(site, "index.html"))
    s3 = os.path.join(root, "elrs-xiao-webui", ".pio", "build", "xiao_esp32s3")
    br = os.path.join(root, "elrs-espnow-bridge", ".pio", "build")
    devices = [
        device(site, "netpack-s3", "ELRS-NETpack（XIAO ESP32-S3）", "ESP32-S3", s3,
               "elrs-netpack-%s-xiao_esp32s3-full.bin" % tag),
        device(site, "bridge-c3", "ELRSバックパック-ブリッジ（XIAO ESP32-C3）", "ESP32-C3",
               os.path.join(br, "xiao_esp32c3"), "elrs-espnow-bridge-%s-xiao_esp32c3-full.bin" % tag),
        device(site, "bridge-esp32", "ELRSバックパック-ブリッジ（ESP32 Wrover-E）", "ESP32",
               os.path.join(br, "esp32dev"), "elrs-espnow-bridge-%s-esp32dev-full.bin" % tag),
        device(site, "bridge-c5", "ELRSバックパック-ブリッジ（XIAO ESP32-C5）", "ESP32-C5",
               os.path.join(br, "xiao_esp32c5"), "elrs-espnow-bridge-%s-xiao_esp32c5-full.bin" % tag),
    ]
    for d in devices:
        manifest = {
            "name": d["name"],
            "version": tag,
            "new_install_prompt_erase": True,   # asks; a plain install keeps the settings
            "new_install_improv_wait_time": 0,
            "builds": [{"chipFamily": d["chip"], "parts": d["parts"]}],
        }
        with open(os.path.join(site, "manifest-%s.json" % d["key"]), "w") as f:
            json.dump(manifest, f, ensure_ascii=False, indent=1)
        if full_dir:
            os.makedirs(full_dir, exist_ok=True)
            full_image(d, os.path.join(full_dir, d["full"]))
    # The offsets per device, for writing files of one's own.
    with open(os.path.join(site, "devices.json"), "w") as f:
        json.dump([{"key": d["key"], "name": d["name"], "chip": d["chip"],
                    "offsets": {fname: off for fname, _, off in d["files"]}} for d in devices],
                  f, ensure_ascii=False, indent=1)
    with open(os.path.join(site, "version.json"), "w") as f:
        json.dump({"version": tag}, f)
    print("flasher site for %s in %s: %s" % (tag, site, ", ".join(d["key"] for d in devices)))


if __name__ == "__main__":
    main()
