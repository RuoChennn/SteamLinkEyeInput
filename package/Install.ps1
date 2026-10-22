param([string]$SteamVR,[string]$InstallRoot = (Join-Path $env:PUBLIC 'Documents\SteamLinkEyeInput'))
$ErrorActionPreference = 'Stop'
if (-not $SteamVR) {
    $steamPath = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if ($steamPath) { $SteamVR = Join-Path $steamPath 'steamapps\common\SteamVR' }
}
if (-not $SteamVR -or -not (Test-Path -LiteralPath (Join-Path $SteamVR 'bin\win64\vrpathreg.exe'))) {
    throw '请通过 -SteamVR 指定 SteamVR 安装目录。'
}
$InstallRoot = [IO.Path]::GetFullPath($InstallRoot)
if ($InstallRoot -match '[^\x20-\x7e]') { throw 'vrpathreg 需要纯英文安装路径，请通过 -InstallRoot 指定。' }
if ([IO.Path]::GetPathRoot($InstallRoot) -eq $InstallRoot) { throw '安装位置必须是独立文件夹。' }
if ($InstallRoot.TrimEnd('\') -ne $PSScriptRoot.TrimEnd('\')) {
    New-Item -ItemType Directory -Force -Path $InstallRoot | Out-Null
    foreach ($item in Get-ChildItem -LiteralPath $PSScriptRoot -Force) {
        if ($item.Name -in @('install-location.txt','status.json','status.tmp')) { continue }
        if ($item.Name -eq 'steamlinkeyeinput') {
            $destination = Join-Path $InstallRoot 'steamlinkeyeinput'
            New-Item -ItemType Directory -Force -Path $destination | Out-Null
            foreach ($part in Get-ChildItem -LiteralPath $item.FullName -Force) {
                if ($part.Name -in @('status.json','status.tmp')) { continue }
                if ($part.Name -eq 'steamlinkeyeinput.ini' -and (Test-Path -LiteralPath (Join-Path $destination $part.Name))) { continue }
                Copy-Item -LiteralPath $part.FullName -Destination $destination -Recurse -Force
            }
        } else { Copy-Item -LiteralPath $item.FullName -Destination $InstallRoot -Recurse -Force }
    }
}
$driverRoot = (Resolve-Path -LiteralPath (Join-Path $InstallRoot 'steamlinkeyeinput')).Path
$configPath = Join-Path $driverRoot 'steamlinkeyeinput.ini'
$configuration = [IO.File]::ReadAllText($configPath)
$configuration = [regex]::Replace($configuration,'(?m)^Eye(?:Left|Right)[XY]=[^\r\n]*\r?\n','')
[IO.File]::WriteAllText($configPath,$configuration,[Text.UTF8Encoding]::new($false))
& (Join-Path $SteamVR 'bin\win64\vrpathreg.exe') adddriver $driverRoot
if ($LASTEXITCODE -ne 0) { throw 'SteamVR 驱动注册失败。' }
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'install-location.txt'),$InstallRoot,[Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $InstallRoot 'install-location.txt'),$InstallRoot,[Text.UTF8Encoding]::new($false))
Write-Host "已注册 SteamLinkEyeInput：$driverRoot"
Write-Host '下次启动 SteamVR 自动加载。请保留此文件夹，关闭原有外部眼追桥接程序以释放 UDP 9100。'
