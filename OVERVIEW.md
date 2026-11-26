# FreeOS Any — プロジェクト概要 (2026-11-26 時点、ソース版 1.3)

## 目的
- 「どこでも動く」自作 OS。USB メモリから PC で単独起動でき、いつもの OS の上でも仮想マシンとして動く。
- **いまの最優先は macOS の上で動く版。** Windows / Linux 版とブラウザ版は後回し。

## これまでの流れ
1. ブラウザで動く画面を試作 → 「PC で本当に動く OS」への要望で廃棄し、UEFI の OS として作り直し
2. x86-64 と ARM64 の両対応、名称を FreeOS Any に決定
3. 1.2: スタートメニュー、ターミナル、エディター、ブラウザ、ファイル転送、ソリティア
4. Mac / Windows の上で動かす起動スクリプトを追加
5. 1.3: 仮想マシンでのマウス対応、Mac アプリ化 (dmg)

## 配布物 (dist/)
| ファイル | OS の版 | 内容 |
|---|---|---|
| `FreeOSAny-1.3.dmg` | 1.3 | Mac アプリ (**最新・優先**) |
| `freeos-any-1.2-windows.zip` | 1.2 | Windows の上で起動 (マウス非対応) |
| `freeos-any-1.2-usb.zip` | 1.2 | USB 単独起動用 |
| `freeos-any-1.2.img.gz` | 1.2 | 仮想マシン用ディスク |

USB 版とディスク版は、まだ 1.3 で作り直していません。

## 構成
- C 言語のフリースタンディング UEFI アプリ。ブートサービスを使ったまま動き、独自カーネルはありません。
- ファームウェアに任せる部分: 画面 (GOP)、キーボード、ファイル読み書き、LAN の送受信 (SNP)、時計。
- 自作する部分: ウィンドウ管理、アプリ、TCP/IP、HTTP、ブラウザ表示、仮想タブレットのドライバ。
- 外部のもの: BearSSL 0.6 (TLS)、GNU Unifont (文字)、ルート証明書一覧、QEMU (仮想化、非同梱)。
- 保存先は起動ドライブの `\HOME` の中だけです。

## ソースの地図
| 場所 | 役割 |
|---|---|
| `src/core.c` | 起動、描画、ファイル、ウィンドウ管理、スタートメニュー、入力 |
| `src/apps.c` | ターミナル、ファイル管理、エディター (かな入力)、スネーク、情報 |
| `src/browser.c` | ブラウザ (文章とリンク)、ダウンロード |
| `src/net.c` `src/net.h` | ARP / IP / ICMP / UDP / DHCP / DNS / TCP、乱数 |
| `src/http.c` | HTTP / HTTPS |
| `src/transfer.c` | ファイル転送サーバー |
| `src/solitaire.c` | ソリティア |
| `src/virtio.c` | 仮想マシン用タブレット (マウス) |
| `src/efi.h` `src/os.h` | UEFI の定義、内部の共通インターフェース |
| `vm/macapp/` | Mac アプリの本体スクリプトと説明 |
| `vm/windows/` | Windows 用の起動スクリプト |
| `tools/` | フォント生成、dmg / zip 作成、この参照ページの生成 |
| `tests/` | テストスクリプトの控え |
| `third_party/` | BearSSL と ルート証明書 |

## ビルド
```
make          # build/esp/EFI/BOOT/ に BOOTX64.EFI と BOOTAA64.EFI
make image    # build/freeos-any.img (mkfs.fat, mtools)
make dmg      # dist/FreeOSAny-<版>.dmg (xorriso, python3 の Pillow)
make vmzip    # Windows 用 zip
make srcdoc   # source-<版>.html と dist/freeos-any-<版>-src.zip
```
必要なもの: clang、lld-link、python3、GRUB の `unicode.pf2`。版番号は `Makefile` の `VER` と、
`src/core.c` `src/apps.c` `src/http.c` `vm/windows/はじめに.txt` の表記を合わせて変えます。

開発サーバーには mtools と xorriso を入れておらず、配布パッケージを一時フォルダに展開して使いました。

## 確認済みと未確認
確認は仮想 PC (QEMU) だけです。
- 確認済み: x86-64 と ARM64 の起動、キー操作、HTTPS、証明書エラーの拒否 (1.2、x86-64)、
  ファイル転送、1.3 のマウス操作、Mac アプリのスクリプト (macOS のコマンドは模擬)。
- **未確認: すべての実機、macOS 上での dmg とアプリ、hvf、QEMU の自動インストール、
  Windows の実機、UTM の手順。**

## 主な制限
- 単独起動時のネットワークは有線 LAN のみ。ファームウェアの対応が必要。
- ブラウザは文章とリンクだけ。TLS 1.2 まで。
- 通信中は他の操作ができない。ファイル転送は暗号化されない。
- Mac アプリは署名・公証なし。dmg の中身は ISO 9660 形式。
- 単独起動にはセキュアブートの無効化が必要。Apple シリコン Mac では直接起動できない。

## 次の候補
1. Mac 実機で dmg とアプリを確認し、出た問題を直す (最優先)
2. QEMU の同梱 (ライセンス上、ソース提供の対応が必要)
3. Apple の署名と公証 (有料の開発者登録が必要)
4. USB 版を 1.3 に更新、Windows 版の仕上げ

## 運用上の注意
- このフォルダは **Web で公開される場所** です。秘密の情報を置かないこと。git 管理はしていません。
- CDN が静的ファイルを数時間キャッシュするため、内容を変えた配布物は新しいファイル名にします。
- 詳しい使い方は `README.md`。
