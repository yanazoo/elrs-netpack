# ELRS Netpack — WiFi → ESP-NOW ブリッジ

[English](README.md) | 日本語

> [!IMPORTANT]
> このフォークは、ESP32-S3 の根本的な無線競合問題を解決するため、**デュアル MCU 構成**に変更しています。
> 元の Waveshare ESP32-S3 Ethernet ボードとは**互換性がありません**。

> [!NOTE]
> このプロジェクトは ExpressLRS organization と公式な関係はありません。

---

## 概要

ESP32-S3 は 2.4 GHz 無線を 1 つしか持っていないため、WiFi STA と ESP-NOW を同時に使用するとパケットロスやピアリストの破損が発生します。このフォークでは役割を 2 枚のボードに分離し、UART で接続することで問題を解決しています。

```
RotorHazard（Raspberry Pi）
        │  TCP ポート 8080
        ▼
┌─────────────────────┐
│  XIAO ESP32-S3      │  elrs-xiao-bridge/
│  WiFi STA + TCP +   │
│  mDNS               │
└────────┬────────────┘
         │  UART 115200 baud
         │  D0(GPIO1) TX ──► GPIO26 RX
         │  D1(GPIO2) RX ◄── GPIO27 TX
         │  GND        ◄──►  GND
         ▼
┌─────────────────────┐
│  ESP32 Wrover-E     │  elrs-espnow-bridge/
│  ESP-NOW 専用       │
└────────┬────────────┘
         │  ESP-NOW（2.4 GHz、ch 1）
         ▼
   ELRS VRx バックパック（FPV ゴーグル）
```

---

## 必要なハードウェア

| 部品 | 備考 |
|---|---|
| Seeed Studio XIAO ESP32-S3 | 外部アンテナモデル（Sense）推奨 |
| ESP32 Wrover-E | 4 MB 以上の Flash を持つ ESP32 開発ボードであれば代替可 |
| ジャンパーワイヤー 3 本 | TX / RX / GND |
| USB-C ケーブル | データ転送対応（XIAO 書き込み用） |
| USB-A / マイクロ USB ケーブル | ESP32 書き込み用 |
| Raspberry Pi | RotorHazard 動作中 |

---

## 配線

| XIAO ESP32-S3 | ESP32 Wrover-E |
|---|---|
| D0 / GPIO1（TX） | GPIO26（RX） |
| D1 / GPIO2（RX） | GPIO27（TX） |
| GND | GND |

> 3V3 / 5V はボード間で接続**しないでください** — 各ボードを USB で個別に給電してください。

---

## 事前準備

1. **VS Code** — [code.visualstudio.com](https://code.visualstudio.com/)
2. **PlatformIO IDE 拡張機能** — VS Code 拡張機能タブ（`Ctrl+Shift+X`）で "PlatformIO IDE" を検索してインストール
3. リポジトリをクローン：
   ```powershell
   git clone https://github.com/yanazoo/elrs-netpack
   cd elrs-netpack
   ```

---

## 初期設定

XIAO に書き込む前に `elrs-xiao-bridge/include/config.h` を編集してください：

```cpp
#define WIFI_SSID     "あなたのSSID"
#define WIFI_PASSWORD "あなたのパスワード"
```

> **セキュリティ注意:** `config.h` には WiFi パスワードが含まれています。公開リポジトリにはコミットしないでください。

---

## XIAO ESP32-S3 の書き込み（`elrs-xiao-bridge/`）

1. VS Code でフォルダを開く：
   **ファイル → フォルダーを開く → `elrs-xiao-bridge/`**
2. PlatformIO がパッケージをインストールするまで待つ（初回のみ）
3. XIAO を USB-C で接続
4. PlatformIO サイドバー → **`xiao_esp32s3` → General → Upload**

書き込みに失敗する場合はブートローダーモードで接続：
```
① BOOT ボタンを押したまま
② RST ボタンを押して離す
③ BOOT ボタンを離す
```

**正常起動時のシリアル出力：**
```
[boot] XIAO ESP32-S3 WiFi bridge
[wifi] connecting to あなたのSSID
[wifi] connected, IP=192.168.x.xxx
[mdns] elrs-netpack.local
[tcp] listening on port 8080
[boot] ready
```

---

## ESP32 Wrover-E の書き込み（`elrs-espnow-bridge/`）

1. VS Code でフォルダを開く：
   **ファイル → フォルダーを開く → `elrs-espnow-bridge/`**
2. ESP32 を USB で接続
3. PlatformIO サイドバー → **`esp32dev` → General → Upload**

**正常起動時のシリアル出力：**
```
[boot] ESP32 Wrover-E ESP-NOW bridge
[wifi] channel fixed to 1
[espnow] (re)initialized
[boot] ready
```

---

## RotorHazard 接続設定

```
Settings → ELRS バックパック 一般設定
  バックパック接続タイプ → SOCKET
  ELRS Netpack アドレス  → elrs-netpack.local
                           （または XIAO の IP アドレス）
  ポート                 → 8080
```

---

## RotorHazard プラグイン

プラグイン（`custom_plugins/netpack_installer/`）をインストールすると、RotorHazard の UI からデバイス接続確認やファームウェア書き込みができます。

**Raspberry Pi でのインストール：**
```bash
cp -r custom_plugins/netpack_installer ~/rh-data/plugins/
sudo systemctl restart rotorhazard
```

> インストール先は `~/rh-data/plugins/` です。`~/RotorHazard/src/server/plugins/` では**ありません**。

---

## トラブルシューティング

| 症状 | 対処 |
|---|---|
| WiFi AUTH_EXPIRE が繰り返される | `config.h` のパスワードを確認して再書き込み |
| `elrs-netpack.local` が見つからない | XIAO と RPi が同じネットワークか確認。IP アドレス直指定を試す |
| ESP-NOW send error | ゴーグルのバックパックが起動しているか確認 |
| OSD がゴーグルに表示されない | ESP-NOW チャンネルがバックパックと一致しているか確認（デフォルト ch 1） |
| PlatformIO でボードが見つからない | `pio platform update espressif32` を実行 |
| `No module named intelhex` | `C:\Users\<ユーザー名>\.platformio\penv\Scripts\python.exe -m pip install intelhex` を実行 |
| 書き込みに失敗する | BOOT モードで接続（上記手順参照） |

---

## 3D プリントケース

`resources/3d-case/` に元の Waveshare ボード用ケースデータが含まれています（XIAO のサイズには非対応）。

[![3D-Printable Case](resources/3d-case/case-photo.jpg)](resources/3d-case/)

---

## クレジット・フォーク情報

このプロジェクトは、Waveshare ESP32-S3 Ethernet ボード向けに設計された **ELRS Netpack** コンセプトのフォークです。

- 元の ELRS バックパックプロトコル: [ExpressLRS/backpack](https://github.com/ExpressLRS/backpack)
- RotorHazard: [RotorHazard/RotorHazard](https://github.com/RotorHazard/RotorHazard)
- Seeed Studio XIAO ESP32-S3 ドキュメント: [wiki.seeedstudio.com](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
- このフォーク: [yanazoo/elrs-netpack](https://github.com/yanazoo/elrs-netpack)

デュアル MCU ESP-NOW ブリッジ構成は、ESP32-S3 の 2.4 GHz 無線を WiFi STA と ESP-NOW で共有することによるハードウェア制限を解決するために開発されました。
