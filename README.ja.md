# ELRS Netpack — TCP → WiFi → ELRSバックパック-ブリッジ

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
│  XIAO ESP32-S3      │  elrs-xiao-webui/
│  WiFi STA + Web UI  │  TCP を受信して UART に転送
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

XIAO ファームウェア（`elrs-xiao-webui/`）は**ソースの編集が不要**です。WiFi の認証情報は初回起動時のキャプティブポータルから入力します（下記「[XIAO ESP32-S3 の書き込み](#xiao-esp32-s3-の書き込みelrs-xiao-webui)」「[AP モードのデフォルト認証情報](#ap-モードのデフォルト認証情報elrs-xiao-webui)」を参照）。

ESP-NOW 側（`elrs-espnow-bridge/`）も設定不要です。チャンネルは `platformio.ini` の `ESPNOW_CHANNEL`（デフォルト 1）でコンパイル時に固定されます。

---

## ESP32 Wrover-E の書き込み（`elrs-espnow-bridge/`）

1. VS Code でフォルダを開く：
   **ファイル → フォルダーを開く → `elrs-espnow-bridge/`**
2. ESP32 を USB で接続
3. PlatformIO サイドバー → **`esp32dev` → General → Upload**

**正常起動時のシリアル出力：**
```
[boot] ESP-NOW bridge (UART RX=GPIO26 TX=GPIO27)
[wifi] channel fixed to 1
[espnow] (re)initialized
[boot] ready
```

### XIAO ESP32-C3 で代用する場合

ESP32 Wrover-E の代わりに Seeed Studio XIAO ESP32-C3 でも動作します（PlatformIO 環境 **`xiao_esp32c3`**）。

| XIAO ESP32-C3 | 接続先（XIAO ESP32-S3 などのホスト） |
|---|---|
| D7 / GPIO20（RX） | ホストの TX（netpack の XIAO なら D0 / GPIO1） |
| D6 / GPIO21（TX） | ホストの RX（netpack の XIAO なら D1 / GPIO2） |
| GND | GND |

```
pio run -d elrs-espnow-bridge -e xiao_esp32c3 -t upload
```

起動ログ（USB シリアル）は `[boot] ESP-NOW bridge (UART RX=GPIO20 TX=GPIO21)` になります。

### XIAO ESP32-C5 で代用する場合（v2.2.1）

Seeed Studio XIAO ESP32-C5 でも動作します（PlatformIO 環境 **`xiao_esp32c5`**、pioarduino）。配線は XIAO C3 と同じ D7 / D6 です。C5 は 5 GHz も使えるボードですが、ブリッジは 2.4 GHz のチャンネル 1 だけを使います。

| XIAO ESP32-C5 | 接続先（XIAO ESP32-S3 などのホスト） |
|---|---|
| D7 / GPIO12（RX） | ホストの TX（netpack の XIAO なら D0 / GPIO1） |
| D6 / GPIO11（TX） | ホストの RX（netpack の XIAO なら D1 / GPIO2） |
| GND | GND |

```
pio run -d elrs-espnow-bridge -e xiao_esp32c5 -t upload
```

起動ログ（USB シリアル）は `[boot] ESP-NOW bridge (UART RX=GPIO12 TX=GPIO11)` になります。

---

## elrs-xiao-webui — XIAO ファームウェア（Web UI 版）

`elrs-xiao-webui/` が XIAO 用ファームウェアです。完全な TCP MSP ブリッジ機能に加えて、**Web UI・キャプティブポータル・バッテリー電圧監視・ブザー・LED 通知**を備えています。

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

### 書き込み方法（v2.2.0 から）

| 方法 | 対象 | 使い方 |
|---|---|---|
| **Web 書き込みページ**（最新版） | netpack XIAO・ELRSバックパック-ブリッジ（XIAO C3 / XIAO C5 / Wrover-E を選択） | PC の Chrome / Edge で [https://yanazoo.github.io/elrs-netpack/](https://yanazoo.github.io/elrs-netpack/) を開き、機器の「書き込む」 |
| **Web 書き込みページ**（ファイルを指定） | 同上 | 同じページの「ファイルを指定して書き込む」に .bin（試験版・前の版）またはリリースの zip をドロップ。アドレスはファイル名から自動で入ります（変更可）。firmware.bin には「最初のアプリから起動」（otadata）が自動で付きます |
| **ネットから更新**（ボタン） | netpack XIAO | netpack の設定ページ →「更新」→「🔍 最新版を確認」→「⬇️ 〜に更新」。インターネットが必要です（クライアントモードのルーター、またはホストモードの USB テザリング） |
| **ファイルから** | netpack XIAO | netpack の設定ページ →「更新」→ firmware.bin（`elrs-netpack-xiao_esp32s3-firmware.bin` かリリース zip の中）を選んで「ファイルで更新」 |
| **PlatformIO**（GitHub のソースから） | すべて | 下記 |

- リリースには、zip（bootloader・partitions・firmware）、1 つにまとめた `*-full.bin`（アドレス `0x0` に書き込む。esptool-js などで）、ネット更新用の `elrs-netpack-xiao_esp32s3-firmware.bin` と `elrs-netpack-version.txt` が付きます。
- 設定（WiFi など）は、どの方法でも残ります（「消去」にチェックを入れたときを除く）。
- Web 書き込みページは、リリースのたびに GitHub Actions が作って GitHub Pages に置きます（リポジトリの Settings → Pages → Source を **GitHub Actions** にしておく必要があります）。

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
| TCP MSP ブリッジ | ESP-NOW ボードへの完全な TCP ⇄ UART MSP ブリッジ |
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
| 高速再接続 | ノンブロッキング自動再接続。10 秒ごとに再試行し、60 秒で復帰しなければキャプティブポータルへ |
| ホストモード（v2.2.0） | netpack が自分の WiFi を出し、タイマーやスマホがそこに接続（下記） |
| USB テザリング（v2.2.0） | ホストモードで、USB につないだ Android スマホからインターネットへ（設定で ON / OFF） |
| ファーム更新ページ（v2.2.0） | ブラウザーから firmware.bin を書き込み（`/update`） |
| 最大送信出力 | WiFi・ESP-NOW 両側とも 21 dBm |
| バックパックバージョン | RotorHazard にバージョン 10.1 を報告 |

### ELRSバックパック-ブリッジの送信方式（elrs-espnow-bridge）

UDP のような**コネクションレス送信**です。ブリッジはパイロットのバインドフレーズの UID を自分の MAC にし（ゴーグルのバックパックは、その UID から来た OSD パケットだけを表示します）、ESP-NOW の**ブロードキャスト**で送ります。受信確認（ACK）は待たず、再送もしません。

- 1 人分の画面更新（次の `SET_SEND_UID` まで、または 20 ミリ秒送信が途切れるまで）を、続けて **3 回**送ります（`SEND_REPEATS`）
- 同じパケットのコピー同士は**最低 3 ミリ秒**あけます。各パイロットの ELRS 2.4GHz 送信機は数ミリ秒ごとに周波数を変え、その約 3 割がチャンネル 1 に入るため、少し後のコピーは別の周波数のタイミングに当たり、届きやすくなります
- 電源の入っていないゴーグルがいくつあっても、ほかのパイロットの表示は遅れません。同じバインドフレーズのゴーグルが 2 台あれば、2 台とも同じ表示になります
- 長めの電波の途切れは、送信元（RotorHazard の vrxc_elrs、ラップタイマー）の時間差の再送で補います
- **受信フィルター**（独自 MSP `0x0F01`、ペイロード＝6 バイトの UID、空で解除）：接続元（FPV C5 Lap Timer）がレースディレクターの UID を指定すると、ブリッジはその UID **から**届いたフレーム（ディレクターの送信機のスイッチ）だけを接続元に渡します。ほかのブリッジのブロードキャスト OSD や、パイロット自身の DVR スイッチは渡しません。ブリッジは `0x0F01`（ペイロード `1`）で応答します。これを送らない接続元（RotorHazard の vrxc_elrs）には、これまでどおりすべて渡します。フィルターを指定している間は、受信モニター（promiscuous）でもディレクターのスイッチを聞き取ります（v2.12）。送信機はスイッチを切り替えたとき 1 回だけ送り、OSD の送信中はブリッジの宛先がパイロットの UID になっているため、通常の ESP-NOW 受信だけでは取りこぼしていました。500 ms 以内に同じフレームを 2 回以上聞いたときは 1 回だけ渡します
- **リンクの見張り**（独自 MSP `0x0F03`、ペイロード `1` で開始・`0` で終了、`0x0F03` で応答）：接続元（FPV C5 Lap Timer）が頼むと、ブリッジはチャンネル 1 の ESP-NOW をすべて受信し、パイロットの送信機のバックパック（ELRS 3.4 以降、*Backpack → Telemetry = ESPNOW*）が送るリンク情報を読みます。UID ごとに `0x0F04`（ペイロード：UID 6 バイト、上りの LQ、経過時間 0.1 秒単位）で、リンクした・切れたときはすぐ、それ以外は 1 秒ごとに知らせます。接続元からの依頼が 20 秒途切れると自動で止まります。依頼しない接続元（RotorHazard の vrxc_elrs）には何も増えません。
- **VTX の知らせ**（独自 MSP `0x0F05`、リンクの見張り中）：パイロットの送信機が VTX の設定（Lua の *VTX Administrator*。リンクしたときに送られ、バックパックが同じ UID のゴーグルへ `MSP_SET_VTX_CONFIG` として渡します）を送ると、`0x0F05`（ペイロード：UID 6 バイト、VTX 設定の先頭 2 バイト＝バンド・チャンネルの番号 `(バンド-1)*8+チャンネル`、または MHz の周波数）で知らせます。
- ESP-NOW で受け取ったフレームはいったんキューに入れ、`loop()` から UART に書き出します（WiFi のタスクで UART を待たない）
- パケットごとのログは、`-DBRIDGE_VERBOSE` を付けてビルドしたときだけ出ます

> **WiFi のチャンネル**：ESP-NOW は**チャンネル 1** 固定です（ELRS VRx バックパックの仕様）。netpack の XIAO が接続するルーターは、2.4GHz の**チャンネル 6〜11** にしてください（1〜4 だと OSD と電波を取り合います）。

---

### ホストモードと USB テザリング（v2.2.0）

netpack を中心にした構成です。netpack の XIAO が WiFi を出し（ホストモード）、タイマーとスマホがその WiFi に接続します。タイマーからの MSP は今まで通り UART → ELRSバックパック-ブリッジ → ゴーグルのバックパックへ届きます（ブリッジの ESP-NOW は別のボードで、チャンネル 1 のまま）。

```
 スマホ（観戦・管理）──┐
 FPV C5 Lap Timer ────┼── WiFi「ELRS-Netpack」── netpack XIAO ──UART── ELRSバックパック-ブリッジ ── ゴーグル
 （クライアントモード）  ┘     （ch 6〜11）          │USB
                                                    └── Android スマホ（USB テザリング）── インターネット
```

**ホストモードの設定**（WiFi 設定ページ）

1. 「接続方式」で **ホスト** を選び、ホストの SSID・パスワード（8〜63 文字）・チャンネル（6〜11 ch）を入れて保存します。netpack が再起動し、その WiFi を出します（初期値：SSID `ELRS-Netpack`、パスワード `elrs-netpack`、6 ch）。
2. 設定ページは `http://192.168.4.1/`（または `http://elrs-netpack.local/`）です。
3. タイマーは **クライアントモード** で、接続先にこの SSID とパスワードを登録します。netpack アドレスは `auto` のままで、タイマーが `elrs-netpack.local` で netpack を見つけます。
4. スマホもこの WiFi に接続し、netpack の設定ページの「**タイマーを開く**」（MSP の接続元＝タイマーのアドレス）からタイマーの画面を開けます。
5. 接続はタイマーを含めて **10 台まで** です。設定ページに、つながっている機器のアドレスと電波の強さが出ます。
6. クライアントモードに戻すときは、設定ページで「クライアント」を選んで保存します。

**USB テザリング**（ホストモードのときだけ）

netpack の XIAO の USB-C に Android スマホをつなぎ、スマホの「USB テザリング」を ON にすると、netpack の WiFi につながった機器（タイマー・スマホ）がスマホのモバイル通信でインターネットへ出られます（タイマーの「最新版を確認」・オンライン更新など）。

- WiFi 設定ページの「**USB テザリング**」にチェックを入れて保存します（設定で ON / OFF。初期値 OFF）。
- スマホ側：「設定 → ネットワークとインターネット → テザリング → **USB テザリング**」を ON（機種により名前が違います）。RNDIS（多くの Android）と CDC-NCM / ECM（新しい Pixel など）に対応しています。**iPhone は非対応**です。
- 設定ページの「USB テザリング」に状態が出ます（スマホ待ち → テザリングを ON にしてください → アドレス待ち → **接続中**）。
- netpack は機器からの DNS をスマホの DNS へ中継し、通信をスマホ経由で外へ出します（NAPT）。
- テザリングに使うスマホ自身は、netpack の WiFi に接続しないでください（スマホの外への道が netpack に戻ってしまいます）。

**USB テザリングの配線・電源（大事）**

- XIAO が USB の**ホスト**になります。スマホとは **USB-C（オス）→ USB-A（メス）の OTG 変換アダプター**を XIAO に挿し、スマホの充電ケーブル（USB-A → スマホ）でつなぎます。USB-C ↔ USB-C のケーブルだと、スマホの方がホストになってしまい動きません。
- スマホが USB 接続を認識するには、USB の **5V（VBUS）**が出ている必要があります。XIAO の 5V ピンは USB の VBUS とつながっているため、netpack を **5V ピンから給電**（ダイオード経由、5V の BEC や USB 電源から）していると VBUS に 5V が出ます。LiPo を BAT パッドにつないでいるだけの場合は 5V が出ないため、スマホが認識しません。
- スマホは VBUS から充電しようとして電流（最大 500 mA 程度）を流します。電源に余裕を持たせてください。
- ON の間は USB が XIAO のシリアル・書き込みに使えません。**ログは D6（GPIO43、UART0 TX、115200）**に出ます。書き込みは「**更新**」ページ（`/update`）から firmware.bin を選ぶか、BOOT ボタンを押しながら USB を PC につないで書き込みます。

---

### Web UI ページ

**`/wifi` — WiFi 設定**
- 状態：今のモード・アドレス、MSP の接続元（「タイマーを開く」のリンク）、ホストモードではつながっている機器と USB テザリングの状態
- 接続方式：クライアント（WiFi に接続）／ホスト（netpack が WiFi を出す）
- クライアント：接続先を **5 つまで**登録（SSID・パスワード。パスワードは表示せず、空欄なら今のまま。SSID を消すと削除）（v2.3.0）
  - 接続先の選び方：**選んだもの（●）**、または**自動**（登録した中で電波が一番強いもの。つなぐときに周りを探します）
  - **IP アドレス**：自動（DHCP）または**固定**（IP アドレス・サブネットマスク・ゲートウェイ・DNS。DNS が空欄ならゲートウェイ）。固定はすべての接続先で同じアドレスを使います
  - v2.2.x までの接続先は、1 件目にそのまま引き継がれます
- ホスト：SSID・パスワード（8〜63 文字、空欄なら今のまま）・チャンネル（6〜11 ch）
- USB テザリング：ON / OFF（ホストモードのみ）
- 接続方式・ホストの WiFi・テザリングを変えると、保存後に再起動します

**`/update` — ファーム更新**（v2.2.0）
- 今のファームの版（リリース名とビルド）
- ネットから更新：「🔍 最新版を確認」→ 新しい版があれば「⬇️ 〜に更新」で書き込み、再起動します
- ファイルから：`elrs-xiao-webui` の firmware.bin を選んで書き込み、再起動します。設定は残ります

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
| STA 接続中・ホストモード（通常） | ハートビート 2 拍 / 2 秒（ピーク輝度約 4%） |
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
| `ss0`〜`ss4` / `sp0`〜`sp4` | String | （空） | クライアントの接続先 1〜5 の SSID / パスワード（v2.3.0） |
| `nets` | Bool | false | 接続先 5 件の形で保存済み（false なら `ssid` / `wifiPass` を 1 件目に引き継ぎ） |
| `staSel` | UChar | 0 | 選んだ接続先（0〜4） |
| `staAuto` | Bool | false | 自動（電波が一番強い接続先） |
| `ipFix` | Bool | false | IP アドレスを固定 |
| `ip` / `mask` / `gw` / `dns` | UInt | 0 / 255.255.255.0 / 0 / 0 | 固定する IP アドレス・サブネットマスク・ゲートウェイ・DNS |
| `ssid` | String | `ELRS-Netpack-Setup` | WiFi SSID（v2.2.x まで） |
| `wifiPass` | String | `elrs-netpack` | WiFi パスワード（v2.2.x まで） |
| `configured` | Bool | false | false の場合は初回起動時 STA 接続をスキップ |
| `vbatRatio` | Float | 2.0 | 電圧分圧比 |
| `alarmV` | Float | 3.5 | アラーム閾値（V） |
| `vbatAlarmEn` | Bool | false | 電圧アラーム有効 |
| `buzzerEn` | Bool | true | ブザー有効 |
| `ledEn` | Bool | true | 通知 LED 有効 |
| `langJa` | Bool | true | 言語（true = 日本語） |
| `netMode` | UChar | 0 | 接続方式（0 = クライアント、1 = ホスト） |
| `hSsid` | String | `ELRS-Netpack` | ホストモードの SSID |
| `hPass` | String | `elrs-netpack` | ホストモードのパスワード |
| `hCh` | UChar | 6 | ホストモードのチャンネル（6〜11） |
| `tether` | Bool | false | USB テザリング（ホストモードのみ） |

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
| WiFi AUTH_EXPIRE が繰り返される | パスワード誤り → キャプティブポータル（AP モード）から再入力 |
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

デュアル MCU ELRSバックパック-ブリッジ構成は、ESP32-S3 の 2.4 GHz 無線を WiFi STA と ESP-NOW で共有することによるハードウェア制限を解決するために開発されました。
