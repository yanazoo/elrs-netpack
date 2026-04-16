# XIAO ESP32-S3 セットアップガイド
# XIAO ESP32-S3 Setup Guide

---

## 目次 / Contents

1. [必要なもの / Requirements](#必要なもの--requirements)
2. [ファームウェア書き込み（Windows + VS Code）](#ファームウェア書き込みwindows--vs-code)
3. [WiFi 設定](#wifi-設定--wifi-configuration)
4. [RotorHazard プラグインのインストール](#rotorhazard-プラグインのインストール)
5. [動作確認](#動作確認--verification)
6. [トラブルシューティング](#トラブルシューティング--troubleshooting)

---

## 必要なもの / Requirements

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

## ファームウェア書き込み（Windows + VS Code）

### 1. ESP-IDF 拡張機能のインストール

1. VS Code を開く
2. 拡張機能タブ（Ctrl+Shift+X）で `Espressif IDF` を検索
3. インストール → 初回セットアップウィザードで **ESP-IDF v5.4.1** を選択

### 2. このリポジトリをクローン

```powershell
git clone -b claude/wifi-espnow-bridge-jptTt `
  https://github.com/yanazoo/elrs-netpack
cd elrs-netpack
```

VS Code で開く：
```powershell
code .
```

### 3. WiFi 設定（menuconfig）

`sdkconfig.defaults` に初期値が設定されています：
- SSID: `y_air-GL`
- パスワード: `88888888`

変更したい場合：
1. `Ctrl+Shift+P` → `ESP-IDF: SDK Configuration Editor (Menuconfig)`
2. `WiFi / XIAO ESP32-S3 options` を開く
3. **WiFi SSID** と **WiFi Password** を入力
4. 外部アンテナを使う場合は `Enable external U.FL antenna` を有効化
5. `Q` で終了 → 保存

### 4. XIAO を USB 接続

1. XIAO を Windows PC に USB-C ケーブルで接続
2. デバイスマネージャーで COM ポートを確認（例: `COM3`）
3. `Ctrl+Shift+P` → `ESP-IDF: Select Port to Use` → 該当 COM ポートを選択

> **書き込みモードに入れない場合**  
> BOOT ボタンを押しながら RST ボタンを押して離す → BOOT を離す

### 5. ビルド＆書き込み

```
Ctrl+Shift+P → ESP-IDF: Build, Flash and Monitor
```

または Ctrl+Shift+B → タスク一覧から：
- `ESP-IDF: ビルド / Build` — ビルドのみ
- `ESP-IDF: フラッシュ（XIAO ESP32-S3）/ Flash` — 書き込み
- `ESP-IDF: ビルド＋フラッシュ＋モニター` — 全部まとめて実行

書き込み完了後、シリアルモニターに以下が表示されれば成功：

```
I (xxxx) main: WiFi driver started (STA mode)
I (xxxx) espnow_server: Connecting to AP: y_air-GL
I (xxxx) tcp_server: Server listening on port 8080
I (xxxx) tcp_server: WiFi STA got IP
I (xxxx) tcp_server: IP:   192.168.x.xxx
```

---

## WiFi 設定 / WiFi Configuration

| 設定項目 | デフォルト | 説明 |
|---|---|---|
| `WIFI_STA_SSID` | `y_air-GL` | 接続先 WiFi の SSID |
| `WIFI_STA_PASSWORD` | `88888888` | WiFi パスワード |
| `XIAO_EXTERNAL_ANTENNA` | 無効 | 外部アンテナ使用時に有効化（GPIO14） |
| `TCP_SERVER_PORT` | `8080` | RotorHazard との通信ポート |

WiFi 接続後、XIAO は `elrs-netpack.local`（mDNS）でアクセス可能になります。

---

## RotorHazard プラグインのインストール

**Raspberry Pi 上で実行**

```bash
# 1. リポジトリをクローン（一時フォルダへ）
git clone -b claude/wifi-espnow-bridge-jptTt --depth 1 \
  https://github.com/yanazoo/elrs-netpack /tmp/elrs-netpack

# 2. データディレクトリ内のプラグインフォルダを確認
ls ~/rh-data/plugins/

# 3. プラグインを上書きコピー
cp -r /tmp/elrs-netpack/custom_plugins/netpack_installer \
      ~/rh-data/plugins/

# 4. 一時フォルダを削除
rm -rf /tmp/elrs-netpack

# 5. RotorHazard を再起動
# サービス名の確認（どちらかが active）
systemctl is-active rotorhazard
systemctl is-active rh

# 再起動
sudo systemctl restart rotorhazard
```

### インストール確認

ブラウザで RotorHazard を開き：  
`Settings` → **「ELRS Netpack（XIAO ESP32-S3）」パネル** が表示されれば OK

---

## 動作確認 / Verification

### XIAO 側

シリアルモニターで以下を確認：

```
✓ WiFi connected
✓ Server listening on port 8080
✓ mDNS registered as elrs-netpack.local
```

### RotorHazard 側

プラグインパネルの **「デバイス接続を確認」** ボタンをクリック：

- `✓ デバイス接続確認: elrs-netpack.local:8080` → 正常
- `✗ デバイスが見つかりません` → 下記トラブルシューティングを参照

### RotorHazard の接続設定

RotorHazard の設定で ELRS バックパックを有効にし、  
ホスト: `elrs-netpack.local`（または DHCP で割り当てられた IP）  
ポート: `8080`  
を設定してください。

---

## トラブルシューティング / Troubleshooting

### XIAO が WiFi に繋がらない

- SSID・パスワードが正しいか menuconfig で確認
- `idf.py -p COMx flash` で再書き込み
- シリアルモニターのエラーメッセージを確認

### `elrs-netpack.local` が見つからない

- XIAO と Raspberry Pi が**同じ WiFi ネットワーク**に接続されているか確認
- IP アドレスで直接接続を試す（ルーターの管理画面で XIAO の IP を確認）
- mDNS が有効でない環境では IP 直指定が必要

### 書き込みモードに入れない（Windows）

1. BOOT ボタンを押し続ける
2. RST ボタンを 1 秒押して離す
3. BOOT ボタンを離す
4. `ESP-IDF: Flash` を実行

### プラグインが RotorHazard に表示されない

```bash
# ログを確認
sudo journalctl -u rotorhazard -n 50
# または
sudo journalctl -u rh -n 50
```

`ImportError` が出ている場合は依存パッケージを手動インストール：
```bash
pip3 install esptool requests
```

---

## 関連リンク / Links

- ファームウェアリポジトリ: https://github.com/yanazoo/elrs-netpack
- XIAO ESP32-S3 ドキュメント: https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/
- RotorHazard: https://github.com/RotorHazard/RotorHazard
- ESP-IDF 拡張機能: https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension
