# XIAO ESP32-S3 セットアップガイド
# XIAO ESP32-S3 Setup Guide

---

## 概要 / Overview

このブランチは Waveshare ESP32-S3 Ethernet（Ethernet/W5500）と
**Seeed Studio XIAO ESP32-S3（WiFi STA）** の両方を
menuconfig で切り替えられるよう拡張したものです。

This branch adds WiFi STA support alongside the existing Ethernet (W5500) interface,
selectable via `idf.py menuconfig`.

```
[RotorHazard (Raspberry Pi)]
        ↓ TCP port 8080
   XIAO ESP32-S3  ─── WiFi LAN ───  Router
        ↓ ESPNow (2.4 GHz)
   ELRSバックパック / ELRS Backpack
```

---

## 目次 / Contents

1. [必要なもの / Requirements](#必要なもの)
2. [接続タイプの選択 / Choosing connection type](#接続タイプの選択)
3. [ファームウェア書き込み（Windows + VS Code）](#ファームウェア書き込み)
4. [RotorHazard プラグインのインストール](#rotorhazard-プラグインのインストール)
5. [動作確認 / Verification](#動作確認)
6. [トラブルシューティング / Troubleshooting](#トラブルシューティング)

---

## 必要なもの

| ハードウェア | 備考 |
|---|---|
| Seeed Studio XIAO ESP32-S3 | 外部アンテナモデル（Sense）推奨 |
| USB-C ケーブル | データ転送対応のもの |
| Windows PC | VS Code 書き込み用 |
| Raspberry Pi | RotorHazard 動作中 |
| WiFi ルーター | XIAO と RPi が同一ネットワーク |

| ソフトウェア | バージョン |
|---|---|
| VS Code | 最新版 |
| ESP-IDF 拡張機能（Espressif IDF） | v1.7 以上 |
| ESP-IDF | v5.4.1 |
| RotorHazard | v4.x 以上 |

---

## 接続タイプの選択

`idf.py menuconfig` → **TCP Socket Server options** → **Network connection type** で選択：

| 選択肢 | 対象ボード |
|---|---|
| `Ethernet (W5500 / Waveshare ESP32-S3)` | 元のデフォルト |
| `WiFi Station (XIAO ESP32-S3 etc.)` | **XIAO の場合はこちら** |

WiFi を選んだ場合、同メニュー内に **WiFi SSID** と **WiFi Password** 入力欄が表示されます。

`sdkconfig.defaults` に XIAO 用のデフォルト値が設定済みです：
- 接続タイプ: `WiFi Station`
- SSID: `y_air-GL`
- パスワード: `88888888`

---

## ファームウェア書き込み

### 1. ESP-IDF 拡張機能のインストール

1. VS Code を開く
2. 拡張機能タブ（Ctrl+Shift+X）で `Espressif IDF` を検索してインストール
3. 初回セットアップウィザードで **ESP-IDF v5.4.1** を選択

### 2. リポジトリをクローン

```powershell
git clone -b claude/wifi-espnow-bridge-jptTt `
  https://github.com/yanazoo/elrs-netpack
cd elrs-netpack
code .
```

### 3. ターゲット設定

```
Ctrl+Shift+P → ESP-IDF: Set Espressif Device Target → esp32s3
```

### 4. menuconfig で WiFi を選択・認証情報を入力

```
Ctrl+Shift+P → ESP-IDF: SDK Configuration Editor (Menuconfig)
→ TCP Socket Server options
  → Network connection type → WiFi Station (XIAO ESP32-S3 etc.)
  → WiFi SSID: （あなたのSSID）
  → WiFi Password: （あなたのパスワード）
→ XIAO ESP32-S3 options
  → Enable external U.FL antenna → 外部アンテナ使用時のみ有効化
```

> `sdkconfig.defaults` に y_air-GL / 88888888 が設定済みのため、
> 変更不要であればこの手順はスキップできます。

### 5. XIAO を USB 接続して書き込み

1. XIAO を USB-C ケーブルで Windows PC に接続
2. `Ctrl+Shift+P` → `ESP-IDF: Select Port to Use` → COM ポートを選択
3. `Ctrl+Shift+P` → `ESP-IDF: Build, Flash and Monitor`

書き込みに失敗する場合（ブートローダーモード）：
```
① BOOT ボタンを押したまま
② RST ボタンを押して離す
③ BOOT ボタンを離す
```

### 6. 成功時のシリアルモニター出力

```
I (xxxx) espnow_server: Connecting to AP: y_air-GL
I (xxxx) tcp_server: Server listening on port 8080
I (xxxx) tcp_server: WiFi Got IP Address
I (xxxx) tcp_server: IP:   192.168.x.xxx
```

---

## RotorHazard プラグインのインストール

**Raspberry Pi 上で実行**

```bash
# 1. リポジトリをクローン
git clone -b claude/wifi-espnow-bridge-jptTt --depth 1 \
  https://github.com/yanazoo/elrs-netpack /tmp/elrs-netpack

# 2. プラグインを上書きコピー（インストール先: ~/rh-data/plugins/）
cp -r /tmp/elrs-netpack/custom_plugins/netpack_installer \
      ~/rh-data/plugins/

# 3. 後始末
rm -rf /tmp/elrs-netpack

# 4. RotorHazard を再起動
sudo systemctl restart rotorhazard
# ※ サービス名が "rh" の場合: sudo systemctl restart rh
```

> **インストール先の注意**  
> 正しくは `~/rh-data/plugins/`、`~/RotorHazard/src/server/plugins/` では**ありません**。

---

## 動作確認

### RotorHazard の接続設定

```
Settings → ELRS バックパック 一般設定
  バックパック接続タイプ → SOCKET
  ELRS Netpack アドレス  → elrs-netpack.local
                           （または XIAO の IP アドレス）
  ポート                 → 8080
```

### プラグインパネルで確認

`Settings` → **「ELRS Netpack（XIAO ESP32-S3）」パネル** →  
「デバイス接続を確認」ボタン → `✓ デバイス接続確認: elrs-netpack.local:8080`

---

## トラブルシューティング

| 症状 | 対処 |
|---|---|
| WiFi に繋がらない | menuconfig で SSID/パスワードを再確認 → 再書き込み |
| `elrs-netpack.local` が見つからない | XIAO と RPi が同じネットワークか確認、IP 直指定を試す |
| 書き込めない | BOOTモードで接続（上記手順参照） |
| プラグインが表示されない | `sudo journalctl -u rotorhazard -n 50` でエラー確認 |
| `ImportError: esptool` | `pip3 install esptool requests` を RPi で実行 |

---

## 関連リンク

- ファームウェアリポジトリ: https://github.com/yanazoo/elrs-netpack
- XIAO ESP32-S3 ドキュメント: https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/
- RotorHazard: https://github.com/RotorHazard/RotorHazard
- ESP-IDF 拡張機能: https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension
