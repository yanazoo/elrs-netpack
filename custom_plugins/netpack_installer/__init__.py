"""
ELRS Netpack Installer – RotorHazard プラグイン
ELRS Netpack Installer – RotorHazard Plugin

XIAO ESP32-S3 (WiFi STA) 対応版
Compatible with XIAO ESP32-S3 (WiFi STA) variant

GitHub: https://github.com/yanazoo/elrs-netpack
"""

import io
import logging
import socket
import sys
from pathlib import Path
from zipfile import ZipFile

import esptool
import gevent.lock
import gevent.subprocess
import requests
from eventmanager import Evt
from RHUI import UIField, UIFieldSelectOption, UIFieldType

logger = logging.getLogger(__name__)
_lock = gevent.lock.BoundedSemaphore()

# GitHub repository for firmware releases (yanazoo fork – XIAO WiFi variant)
GITHUB_REPO = "yanazoo/elrs-netpack"
GITHUB_API_URL = f"https://api.github.com/repos/{GITHUB_REPO}/releases"

# mDNS hostname and TCP port used by the XIAO netpack
NETPACK_HOSTNAME = "elrs-netpack.local"
NETPACK_PORT = 8080
STATUS_TIMEOUT = 2  # seconds


def _t(ja: str, en: str) -> str:
    """Return a bilingual label: Japanese first, English in parentheses."""
    return f"{ja}（{en}）"


class NetpackInstaller:

    def __init__(self, rhapi) -> None:
        self._rhapi = rhapi
        self._firmware_folder = Path(rhapi.server.data_dir).joinpath(
            "plugins/netpack_installer/firmware"
        )
        self._downloaded = False
        self.session = requests.Session()

        ver_green = gevent.spawn(self._get_download_versions)
        gevent.wait((ver_green,))
        self._versions = ver_green.value

        self.update_version_list()

    # ------------------------------------------------------------------
    # Firmware download helpers
    # ------------------------------------------------------------------

    def _get_download_versions(self) -> list:
        try:
            data = self.session.get(GITHUB_API_URL, timeout=5)
            data.raise_for_status()
            return data.json()
        except Exception as exc:
            logger.warning("Failed to fetch release list: %s", exc)
            return []

    def _download_firmware(self) -> bool:
        url = self._rhapi.db.option("_netpack_version")
        if url is None:
            msg = _t("バージョンが選択されていません", "Firmware version not selected")
            self._rhapi.ui.message_notify(msg)
            return False

        msg = _t("ファームウェアをダウンロード中...", "Downloading firmware")
        self._rhapi.ui.message_notify(msg)

        try:
            data_green = gevent.spawn(self.session.get, url)
            gevent.wait((data_green,))
            with ZipFile(io.BytesIO(data_green.value.content)) as zip_:
                self._firmware_folder.mkdir(parents=True, exist_ok=True)
                zip_.extractall(self._firmware_folder)
            self._downloaded = True
            return True
        except Exception as exc:
            logger.error("Download failed: %s", exc)
            msg = _t("ダウンロードに失敗しました", "Download failed")
            self._rhapi.ui.message_notify(msg)
            return False

    # ------------------------------------------------------------------
    # Serial flash (Raspberry Pi / Linux)
    # ------------------------------------------------------------------

    def flash_firmware(self, *_) -> None:
        """Flash firmware via USB serial port (for Raspberry Pi users).
        Windows ユーザーは VS Code + ESP-IDF 拡張機能で書き込んでください。
        Windows users: use VS Code with the ESP-IDF extension instead.
        """
        if _lock.locked():
            msg = _t("書き込み中です。お待ちください", "Flashing already in progress")
            self._rhapi.ui.message_notify(msg)
            return

        with _lock:
            if not self._downloaded:
                if not self._download_firmware():
                    return

            port = self._rhapi.db.option("_netpack_ports")
            if not port:
                msg = _t("ポートが選択されていません", "Port not selected")
                self._rhapi.ui.message_notify(msg)
                return

            boot = self._firmware_folder / "bootloader.bin"
            firm = self._firmware_folder / "elrs-netpack.bin"
            part = self._firmware_folder / "partition-table.bin"

            if not all(f.exists() for f in (boot, firm, part)):
                msg = _t("ファームウェアファイルが見つかりません", "Firmware files not found")
                self._rhapi.ui.message_notify(msg)
                return

            msg = _t("ファームウェアを書き込み中...", "Flashing firmware")
            self._rhapi.ui.message_notify(msg)

            process = gevent.subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "esptool",
                    "-p", port,
                    "-b", "460800",
                    "--before", "default_reset",
                    "--after", "hard_reset",
                    "--chip", "esp32s3",
                    "write_flash",
                    "--flash_mode", "dio",
                    "--flash_freq", "80m",
                    "--flash_size", "2MB",
                    "0x0",      str(boot.absolute()),
                    "0x10000",  str(firm.absolute()),
                    "0x8000",   str(part.absolute()),
                ],
            )

            try:
                process.check_returncode()
            except gevent.subprocess.CalledProcessError:
                logger.error("esptool stderr: %s", getattr(process, "stderr", ""))
                msg = _t("書き込み失敗", "Netpack flashing failed")
                self._rhapi.ui.message_notify(msg)
            else:
                msg = _t("書き込み完了 ✓", "Netpack flashing completed")
                self._rhapi.ui.message_notify(msg)

    # ------------------------------------------------------------------
    # Device status check
    # ------------------------------------------------------------------

    def check_device_status(self, *_) -> None:
        """Try to reach the XIAO netpack over TCP on the local network."""
        status_green = gevent.spawn(self._probe_tcp)
        gevent.wait((status_green,))
        if status_green.value:
            msg = _t(
                f"✓ デバイス接続確認: {NETPACK_HOSTNAME}:{NETPACK_PORT}",
                f"Device reachable at {NETPACK_HOSTNAME}:{NETPACK_PORT}",
            )
        else:
            msg = _t(
                f"✗ デバイスが見つかりません ({NETPACK_HOSTNAME})",
                f"Device not found ({NETPACK_HOSTNAME})",
            )
        self._rhapi.ui.message_notify(msg)

    def _probe_tcp(self) -> bool:
        try:
            with socket.create_connection(
                (NETPACK_HOSTNAME, NETPACK_PORT), timeout=STATUS_TIMEOUT
            ):
                return True
        except OSError:
            return False

    # ------------------------------------------------------------------
    # UI list updates
    # ------------------------------------------------------------------

    def update_port_list(self, *_) -> None:
        ports = esptool.get_port_list()
        options = [UIFieldSelectOption(value=p, label=p) for p in ports]
        if not options:
            options = [UIFieldSelectOption(value="", label=_t("なし", "None"))]

        field = UIField(
            "_netpack_ports",
            _t("シリアルポート", "Serial Port"),
            desc=_t(
                "XIAO が接続されているシリアルポート（Raspberry Pi 経由で書き込む場合）",
                "Serial port where the XIAO is connected (for Pi-side flashing)",
            ),
            field_type=UIFieldType.SELECT,
            options=options,
        )
        self._rhapi.fields.register_option(field, "netpack_panel")
        self._rhapi.ui.broadcast_ui("settings")

    def update_version_list(self, args=None) -> None:
        if args is not None and args.get("option") != "_netpack_beta":
            return

        allow_beta = self._rhapi.db.option("_netpack_beta", as_int=True)

        options = []
        for version in self._versions:
            if version.get("draft"):
                continue
            if not allow_beta and version.get("prerelease"):
                continue
            assets = version.get("assets", [])
            if not assets:
                continue
            tag = version["tag_name"]
            url = assets[0]["browser_download_url"]
            label = f"{tag}{'  [beta]' if version.get('prerelease') else ''}"
            options.append(UIFieldSelectOption(value=url, label=label))

        if not options:
            options = [UIFieldSelectOption(value="", label=_t("取得できませんでした", "No releases found"))]

        field = UIField(
            "_netpack_version",
            _t("ファームウェアバージョン", "Firmware Version"),
            desc=_t(
                "インストールするファームウェアのバージョン",
                "The netpack firmware version to install",
            ),
            field_type=UIFieldType.SELECT,
            options=options,
        )
        self._rhapi.fields.register_option(field, "netpack_panel")
        self._rhapi.ui.broadcast_ui("settings")

    def reset_download_status(self, args=None) -> None:
        if args is None or args.get("option") != "_netpack_version":
            return
        self._downloaded = False


# ----------------------------------------------------------------------
# Plugin entry point
# ----------------------------------------------------------------------

def initialize(rhapi):
    # Register the settings panel
    rhapi.ui.register_panel(
        "netpack_panel",
        _t("ELRS Netpack（XIAO ESP32-S3）", "ELRS Netpack Firmware"),
        "settings",
        order=0,
    )

    # Beta firmware toggle
    beta_field = UIField(
        "_netpack_beta",
        _t("ベータ版を有効化", "Enable Beta"),
        desc=_t(
            "ベータファームウェアのインストールを有効にします",
            "Enables installation of pre-release firmware",
        ),
        field_type=UIFieldType.CHECKBOX,
    )
    rhapi.fields.register_option(beta_field, "netpack_panel")

    installer = NetpackInstaller(rhapi)
    installer.update_port_list()

    # Refresh port list button
    rhapi.ui.register_quickbutton(
        "netpack_panel",
        "update_netpack_ports",
        _t("ポート一覧を更新", "Refresh Ports"),
        installer.update_port_list,
    )

    # Flash firmware button (serial / Raspberry Pi)
    rhapi.ui.register_quickbutton(
        "netpack_panel",
        "flash_netpack",
        _t("ファームウェアを書き込む（Pi 経由）", "Flash Firmware via Pi"),
        installer.flash_firmware,
    )

    # Device status check button
    rhapi.ui.register_quickbutton(
        "netpack_panel",
        "check_netpack_status",
        _t("デバイス接続を確認", "Check Device Status"),
        installer.check_device_status,
    )

    # React to option changes
    rhapi.events.on(Evt.OPTION_SET, installer.update_version_list, name="netpack_version_change")
    rhapi.events.on(Evt.OPTION_SET, installer.reset_download_status, name="netpack_select_change")
