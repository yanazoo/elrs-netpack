# ELRS Netpack — WiFi（TCP）から ELRS バックパックへ

[English](README.md) | 日本語

> [!IMPORTANT]
> このフォークは **ESP32 を 2 台使用**して **TCP → WiFi → ESP-NOW** のバックパック通信システムを構築します。元の Waveshare ESP32-S3 Ethernet ボードとは**互換性がありません**。

> [!NOTE]
> このプロジェクトは ExpressLRS organization と公式な関係はありません。

---

## 概要

ラップタイマー（**FPV C5 Lap Timer** や **RotorHazard**）が送る MSP パケット（ゴーグルの OSD・レースの合図）を、WiFi（TCP）で受け取り、**ELRS VRx バックパック**（FPV ゴーグル）へ ESP-NOW で届けるシステムです。2 台のボードで作ります。

| ボード | ファームウェア | 役割 |
|---|---|---|
| **netpack の XIAO**（Seeed XIAO ESP32-S3） | `elrs-xiao-webui/` | WiFi・TCP サーバー（ポート 8080）・Web 設定画面。受け取った MSP を UART でブリッジへ |
| **ELRSバックパック-ブリッジ**（XIAO ESP32-C3 / XIAO ESP32-C5 / ESP32 Wrover-E） | `elrs-espnow-bridge/` | ESP-NOW（2.4 GHz、ch 1）でゴーグル・送信機のバックパックと送受信 |

```
 FPV C5 Lap Timer / RotorHazard
        │  WiFi・TCP 8080
        ▼
┌──────────────────────────┐
│ netpack の XIAO（S3）       │  elrs-xiao-webui/
│ WiFi（クライアント／ホスト） │  TCP ⇄ UART、Web 設定画面、mDNS
└────────────┬─────────────┘
             │  UART 115200 baud（TX / RX / GND）
             ▼
┌──────────────────────────┐
│ ELRSバックパック-ブリッジ    │  elrs-espnow-bridge/
│ XIAO C3 / XIAO C5 / Wrover-E │
└────────────┬─────────────┘
             │  ESP-NOW（2.4 GHz、ch 1）
             ▼
   ELRS VRx バックパック（ゴーグル）・送信機のバックパック
```

**なぜ 2 台必要か？** ESP32 の 2.4 GHz 無線は 1 つだけです。WiFi（ルーターや FPV C5 Lap Timer の WiFi）と ESP-NOW（ch 1 固定）を 1 台で同時に使うと、チャンネルが合わずパケットが落ちます。役割を 2 台に分けて UART でつなぐことで、この問題を解決しています。

> FPV C5 Lap Timer では、ブリッジを **本体に UART で直結**して使うこともできます（netpack の XIAO は不要。FPV C5 Lap Timer の [docs/ELRS_UART_SETUP.md](https://github.com/yanazoo/FPV-C5-Lap-Timer/blob/main/docs/ELRS_UART_SETUP.md)）。netpack は、ブリッジを FPV C5 Lap Timer の近くに置けない場合や、RotorHazard で使う場合の構成です。

---

## 使い方の 2 つの形

**A. クライアントモード（初期値）** — netpack の XIAO が、FPV C5 Lap Timer の WiFi（ホストモードの FPV C5 Lap Timer）や会場のルーターに接続します。FPV C5 Lap Timer（または RotorHazard）は netpack を `elrs-netpack.local` で見つけて TCP 8080 につなぎます。

**B. ホストモード（v2.2.0）** — netpack の XIAO が自分の WiFi を出し、FPV C5 Lap Timer（クライアントモード）とスマホがそこに接続します。netpack を中心にした構成です。USB につないだ Android スマホの**USB テザリング**で、この WiFi からインターネットへ出ることもできます（[下記](#ホストモードと-usb-テザリング)）。

```
 スマホ（観戦・管理）──┐
 FPV C5 Lap Timer ────┼── WiFi「ELRS-NETpack」── netpack XIAO ──UART── ELRSバックパック-ブリッジ ── ゴーグル
 （クライアントモード）  ┘     （ch 6〜11）          │USB
                                                    └── Android スマホ（USB テザリング）── インターネット
```

---

## 必要なハードウェア

| 部品 | 備考 |
|---|---|
| Seeed Studio XIAO ESP32-S3 | netpack の XIAO。外部アンテナのモデル推奨 |
| ELRSバックパック-ブリッジ用のボード（どれか 1 つ） | **XIAO ESP32-C3**（小型）・**XIAO ESP32-C5**（小型、v2.2.1）・**ESP32 Wrover-E**（4 MB 以上の Flash の ESP32 開発ボードなら代替可） |
| ジャンパーワイヤー 3 本 | TX / RX / GND |
| USB ケーブル | 書き込み・給電用（データ転送対応） |
| （任意）パッシブブザー・LED・抵抗・LiPo | [追加ハードウェア](#追加ハードウェア任意)を参照 |
| （USB テザリングを使う場合）USB-C → USB-A の OTG 変換アダプター | [USB テザリングの配線・電源](#usb-テザリングの配線電源大事)を参照 |

---

## 配線（netpack の XIAO ↔ ブリッジ）

TX と RX は**交差**させます。

| netpack の XIAO（S3） | 向き | XIAO ESP32-C3 | XIAO ESP32-C5 | ESP32 Wrover-E |
|---|---|---|---|---|
| D0 / GPIO1（TX） | → | D7 / GPIO20（RX） | D7 / GPIO12（RX） | GPIO26（RX） |
| D1 / GPIO2（RX） | ← | D6 / GPIO21（TX） | D6 / GPIO11（TX） | GPIO27（TX） |
| GND | — | GND | GND | GND |

> 3V3 / 5V はボード間で直接つながないでください（つなぐのは GND だけ）。各ボードを USB などで給電するか、同じ電源から各ボードの 5V / GND へ配線します。

---

## 書き込み

### かんたんな方法：Web 書き込みページ（PlatformIO 不要）

PC の **Chrome / Edge** で **[https://yanazoo.github.io/elrs-netpack/](https://yanazoo.github.io/elrs-netpack/)** を開き、機器を USB でつないで「書き込む」を押します。

- **ELRS Netpack（XIAO ESP32-S3）** と **ELRSバックパック-ブリッジ**（ハード：XIAO C3 / XIAO C5 / Wrover-E を選択）の最新リリースを書き込めます。
- 「**ファイルを指定して書き込む**」では、手元の .bin（試験版・前の版）やリリースの zip を選んで書き込めます。アドレスはファイル名から自動で入ります（変更可）。firmware.bin には「最初のアプリから起動」（otadata）が自動で付きます。
- 「デバイスを消去する」「すべて消去」にチェックを入れなければ、WiFi などの設定は残ります。
- つながらないときは、BOOT ボタンを押したまま USB を挿し直すと書き込みモードになります。

### netpack の XIAO の更新（設定ページから）

netpack の設定ページ →「**更新**」（`/update`）：

- **ネットから更新**：「🔍 最新版を確認」→「⬇️ 〜に更新」。インターネットが必要です（クライアントモードのルーター、またはホストモードの USB テザリング）。
- **ファイルから**：firmware.bin（`elrs-netpack-xiao_esp32s3-firmware.bin`、またはリリース zip の中）を選んで「ファイルで更新」。

### リリースのファイル

[Releases](https://github.com/yanazoo/elrs-netpack/releases) に次のファイルが付きます。

| ファイル | 中身 |
|---|---|
| `elrs-netpack-<版>-xiao_esp32s3.zip` | netpack の XIAO（bootloader・partitions・firmware） |
| `elrs-espnow-bridge-<版>-<ボード>.zip` | ブリッジ（`xiao_esp32c3` / `xiao_esp32c5` / `esp32dev`） |
| `*-full.bin` | 1 つにまとめたもの（アドレス `0x0` に書き込む。esptool-js など） |
| `elrs-netpack-xiao_esp32s3-firmware.bin`・`elrs-netpack-version.txt` | netpack の「ネットから更新」用 |

> Web 書き込みページは、リリースのたびに GitHub Actions が作って GitHub Pages に置きます（リポジトリの Settings → Pages → Source を **GitHub Actions** にしておく必要があります）。

### PlatformIO で書き込む（ソースから）

1. **VS Code** と **PlatformIO IDE 拡張機能**をインストールします。
2. リポジトリをクローン：
   ```powershell
   git clone https://github.com/yanazoo/elrs-netpack
   cd elrs-netpack
   ```
3. 書き込み（`COMx` はポート番号）：

| 機器 | コマンド |
|---|---|
| netpack の XIAO（S3） | `pio run -d elrs-xiao-webui -e xiao_esp32s3 -t upload --upload-port COMx` |
| ブリッジ（XIAO C3） | `pio run -d elrs-espnow-bridge -e xiao_esp32c3 -t upload --upload-port COMx` |
| ブリッジ（XIAO C5） | `pio run -d elrs-espnow-bridge -e xiao_esp32c5 -t upload --upload-port COMx` |
| ブリッジ（Wrover-E） | `pio run -d elrs-espnow-bridge -e esp32dev -t upload --upload-port COMx` |

- netpack の XIAO と XIAO C5 のブリッジは pioarduino（Arduino-ESP32 3.3）でビルドします。初回はパッケージのダウンロードに時間がかかります。
- 書き込めないときは、BOOT ボタンを押したまま RST を押して離し、BOOT を離してから書き込みます。

**起動時のシリアル出力（USB、115200）**

netpack の XIAO（初回起動・WiFi 未設定時）：
```
[boot] XIAO ESP32-S3 WiFi bridge + Web UI  v2.3.0 (xxxxxxx)
[wifi] not configured — starting captive portal immediately
[ap] IP=192.168.4.1
[web] HTTP server on port 80
[boot] ready
```

ブリッジ：
```
[boot] ESP-NOW bridge (UART RX=GPIO20 TX=GPIO21)   ← XIAO C3（XIAO C5 は GPIO12 / GPIO11、Wrover-E は GPIO26 / GPIO27）
[wifi] channel fixed to 1
[espnow] (re)initialized
[boot] ready
```

---

## 初期設定（netpack の XIAO）

ソースの編集は不要です。設定はすべて Web 画面で行います。

1. 書き込み後、netpack の XIAO は設定用の WiFi **`ELRS-NETpack-Setup`**（パスワード `elrs-netpack`）を出します。スマホや PC で接続すると設定ページが開きます（開かないときは `http://192.168.4.1/`）。
2. 「**WiFi 設定**」で接続方式を選んで保存します。
   - **クライアント**：接続先（FPV C5 Lap Timer の WiFi やルーター）の SSID とパスワード。5 つまで登録できます。
   - **ホスト**：netpack が出す WiFi の SSID・パスワード・チャンネル。
3. つながったあとは `http://elrs-netpack.local/`（クライアントモード。またはそのアドレス）、ホストモードでは `http://192.168.4.1/` で設定ページを開けます。

ブリッジは設定不要です。ESP-NOW のチャンネルは `platformio.ini` の `ESPNOW_CHANNEL`（初期値 1）でビルド時に決まります。

---

## WiFi の設定（`/wifi`）

**状態**：いちばん上に「**FPV C5 Lap Timer**」の画面へのリンク（MSP の接続元、または同じ WiFi の `fpv-c5-lt.local` を自動で探します。見つからなければ「見つかりません」）。そのほか、今のモード・アドレス、MSP の接続状態、ファームの版。ホストモードでは、つながっている機器と USB テザリングの状態。

**クライアントモード**（v2.3.0）
- 接続先を **5 つまで**登録できます（SSID・パスワード）。パスワードは表示しません（変えないときは空欄）。SSID を消すと削除されます。
- **接続先の選び方**：**選んだもの（●）**、または**自動**（つなぐときに周りを探し、登録した中で電波が一番強いもの）。
- **IP アドレス**：自動（DHCP）または**固定**（IP アドレス・サブネットマスク・ゲートウェイ・DNS。DNS が空欄ならゲートウェイ）。固定は、登録したすべての接続先で同じアドレスを使います。FPV C5 Lap Timer の「netpack アドレス」にその IP を入れておけば、すぐにつながります。
- どの接続先にもつながらないときは、設定用の WiFi（`ELRS-NETpack-Setup`）を出し、15 秒ごとにつなぎ直します。つながっていた WiFi が切れたときは 10 秒ごとに再接続し、60 秒戻らなければ設定用の WiFi を出します。
- v2.2.x までの接続先は、1 件目にそのまま引き継がれます。

**ホストモード**：SSID・パスワード（8〜63 文字）・チャンネル（6〜11 ch）・**スマホ・PC の接続台数**（1〜9 台、初期値 5 台）（v2.3.0）。
- ブリッジとして使うための制限です。**FPV C5 Lap Timer の 1 台分はいつも空けておき**、スマホ・PC は設定した台数までにします（初期値は 5 台。FPV C5 Lap Timer のホストモードの初期値と同じ）。
- 上限を超えてつないだ機器は外し、30 秒間は入れません。FPV C5 Lap Timer は、OSD の接続（TCP）をしてきた機器として覚えておきます（一度つながったあとは、いつでも入れます）。
- netpack の WiFi で大勢が観戦するときは、台数を増やしてください（WiFi 全体で最大 10 台）。

**USB テザリング**：ON / OFF（ホストモードのみ）。

接続方式・ホストの WiFi・テザリングを変えると、保存後に再起動します。

> **WiFi のチャンネル**：ESP-NOW は**チャンネル 1** 固定です（ELRS VRx バックパックの仕様）。netpack の XIAO が接続するルーター、ホストモードの WiFi は、2.4 GHz の**チャンネル 6〜11** にしてください（1〜4 だと OSD と電波を取り合います）。

---

## ホストモードと USB テザリング

**ホストモードの設定**（WiFi 設定ページ）

1. 「接続方式」で **ホスト** を選び、ホストの SSID・パスワード（8〜63 文字）・チャンネル（6〜11 ch）を入れて保存します。netpack が再起動し、その WiFi を出します（初期値：SSID `ELRS-NETpack`、パスワード `elrs-netpack`、6 ch）。
2. 設定ページは `http://192.168.4.1/`（または `http://elrs-netpack.local/`）です。
3. FPV C5 Lap Timer は **クライアントモード** で、接続先にこの SSID とパスワードを登録します。netpack アドレスは `auto` のままで、FPV C5 Lap Timer が `elrs-netpack.local` で netpack を見つけます。
4. スマホもこの WiFi に接続し、netpack の設定ページのいちばん上の「**FPV C5 Lap Timer**」のリンクから、その画面を開けます（MSP の接続元、または同じ WiFi の `fpv-c5-lt.local` を自動で探します）。
5. スマホ・PC は **設定した台数まで**（初期値 5 台）です。FPV C5 Lap Timer の 1 台分は別にいつも空いています。設定ページに、つながっている機器のアドレスと電波の強さが出ます。
6. クライアントモードに戻すときは、設定ページで「クライアント」を選んで保存します。

**USB テザリング**（ホストモードのときだけ）

netpack の XIAO の USB-C に Android スマホをつなぎ、スマホの「USB テザリング」を ON にすると、netpack の WiFi につながった機器（FPV C5 Lap Timer ・スマホ）がスマホのモバイル通信でインターネットへ出られます（FPV C5 Lap Timer の「最新版を確認」・オンライン更新など）。

- WiFi 設定ページの「**USB テザリング**」にチェックを入れて保存します（初期値 OFF）。
- スマホ側：「設定 → ネットワークとインターネット → テザリング → **USB テザリング**」を ON（機種により名前が違います）。RNDIS（多くの Android）と CDC-NCM / ECM（新しい Pixel など）に対応しています。**iPhone は非対応**です。
- 設定ページの「USB テザリング」に状態が出ます（スマホ待ち → テザリングを ON にしてください → アドレス待ち → **接続中**）。
- netpack は機器からの DNS をスマホの DNS へ中継し、通信をスマホ経由で外へ出します（NAPT）。
- テザリングに使うスマホ自身は、netpack の WiFi に接続しないでください（スマホの外への道が netpack に戻ってしまいます）。

### USB テザリングの配線・電源（大事）

- XIAO が USB の**ホスト**になります。**USB-C（オス）→ USB-A（メス）の OTG 変換アダプター**を XIAO に挿し、スマホの充電ケーブル（USB-A → スマホ）でつなぎます。USB-C ↔ USB-C のケーブルだと、スマホの方がホストになってしまい動きません。
- スマホが USB 接続を認識するには、USB の **5V（VBUS）**が出ている必要があります。XIAO の 5V ピンは USB の VBUS とつながっているため、netpack を **5V ピンから給電**（ダイオード経由、5V の BEC や USB 電源から）していると VBUS に 5V が出ます。LiPo を BAT パッドにつないでいるだけの場合は 5V が出ないため、スマホが認識しません。
- スマホは VBUS から充電しようとして電流（最大 500 mA 程度）を流します。電源に余裕を持たせてください。
- ON の間は USB が XIAO のシリアル・書き込みに使えません。**ログは D6（GPIO43、UART0 TX、115200）**に出ます。書き込みは「**更新**」ページから、または BOOT ボタンを押しながら USB を PC につないで書き込みます。

---

## FPV C5 Lap Timer・RotorHazard との接続

**FPV C5 Lap Timer**（設定タブ → ELRS OSD）
- 接続方式：**「WiFi（elrs-netpack XIAO, TCP）」**
- netpack アドレス：`auto`（ホストモードの FPV C5 Lap Timer は自分の WiFi につながった機器から、クライアントモードの FPV C5 Lap Timer は `elrs-netpack.local` で探します）。netpack の IP を固定した場合は、その IP を直接入れることもできます。
- 設定画面に「ELRS netpack 接続中」と出れば OK です。

**RotorHazard**（Raspberry Pi）
- プラグイン **[yanazoo/vrxc_elrs](https://github.com/yanazoo/vrxc_elrs)** をインストールし、次のように設定します。
```
Settings → ELRS バックパック 一般設定
  バックパック接続タイプ → SOCKET
  ELRS Netpack アドレス  → elrs-netpack.local（または netpack の IP アドレス）
  ポート                 → 8080
```

---

## 追加ハードウェア（任意）

| 部品 | 備考 |
|---|---|
| パッシブブザー | GPIO4(+) / GPIO6(−) に接続 |
| 7 色自動カラーサイクル LED | GPIO9（D10）+ **100 Ω 抵抗**を直列に接続 |
| 100 kΩ 抵抗 × 2 | LiPo 電圧監視用の分圧回路 |
| LiPo バッテリー | 1S 推奨（netpack の XIAO とブリッジの共有電源） |

| XIAO GPIO | ピン名 | 接続先 |
|---|---|---|
| GPIO3 | D2 / A0 | 分圧抵抗の中点（LiPo+ → R1 100kΩ → GPIO3 → R2 100kΩ → GND） |
| GPIO4 | D3 | ブザー プラス極（+） |
| GPIO6 | D5 | ブザー マイナス極（−） |
| GPIO9 | D10 | LED アノード（+ 100 Ω → GND） |
| GPIO21 | — | 内蔵 LED（負論理、基板上） |
| GPIO43 | D6 | ログ出力（UART0 TX、USB テザリング ON の間） |

> **重要:** 通知 LED には必ず **100 Ω** の直列抵抗を入れてください（GPIO9 保護のため）。

---

## 機能一覧（netpack の XIAO：elrs-xiao-webui）

| 機能 | 説明 |
|---|---|
| TCP MSP ブリッジ | TCP 8080 ⇄ UART。新しい接続が来たら古い接続と入れ替え（FPV C5 Lap Timer の再起動・つなぎ直し） |
| Web 設定画面 | WiFi・電圧・更新の各ページ。日本語 / 英語の切り替え |
| WiFi（クライアント） | 接続先 5 件（選択 / 自動）、IP 固定、つながらないときは設定用の WiFi（キャプティブポータル） |
| WiFi（ホスト） | netpack の WiFi（SSID・パスワード・ch 6〜11）、FPV C5 Lap Timer ＋ スマホ・PC（1〜9 台、初期値 5 台）、DNS の中継 |
| USB テザリング | ホストモードで Android スマホからインターネットへ（RNDIS / NCM / ECM、NAPT） |
| mDNS | `elrs-netpack.local`（Web と TCP の両方） |
| ファーム更新 | ネットから（GitHub のリリース）／ファイルから |
| 電圧監視・アラーム | LiPo 電圧を表示。しきい値を下回るとブザーと LED |
| ブザー・LED 通知 | WiFi 接続で 2 音、設定保存で 1 音、FPV C5 Lap Timer（MSP）がつながると 1 音 |
| 接続切れの通知 | **FPV C5 Lap Timer（MSP の TCP）との接続が切れたら、ブザーの長音 5 回と LED の中速点滅**（つながり直すまで点滅）。ホストモードでは、FPV C5 Lap Timer が netpack の WiFi から抜けたらすぐ、それ以外は約 11 秒で気づきます（TCP キープアライブ）。netpack 自身の WiFi が切れたときは別の通知（ブザー 5 秒・LED 高速点滅） |
| 最大送信出力 | WiFi・ESP-NOW とも 21 dBm |
| バックパックバージョン | 接続元にバージョン 10.1 を返す |

### ELRSバックパック-ブリッジの送信方式（elrs-espnow-bridge）

UDP のような**コネクションレス送信**です。ブリッジはパイロットのバインドフレーズの UID を自分の MAC にし（ゴーグルのバックパックは、その UID から来た OSD パケットだけを表示します）、ESP-NOW の**ブロードキャスト**で送ります。受信確認（ACK）は待たず、再送もしません。

- 1 人分の画面更新（次の `SET_SEND_UID` まで、または 20 ミリ秒送信が途切れるまで）を、続けて **3 回**送ります（`SEND_REPEATS`）
- 同じパケットのコピー同士は**最低 3 ミリ秒**あけます。各パイロットの ELRS 2.4GHz 送信機は数ミリ秒ごとに周波数を変え、その約 3 割がチャンネル 1 に入るため、少し後のコピーは別の周波数のタイミングに当たり、届きやすくなります
- 電源の入っていないゴーグルがいくつあっても、ほかのパイロットの表示は遅れません。同じバインドフレーズのゴーグルが 2 台あれば、2 台とも同じ表示になります
- 長めの電波の途切れは、送信元（RotorHazard の vrxc_elrs、ラップタイマー）の時間差の再送で補います
- **受信フィルター**（独自 MSP `0x0F01`、ペイロード＝6 バイトの UID、空で解除）：接続元（FPV C5 Lap Timer）がレースディレクターの UID を指定すると、ブリッジはその UID **から**届いたフレーム（ディレクターの送信機のスイッチ）だけを接続元に渡します。ほかのブリッジのブロードキャスト OSD や、パイロット自身の DVR スイッチは渡しません。ブリッジは `0x0F01`（ペイロード `1`）で応答します。これを送らない接続元（RotorHazard の vrxc_elrs）には、これまでどおりすべて渡します。フィルターを指定している間は、受信モニター（promiscuous）でもディレクターのスイッチを聞き取ります（v2.12）。500 ms 以内に同じフレームを 2 回以上聞いたときは 1 回だけ渡します
- **リンクの見張り**（独自 MSP `0x0F03`、ペイロード `1` で開始・`0` で終了、`0x0F03` で応答）：接続元（FPV C5 Lap Timer）が頼むと、ブリッジはチャンネル 1 の ESP-NOW をすべて受信し、パイロットの送信機のバックパック（ELRS 3.4 以降、*Backpack → Telemetry = ESPNOW*）が送るリンク情報を読みます。UID ごとに `0x0F04`（ペイロード：UID 6 バイト、上りの LQ、経過時間 0.1 秒単位）で、リンクした・切れたときはすぐ、それ以外は 1 秒ごとに知らせます。接続元からの依頼が 20 秒途切れると自動で止まります
- **VTX の知らせ**（独自 MSP `0x0F05`、リンクの見張り中）：パイロットの送信機が VTX の設定（Lua の *VTX Administrator*）を送ると、`0x0F05`（ペイロード：UID 6 バイト、VTX 設定の先頭 2 バイト＝バンド・チャンネルの番号 `(バンド-1)*8+チャンネル`、または MHz の周波数）で知らせます
- XIAO C5 のブリッジは、2.4 GHz だけを使います（5 GHz は使いません）
- ESP-NOW で受け取ったフレームはいったんキューに入れ、`loop()` から UART に書き出します（WiFi のタスクで UART を待たない）
- パケットごとのログは、`-DBRIDGE_VERBOSE` を付けてビルドしたときだけ出ます

---

## Web UI ページ

**`/wifi` — WiFi 設定**：[上記](#wifi-の設定wifi)を参照。

**`/update` — ファーム更新**
- 今のファームの版（リリース名とビルド）
- ネットから更新：「🔍 最新版を確認」→ 新しい版があれば「⬇️ 〜に更新」で書き込み、再起動します
- ファイルから：`elrs-xiao-webui` の firmware.bin を選んで書き込み、再起動します。設定は残ります

**`/voltage` — 電圧モニター**
- 現在の LiPo 電圧を表示
- 分圧比：100 kΩ + 100 kΩ の等値抵抗の場合は `2.0` を設定
- アラーム閾値：初期値 3.5 V（1S LiPo 低電圧警告）
- 電圧アラーム / ブザー / 通知 LED の有効・無効を個別に設定可能

---

## LED 動作

**通知 LED（GPIO9 — 7 色自動サイクル）**

| 状態 | パターン |
|---|---|
| 設定用の WiFi（設定待ち） | ゆっくり点滅 500 ms |
| WiFi 切断中（クライアントモード） | 高速点滅 80 ms（再接続まで継続） |
| FPV C5 Lap Timer（MSP）との接続が切れた（一度つながったあと） | 中速点滅 300 ms（つながり直すまで） |
| 電圧アラーム | 点灯 |
| 通常（クライアントで接続中・ホストモード） | ハートビート 2 拍 / 2 秒（ピーク輝度約 4%） |
| LED 無効（設定オフ） | 消灯 |

**内蔵 LED（GPIO21 — 負論理）**

| 状態 | パターン |
|---|---|
| WiFi 接続試行中 | 点灯 |
| 設定用の WiFi | 点滅 200 ms |
| 接続完了・ホストモード | 消灯 |

---

## NVS 保存設定一覧

すべての設定は電源オフ後も保持されます（ESP32 の Preferences、名前空間 `elrs`）。Web 書き込みページで「消去」にチェックを入れたときだけ消えます。

| キー | 型 | 初期値 | 説明 |
|---|---|---|---|
| `netMode` | UChar | 0 | 接続方式（0 = クライアント、1 = ホスト） |
| `ss0`〜`ss4` / `sp0`〜`sp4` | String | （空） | クライアントの接続先 1〜5 の SSID / パスワード（v2.3.0） |
| `nets` | Bool | false | 接続先 5 件の形で保存済み（false なら `ssid` / `wifiPass` を 1 件目に引き継ぎ） |
| `staSel` | UChar | 0 | 選んだ接続先（0〜4） |
| `staAuto` | Bool | false | 自動（電波が一番強い接続先） |
| `ipFix` | Bool | false | IP アドレスを固定 |
| `ip` / `mask` / `gw` / `dns` | UInt | 0 / 255.255.255.0 / 0 / 0 | 固定する IP アドレス・サブネットマスク・ゲートウェイ・DNS |
| `configured` | Bool | false | 接続先が 1 つ以上ある |
| `ssid` / `wifiPass` | String | — | v2.2.x までの接続先（1 件目に引き継ぎ） |
| `hSsid` | String | `ELRS-NETpack` | ホストモードの SSID |
| `hPass` | String | `elrs-netpack` | ホストモードのパスワード |
| `hCh` | UChar | 6 | ホストモードのチャンネル（6〜11） |
| `hMax` | UChar | 5 | ホストモードのスマホ・PC の接続台数（1〜9、FPV C5 Lap Timer とは別） |
| `lapMac` | Bytes | — | FPV C5 Lap Timer の MAC アドレス（OSD の接続から覚える。台数の制限で外さない） |
| `tether` | Bool | false | USB テザリング（ホストモードのみ） |
| `vbatRatio` | Float | 2.0 | 電圧分圧比 |
| `alarmV` | Float | 3.5 | アラーム閾値（V） |
| `vbatAlarmEn` | Bool | false | 電圧アラーム有効 |
| `buzzerEn` | Bool | true | ブザー有効 |
| `ledEn` | Bool | true | 通知 LED 有効 |
| `langJa` | Bool | true | 言語（true = 日本語） |

---

## 初期の WiFi（公開されている初期値です。使う場合は変更をおすすめします）

| | 設定用の WiFi（クライアントモードで未接続のとき） | ホストモードの WiFi |
|---|---|---|
| SSID | `ELRS-NETpack-Setup` | `ELRS-NETpack`（変更可） |
| パスワード | `elrs-netpack` | `elrs-netpack`（変更可） |
| IP アドレス | `192.168.4.1` | `192.168.4.1` |
| 設定 URL | `http://192.168.4.1/` | `http://192.168.4.1/` |

---

## トラブルシューティング

| 症状 | 対処 |
|---|---|
| netpack が WiFi につながらない | 設定用の WiFi（`ELRS-NETpack-Setup`）に接続して、SSID・パスワードを確認。ルーターは 2.4 GHz を出しているか（5 GHz だけでは不可） |
| `elrs-netpack.local` が見つからない | 同じネットワークか確認。netpack の IP を固定して、その IP を直接指定 |
| FPV C5 Lap Timer に「ELRS netpack 接続中」と出ない | FPV C5 Lap Timer の接続方式が「WiFi（elrs-netpack XIAO, TCP）」か、netpack アドレス（`auto` か IP）、netpack の設定ページの「MSP」の行 |
| OSD がゴーグルに表示されない | ブリッジの配線（TX / RX の交差・GND）、ブリッジの起動ログ、バインドフレーズがゴーグル側と一致しているか、ESP-NOW チャンネル（ch 1） |
| 表示が抜ける・遅れる | ルーター／ホストモードの WiFi を ch 6〜11 に。ブリッジのアンテナ（XIAO C3 / C5 は付属アンテナを接続） |
| USB テザリングが「スマホ待ち」のまま | OTG 変換アダプターを XIAO 側に挿しているか、5V ピンから給電しているか（VBUS に 5V が必要） |
| USB テザリングが「テザリングを ON にしてください」のまま | スマホの USB テザリングを ON。iPhone は非対応 |
| テザリング中に USB で書き込めない | 更新ページから書き込むか、BOOT ボタンを押しながら USB を PC につなぐ |
| PlatformIO のビルドが失敗する | 初回はパッケージのダウンロードに時間がかかります。`pio pkg update` を試す。Windows で `No module named intelhex` は `C:\Users\<ユーザー名>\.platformio\penv\Scripts\python.exe -m pip install intelhex` |

---

## クレジット・フォーク情報

このプロジェクトは、Waveshare ESP32-S3 Ethernet ボード向けに設計された **ELRS Netpack** コンセプトのフォークです。

- 元の ELRS バックパックプロトコル: [ExpressLRS/backpack](https://github.com/ExpressLRS/backpack)
- RotorHazard: [RotorHazard/RotorHazard](https://github.com/RotorHazard/RotorHazard)
- FPV C5 Lap Timer: [yanazoo/FPV-C5-Lap-Timer](https://github.com/yanazoo/FPV-C5-Lap-Timer)
- Seeed Studio XIAO ESP32-S3 ドキュメント: [wiki.seeedstudio.com](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
- このフォーク: [yanazoo/elrs-netpack](https://github.com/yanazoo/elrs-netpack)

2 台構成（netpack の XIAO ＋ ELRSバックパック-ブリッジ）は、ESP32 の 2.4 GHz 無線を WiFi と ESP-NOW で共有することによる制限を解決するために開発されました。
