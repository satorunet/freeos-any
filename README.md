# FreeOS Any

開発の記録 (版ごとの変更点とスクリーンショット): [docs/HISTORY.md](docs/HISTORY.md)

名前の「Any」は「どこでも動く」の意味です(現在の対応範囲は下の表のとおり)。

PCの電源を入れてUSBメモリから直接起動する、小さな自作OSです(x86-64 と ARM64 / UEFI、C言語)。
Windows も Linux も使いません。

## 対応状況
| パソコン | 使い方 |
|---|---|
| Windows PC (Intel / AMD) | USB から直接起動 |
| Windows PC (ARM、Snapdragon など) | USB から直接起動(実機未確認) |
| Intel Mac | USB から直接起動(実機未確認) |
| Apple シリコン Mac (M1 以降) | **仮想マシンのみ**(下の Mac 版や UTM)。Mac の仕組み上、直接起動はできない |

## macOS / Windows の上で動かす
いつもの OS を起動したまま、窓の中で動かします。仮想化ソフト [QEMU](https://www.qemu.org/) を使います(同梱していません)。

### macOS (優先して開発中)
1. `dist/FreeOSAny-1.3.dmg` を開き、`FreeOS Any` を `Applications` にドラッグして起動します。
2. Apple の署名がないため、止められたら「システム設定」→「プライバシーとセキュリティ」→「このまま開く」。
3. QEMU がなければ画面の案内でインストールします(先に [Homebrew](https://brew.sh/ja/) が必要)。

- マウスとキーボードで操作します。キー入力を Mac に戻すのは control+option+G。
- ファイルは `~/Library/Application Support/FreeOS Any/freeos-any.img` に保存されます。
  アプリを新しい版に入れ替えると、ディスクの控えを作ってから OS 本体だけを更新します。
- ファイル転送は `http://localhost:8080/番号/`(ポートはウィンドウのタイトル)。同じ Mac からだけ接続できます。
- アプリのスクリプトは Linux 上で macOS のコマンドを模擬して試したもので、**macOS の実機では未確認**です。
- dmg の中身は ISO 9660 形式です(`make dmg`、`tools/mkdmg.py`)。

### Windows (旧版 1.2、後回し)
`dist/freeos-any-1.2-windows.zip` を展開して `FreeOSAny.bat` を実行します。マウスは使えません。実機では未確認です。

## 実機で起動する
1. FAT32 の USB メモリを用意する
2. `build/esp/` の中の `EFI` フォルダを、そのまま USB メモリの一番上にコピーする
   (`BOOTX64.EFI` と `BOOTAA64.EFI` の両方が入り、PC が合う方を選ぶ。
   すでに起動用に使っている USB には上書きしないこと)
3. USB メモリから起動する
   - Windows PC: 起動メニュー(F12 / F11 / F8 / Esc など機種による)で選ぶ
   - Intel Mac: 電源を入れてすぐ Option キーを押し続けて選ぶ

注意
- 署名がないため、**セキュアブートを無効**にする必要があります。
- Windows で BitLocker(デバイスの暗号化)が有効な PC は、設定変更後に回復キーを
  求められることがあります。**先に回復キーを控えてから**変更してください。
- T2 チップ搭載の Intel Mac は、macOS 復旧の「起動セキュリティユーティリティ」で
  セキュリティを下げ、外部メディアからの起動を許可する必要があります。使用後は戻してください。

## Apple シリコン Mac (UTM、手順は未検証)
1. `freeos-any-1.2.img.gz` を展開する
2. UTM で「仮想化」→「その他」で仮想マシンを作る(起動 ISO は指定しない)
3. 設定で空のドライブを外し、展開した `.img` をドライブとして読み込む。UEFI 起動は有効のまま

## 機能
スタートメニュー、ターミナル、エディター、ブラウザ、ファイル転送、ファイル管理、ソリティア

## 操作
| キー | 動作 |
|---|---|
| Esc | スタートメニュー(通信中は中止) |
| F1 / F4 / F10 | ウィンドウ切替 / 閉じる / 最大化 |
| F5〜F9 | ターミナル / ファイル / エディター / ブラウザ / ファイル転送 |
| F2 / F3 / Insert | (エディター) 保存 / ローマ字かな入力 / 新規 |
| F2 / Tab / Enter / BackSpace / s | (ブラウザ) URL入力 / リンク選択 / 開く / 戻る / ページ保存 |
| 矢印 / Enter / Space / A / U / N | (ソリティア) 選択 / 持つ・置く / めくる / 組札へ自動 / 元に戻す / 新規 |

ソリティアは、札を持って同じ列でもう一度 Enter を押すと組札へ送ります。
電源オフと再起動はスタートメニュー、またはターミナルの `shutdown` / `reboot`。

Mac のキーボードでは F キーは `fn` と一緒に押します。ファイルをゴミ箱へ入れる Del は `fn`+`delete`。
Insert キーがないので、新しいメモはターミナルで `edit 名前.txt` と入力して作ります。

## ネットワーク
- **有線LANのみ**です。Wi-Fi は使えません。IP アドレスは自動取得(DHCP)です。
- PC のファームウェアが LAN の機能(UEFI Simple Network Protocol)を提供する必要があります。
  設定画面に「UEFI Network Stack」などがあれば有効にします。機種によっては使えません。
- ターミナル: `net`(接続確認)、`get <URL> [名前]`(ダウンロード、16MBまで)、`web <URL>`

### ブラウザ
HTML を要素の木 (DOM) にして表示し、小さな JavaScript エンジン (`src/js.c`) で動かします。
見出し・強調・リスト・表・フォーム (GET で送信)・リンク、DOM の書き換え・イベント・タイマー・alert に対応します。
画像、CSS の配置、Cookie、Shift_JIS などには対応しません。HOME の `.html` はファイルアプリから開けます (`file:名前.html`)。
`about:demo` で見本ページが開きます。
HTTPS は TLS 1.2 まで(TLS 1.3 専用のサイトは開けません)。証明書は検証し、問題があれば接続しません。
ルート証明書はビルド時に取り込んだもので、自動では更新されません。

### ファイル転送
「ファイル転送」を開いて「開始する」を押し、表示された URL を同じ LAN の PC やスマホの
ブラウザで開くと、`HOME` のファイルのダウンロードとアップロード(1つ32MBまで)ができます。
同じ名前がある場合は別名で保存します。**通信は暗号化されません。** 信頼できる LAN だけで使い、
終わったら停止してください。ウィンドウを閉じても停止します。

## AI チャット (Qwen3-0.6B)
OS の中だけで動く AI と日本語で会話できます (通信しません)。Intel / AMD (AVX2) なら速く、それ以外は遅いですが動きます。
メモリは 1.7B なら 2GB、0.6B なら 1GB 以上必要です (Mac 版は 2GB を割り当てます)。F11 でどこからでも開けます。モデルは大きいので同梱していません。作り方:

```
# Qwen/Qwen3-0.6B (Apache-2.0) の config.json, tokenizer.json, model.safetensors を models/qwen3-0.6b に置いて
python3 tools/export_qwen3.py models/qwen3-1.7b qwen3-1.7b.fqw      # 約 1.1GB (重み 4 ビット)。0.6B なら約 400MB
# 元データを置かずに Hugging Face から直接: --url=https://huggingface.co/Qwen/Qwen3-1.7B/resolve/main
# 起動ディスクの HOME/AI/ に置く (qwen3-1.7b.fqw を優先、なければ qwen3-0.6b.fqw)。Mac なら: MODEL=qwen3-1.7b.fqw make image-mac
```

- エンジンは `src/llm.c` (C、1 コア、AVX2 で 4 ビット×8 ビット)。設計は [llm.asm](https://github.com/alesha-pro/llm.asm) (MIT) を参考にした
- 覚えていられるのは 512 トークンまで。超えたら会話を忘れて続ける
- 0.6B の小さなモデルなので、事実をよく間違えます (「日本の首都は?」に「大阪」と答えるなど。元の精度の計算でも同じ)
- `tests/llmtest.c` で Mac / Linux の上で直接試せる

## 仕組みと限界
- 画面描画、ウィンドウ管理、シェル、エディタ、かな入力、TCP/IP、HTTP、ブラウザ表示、
  ファイル転送サーバーは自作です。暗号(TLS)は [BearSSL](https://bearssl.org/) 0.6 を使っています。
- キーボード・マウス・LAN・USB への読み書き・時計は、PC 内蔵の UEFI ファームウェアの機能を使います。
  独自カーネル(メモリ保護、マルチタスク、独自ドライバ)はまだありません。
- 通信中(ページの読み込みやダウンロード)は他の操作ができません。Esc で中止できます。
- 音、漢字変換、他OSのアプリの実行はできません。
- マウスは Mac 版(仮想マシン)と、ファームウェアが対応している機種で使えます。キーボードだけで全操作できます。
- 書き込むのは起動したドライブの `HOME` フォルダの中だけです。

## ビルド
`make` (clang, lld-link, python3, GRUB の unicode.pf2)。
`make image` は mkfs.fat と mtools も使います。`make vmzip` で Mac 版の zip を作ります。
- 画面の文字は [BIZ UDゴシック](https://github.com/googlefonts/morisawa-biz-ud-gothic) (SIL Open Font License 1.1、`third_party/bizud/OFL.txt`) をなめらかな文字にして使い、
  それにない文字は GNU Unifont (GPLv2+ フォント埋め込み例外 / OFL) で描いています。
  `make` には Pillow と fontTools も必要です (`PYTHON=` で Python を指定できます)。
- `third_party/bearssl/` は BearSSL(MIT ライセンス、`LICENSE.txt` 参照。Debian のソースパッケージより)。
- `third_party/trust_anchors.h` は `brssl ta` でシステムのルート証明書一覧から生成したものです。
