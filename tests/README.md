# テストの控え
開発中に使ったスクリプトをそのまま保存したものです。置き場所(/t など)や展開した QEMU の場所を
前提にしているので、動かすには環境の準備と書き換えが必要です。

| ファイル | 内容 |
|---|---|
| `vm-x64.py` | x86-64 の仮想 PC を起動し、キー入力して画面を撮る (配布パッケージから展開した QEMU を `root/` に置く) |
| `vm-arm64.py` | 同じことを ARM64 で行う (コンテナ内) |
| `macapp-test.py` と `macfake/` | Mac アプリのスクリプトを、macOS のコマンドを模擬して試す (本家版 QEMU 入りの Alpine コンテナ内) |
| `launcher-drive.py` | 旧 Mac zip 版 / Windows 版の起動確認 (Mac zip 版は廃止済み) |
| `solitaire-fuzz.c` | ソリティアのランダム操作テスト (`-Isrc`、ASan/UBSan で実行) |
