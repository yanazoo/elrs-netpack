# ELRS Netpack — TCP → WiFi → ESP-NOW ブリッジ

[English](README.md) | 日本語

> [!IMPORTANT]
> このフォークは **ESP32 を 2 台使用**して **TCP → WiFi → ESP-NOW** のバックパック通信システムを構築します。元の Waveshare ESP32-S3 Ethernet ボードとは**互換性がありません**。

> [!NOTE]
> このプロジェクトは ExpressLRS organization と公式な関係はありません。

---

## 概要

RotorHazard は MSP パケットを TCP で送信します。このプロジェクトはそのパケットを UART で接続した 2 台の ESP32 を経由して、ELRS VRx バックパックまで ESP-NOW で届けるシステムです。

```
RotorHazard（Raspberry Pi）
        │  TCP ポート 8080
        ▼
┌─────────────────────┐
│  XIAO ESP32-S3      │  elrs-xiao-bridge/
│  WiFi STA           │  TCP を受信して UART に転送
│  TCP サーバー + mDNS  │
└────────┬────────────┘
         │  UART 115200 baud
         │  D0(GPIO1) TX ──► GPIO26 RX
         │  D1(GPIO2) RX ◄── GPIO27 TX
         │  GND        ◄──►  GND
         ▼
┌─────────────────────┐
│  ESP32 Wrover-E     │  elrs-espnow-bridge/
│  ESP-NOW 専用        │  ESP-NOW パケットの送受信
└────────┬────────────┘
         │  ESP-NOW（2.4 GHz、ch 1）
         ▼
   ELRS VRx バックパック（FPV ゴーグル）
```

**なぜ 2 台必要か？**
ESP32-S3 の 2.4 GHz 無線は 1 つだけです。WiFi STA と ESP-NOW を同時に使うとパケットロスやピアリストの破損が発生するハードウェア上の制限があります。役割を 2 台のボードに分けて UART で接続することでこの問題を完全に解決しています。

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

## elrs-xiao-webui — XIAO 拡張ファームウェア（Web UI 版）

`elrs-xiao-webui/` は `elrs-xiao-bridge/` の機能強化版です。TCP MSP ブリッジ機能をそのまま維持しつつ、**Web UI・キャプティブポータル・バッテリー電圧監視・ブザー・LED 通知**を追加します。

> XIAO には `elrs-xiao-bridge/` **または** `elrs-xiao-webui/` のどちらか一方だけを書き込んでください。

---

### 追加ハードウェア（elrs-xiao-webui）

| 部品 | 備考 |
|---|---|
| パッシブブザー | GPIO4(+) / GPIO6(−) に接続 |
| 7色自動カラーサイクル LED | GPIO9（D10）+ **100 Ω 抵抗**を直列に接続 |
| 100 kΩ 抵抗 × 2 | LiPo 電圧監視用分圧回路（任意） |
| LiPo バッテリー | 1S 推奨（XIAO と ESP32 Wrover-E の共有電源） |

---

### 追加配線（elrs-xiao-webui）

| XIAO GPIO | ピン名 | 接続先 |
|---|---|---|
| GPIO3 | D2 / A0 | 分圧抵抗の中点（LiPo+ → R1 100kΩ → GPIO3 → R2 100kΩ → GND） |
| GPIO4 | D3 | ブザー プラス極（+） |
| GPIO6 | D5 | ブザー マイナス極（−） |
| GPIO9 | D10 | LED アノード（+ 100 Ω → GND） |
| GPIO21 | — | 内蔵 LED（負論理、基板上） |

> **重要:** 通知 LED には必ず **100 Ω** の直列抵抗を入れてください（GPIO9 保護のため）。

---

### XIAO ESP32-S3 への書き込み（`elrs-xiao-webui/`）

1. VS Code でフォルダを開く：
   **ファイル → フォルダーを開く → `elrs-xiao-webui/`**
2. PlatformIO がパッケージをインストールするまで待つ（初回のみ）
3. XIAO を USB-C で接続
4. PlatformIO サイドバー → **`xiao_esp32s3` → General → Upload**

COM ポートを指定する必要がある場合は `platformio.ini` に追記：
```ini
upload_port = COM3   ; デバイスマネージャーで確認した番号に変更
```

書き込みに失敗する場合はブートローダーモードで接続：
```
① BOOT ボタンを押したまま
② RST ボタンを押して離す
③ BOOT ボタンを離す
```

**正常起動時のシリアル出力（初回起動・WiFi 未設定時）：**
```
[boot] XIAO ESP32-S3 WiFi bridge + Web UI
[wifi] not configured — starting captive portal immediately
[ap] IP=192.168.4.1
[web] HTTP server on port 80
[boot] ready
```

---

### 機能一覧（elrs-xiao-webui）

| 機能 | 説明 |
|---|---|
| TCP MSP ブリッジ | `elrs-xiao-bridge` と同じ — 完全互換 |
| Web UI | `http://elrs-netpack.local` にアクセスして設定 |
| 言語切替 | どのページからでも JP / EN を切替可能 |
| RSSI 表示 | WiFi 電波強度をリアルタイム表示 |
| キャプティブポータル | 初回起動・WiFi 接続失敗時に AP モードへ移行、ブラウザが自動で開く |
| mDNS | ローカルネットワークから `elrs-netpack.local` でアクセス可能 |
| WiFi 設定 | SSID / パスワードを NVS（フラッシュ）に保存 |
| 電圧監視 | ADC で LiPo 電圧を計測。分圧比は Web UI で設定可能 |
| 電圧アラーム | 設定閾値を下回るとブザーと LED で警告 |
| ブザー通知 | WiFi 接続成功で 2 音、設定保存で 1 音 |
| 通知 LED | 7 色自動サイクル LED を PWM 輝度制御 |
| 高速再接続 | 切断直後に即 `WiFi.reconnect()` を実行、15 秒後に完全再接続シーケンスへ |
| 最大送信出力 | WiFi・ESP-NOW 両側とも 21 dBm |
| バックパックバージョン | RotorHazard にバージョン 10.1 を報告 |

---

### Web UI ページ

**`/wifi` — WiFi 設定**
- SSID とパスワードを入力 → 保存して接続
- パスワードを空欄のままにすると現在のパスワードを維持

**`/voltage` — 電圧モニター**
- 現在の LiPo 電圧を表示
- 分圧比：100 kΩ + 100 kΩ の等値抵抗の場合は `2.0` を設定
- アラーム閾値：デフォルト 3.5 V（1S LiPo 低電圧警告）
- 電圧アラーム / ブザー / 通知 LED の有効・無効を個別に設定可能

---

### LED 動作（GPIO9 — 7 色自動サイクル）

| 状態 | パターン |
|---|---|
| AP モード（設定待ち） | ゆっくり点滅 500 ms |
| WiFi 切断中 | 高速点滅 80 ms（再接続まで継続） |
| 電圧アラーム | 点灯 |
| STA 接続中（通常） | ハートビート 2 拍 / 2 秒（ピーク輝度約 4%） |
| LED 無効（設定オフ） | 消灯 |

### 内蔵 LED 動作（GPIO21 — 負論理）

| 状態 | パターン |
|---|---|
| WiFi 接続試行中 | 点灯 |
| AP モード | 点滅 200 ms |
| STA 接続完了 | 消灯 |

---

### NVS 保存設定一覧

すべての設定は電源オフ後も保持されます（ESP32 の Preferences ライブラリで保存）。

| キー | 型 | デフォルト | 説明 |
|---|---|---|---|
| `ssid` | String | `ELRS-Netpack-Setup` | WiFi SSID |
| `wifiPass` | String | `elrs-netpack` | WiFi パスワード |
| `configured` | Bool | false | false の場合は初回起動時 STA 接続をスキップ |
| `vbatRatio` | Float | 2.0 | 電圧分圧比 |
| `alarmV` | Float | 3.5 | アラーム閾値（V） |
| `vbatAlarmEn` | Bool | false | 電圧アラーム有効 |
| `buzzerEn` | Bool | true | ブザー有効 |
| `ledEn` | Bool | true | 通知 LED 有効 |
| `langJa` | Bool | true | 言語（true = 日本語） |

---

### AP モードのデフォルト認証情報（elrs-xiao-webui）

| | 値 |
|---|---|
| SSID | `ELRS-Netpack-Setup` |
| パスワード | `elrs-netpack` |
| IP アドレス | `192.168.4.1` |
| 設定 URL | `http://192.168.4.1/` |

---

## RotorHazard プラグイン

RotorHazard から ELRS バックパック通信を使うには、Raspberry Pi に以下のプラグインをインストールしてください：

**[yanazoo/vrxc_elrs](https://github.com/yanazoo/vrxc_elrs)**

そのリポジトリのインストール手順に従ってから、下記の接続設定を行ってください。

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

## クレジット・フォーク情報

このプロジェクトは、Waveshare ESP32-S3 Ethernet ボード向けに設計された **ELRS Netpack** コンセプトのフォークです。

- 元の ELRS バックパックプロトコル: [ExpressLRS/backpack](https://github.com/ExpressLRS/backpack)
- RotorHazard: [RotorHazard/RotorHazard](https://github.com/RotorHazard/RotorHazard)
- Seeed Studio XIAO ESP32-S3 ドキュメント: [wiki.seeedstudio.com](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
- このフォーク: [yanazoo/elrs-netpack](https://github.com/yanazoo/elrs-netpack)

デュアル MCU ESP-NOW ブリッジ構成は、ESP32-S3 の 2.4 GHz 無線を WiFi STA と ESP-NOW で共有することによるハードウェア制限を解決するために開発されました。
