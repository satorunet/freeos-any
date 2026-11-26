# FreeOS Any を Windows の上で(仮想マシンとして)起動する。QEMU が必要。
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot

function Fail($msg) {
    Write-Host ""
    Write-Host "エラー: $msg" -ForegroundColor Red
    Read-Host "Enter キーで閉じます" | Out-Null
    exit 1
}

function Find-Qemu {
    $dirs = @($env:FREEOS_QEMU_BIN)
    if ($env:ProgramFiles) { $dirs += Join-Path $env:ProgramFiles 'qemu' }
    foreach ($d in $dirs) {
        if ($d) {
            $p = Join-Path $d 'qemu-system-x86_64.exe'
            if (Test-Path -LiteralPath $p) { return $p }
        }
    }
    $c = Get-Command 'qemu-system-x86_64.exe' -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $null
}

try {
    if (-not (Test-Path -LiteralPath 'freeos-any.img')) {
        Fail "freeos-any.img がありません。zip をすべて展開してから起動してください。"
    }

    $qemu = Find-Qemu
    if (-not $qemu) {
        Write-Host "仮想化ソフト QEMU が入っていません。"
        if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
            Fail "https://qemu.weilnetz.de/w64/ から QEMU を入れて、もう一度起動してください。"
        }
        $a = Read-Host "winget で QEMU をインストールしますか? [y/N]"
        if ($a -notmatch '^[yY]') { exit 0 }
        winget install --id SoftwareFreedomConservancy.QEMU --exact
        $qemu = Find-Qemu
        if (-not $qemu) { Fail "QEMU が見つかりません。インストール結果を確認してください。" }
    }

    # ファームウェアは最初の起動時にこのフォルダへコピーする
    $dir = Split-Path -Parent $qemu
    foreach ($pair in @(@('edk2-x86_64-code.fd', 'fw-x86_64-code.fd'), @('edk2-i386-vars.fd', 'fw-x86_64-vars.fd'))) {
        if (-not (Test-Path -LiteralPath $pair[1])) {
            $src = @((Join-Path $dir "share\$($pair[0])"), (Join-Path $dir "..\share\qemu\$($pair[0])")) |
                Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
            if (-not $src) { Fail "UEFI ファームウェア $($pair[0]) が QEMU のフォルダにありません。" }
            Copy-Item -LiteralPath $src -Destination $pair[1]
        }
    }
    Set-ItemProperty -LiteralPath 'fw-x86_64-vars.fd' -Name IsReadOnly -Value $false

    # ファイル転送用: この PC の localhost のポートを仮想マシンの 80 番につなぐ
    $port = $null
    foreach ($p in 8080..8099) {
        try {
            $l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $p)
            $l.Start(); $l.Stop(); $port = $p; break
        } catch { }
    }
    if (-not $port) { Fail "空いているポートがありません (8080-8099)。" }

    Write-Host "FreeOS Any を起動します。窓をクリックするとキー入力できます。"
    Write-Host "  Esc: スタートメニュー   終了: スタートメニューの「シャットダウン」"
    Write-Host "  キーとマウスを Windows に戻す: Ctrl + Alt + G"
    Write-Host "  ファイル転送: 開始後、この PC で http://localhost:$port/番号/ を開く"

    # 確実に動くことを優先して、エミュレーション(tcg)で動かす
    $vm = @('-name', 'FreeOS Any', '-m', '512', '-machine', 'q35', '-accel', 'tcg', '-cpu', 'max',
        '-drive', 'if=pflash,format=raw,readonly=on,file=fw-x86_64-code.fd',
        '-drive', 'if=pflash,format=raw,file=fw-x86_64-vars.fd',
        '-drive', 'if=none,id=disk,format=raw,file=freeos-any.img',
        '-device', 'virtio-blk-pci,drive=disk',
        '-nic', "user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:$port-:80",
        '-device', 'virtio-rng-pci', '-device', 'virtio-tablet-pci')
    if ($env:FREEOS_QEMU_EXTRA) { $vm += $env:FREEOS_QEMU_EXTRA -split ' ' }
    # QEMU の警告表示だけで止まらないようにする
    $ErrorActionPreference = 'Continue'
    & $qemu @vm
    if ($LASTEXITCODE -ne 0) {
        Fail "起動できませんでした(上の表示を確認してください。二重起動していないかも確認してください)。"
    }
} catch {
    Fail $_.Exception.Message
}
