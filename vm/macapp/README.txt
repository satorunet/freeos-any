FreeOS Any (macOS の上で動かす版)

■ 入れ方
  「FreeOS Any」を「Applications」フォルダにドラッグして、そこから開きます。

■ 初めて開くとき
  このアプリには Apple の署名がありません。止められたら、
  システム設定 →「プライバシーとセキュリティ」→「このまま開く」を選びます。
  「壊れているため開けません」と出る場合は、ここから入手したことを確認のうえ、
  ターミナルで次を実行します。
      xattr -dr com.apple.quarantine "/Applications/FreeOS Any.app"

■ 必要なもの
  無料の仮想化ソフト QEMU (なければ画面の案内でインストール。先に Homebrew https://brew.sh/ja/ が必要)

■ 使い方
  ・マウスとキーボードで操作します。F キーは fn と一緒に押します。
  ・終了: スタートメニューの「シャットダウン」
  ・キー入力を Mac に戻す: control + option + G
  ・ファイル転送: OS で開始したあと、この Mac で http://localhost:8080/番号/ を開きます
    (ポート番号はウィンドウのタイトルに表示されます)
  ・作ったファイルは次の場所のディスクに保存され、アプリを入れ替えても残ります。
      ~/Library/Application Support/FreeOS Any/
