# ELRS Netpack – XIAO ESP32-S3 WiFi 対応版 / WiFi Variant

> [!IMPORTANT]
> このフォークは **Seeed Studio XIAO ESP32-S3**（WiFi STA モード）向けに改造したものです。
> 元プロジェクト（Waveshare ESP32-S3 Ethernet）とは**ハードウェアが異なります**。
>
> This fork targets the **Seeed Studio XIAO ESP32-S3** (WiFi STA mode).
> It is **not** compatible with the original Waveshare ESP32-S3 Ethernet board.

> [!NOTE]
> このプロジェクトは ExpressLRS organization と公式な関係はありません。
> This project is **not** officially affiliated or supported by the ExpressLRS organization.

## 概要 / Overview

XIAO ESP32-S3 を WiFi ← → ESP-NOW ブリッジとして動作させ、RotorHazard タイマー
バックパック（タイマーバックパック相当）を構築するファームウェアです。  
TCP ソケットで RotorHazard と通信し、ESP-NOW で ELRS バックパックデバイスと通信します。

This firmware turns the XIAO ESP32-S3 into a WiFi ↔ ESP-NOW bridge, acting as the
equivalent of an ELRS timer backpack. It communicates with RotorHazard over TCP and
with ELRS backpack devices over ESP-NOW.

---

## ファームウェア書き込み / Firmware Flashing

### 方法 A：Windows PC + VS Code（推奨 / Recommended）

1. **ESP-IDF 拡張機能**を VS Code にインストール
2. このリポジトリをクローンして VS Code で開く
3. `sdkconfig.defaults` の WiFi SSID・パスワードを確認（デフォルト: `y_air-GL` / `88888888`）
4. 変更が必要な場合は `Ctrl+Shift+P` → **"ESP-IDF: SDK Configuration Editor (Menuconfig)"**  
   → "WiFi / XIAO ESP32-S3 options" で設定
5. XIAO を USB 接続 → `Ctrl+Shift+P` → **"ESP-IDF: Select Port to Use"** で COM ポート選択
6. `Ctrl+Shift+P` → **"ESP-IDF: Build, Flash and Monitor"** で書き込み

または、`Ctrl+Shift+B` → タスク一覧から選択して実行することもできます。

### 方法 B：コマンドライン（Linux / Raspberry Pi）

```bash
# ESP-IDF v5.4 のセットアップ済み環境で
idf.py set-target esp32s3
idf.py menuconfig    # WiFi SSID / パスワードを設定
idf.py build
idf.py -p /dev/ttyACM0 flash
```

---

## RotorHazard プラグインのインストール / Plugin Installation

このリポジトリに含まれるプラグイン（`custom_plugins/netpack_installer/`）を  
RotorHazard の **データディレクトリ内 `plugins/`** にコピーします。

```bash
# Raspberry Pi 上で実行
# RotorHazard データディレクトリは通常 ~/rh-data
cp -r custom_plugins/netpack_installer ~/rh-data/plugins/
sudo systemctl restart rotorhazard
```

> [!NOTE]
> インストール先は `~/RotorHazard/src/server/plugins/` **ではありません**。  
> 正しくは RotorHazard の**データディレクトリ** `~/rh-data/plugins/` です。  
> The correct path is `~/rh-data/plugins/`, **not** `~/RotorHazard/src/server/plugins/`.

### プラグインでできること / Plugin Features

| ボタン | 機能 |
|---|---|
| ポート一覧を更新 | 接続中のシリアルポートを再スキャン |
| ファームウェアを書き込む（Pi 経由） | esptool でシリアル書き込み（Pi に USB 接続時） |
| デバイス接続を確認 | `elrs-netpack.local:8080` への TCP 疎通確認 |

---

## WiFi 設定 / WiFi Configuration

`sdkconfig.defaults` または menuconfig で設定します（コンパイル時設定）。

| 設定項目 | デフォルト値 |
|---|---|
| WIFI_STA_SSID | `y_air-GL` |
| WIFI_STA_PASSWORD | `88888888` |
| XIAO_EXTERNAL_ANTENNA | 無効（Sense 外部アンテナ使用時は有効化） |
| TCP_SERVER_PORT | `8080` |

接続後、RotorHazard から `elrs-netpack.local` または DHCP で割り当てられた IP アドレスで
デバイスを検出できます（mDNS 対応）。

---

## 3D プリントケース / 3D-Printable Case

元の Waveshare 基板用ケースデータが `resources/3d-case/` に含まれています（XIAO には非対応）。

[![3D-Printable Case](resources/3d-case/case-photo.jpg)](resources/3d-case/)
