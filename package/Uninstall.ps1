param([string]$SteamVR)
$ErrorActionPreference = 'Stop'
if (-not $SteamVR) {
    $steamPath = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if ($steamPath) { $SteamVR = Join-Path $steamPath 'steamapps\common\SteamVR' }
}
if (-not $SteamVR) { throw '请通过 -SteamVR 指定 SteamVR 安装目录。' }
$root = $PSScriptRoot
$location = Join-Path $PSScriptRoot 'install-location.txt'
if (Test-Path -LiteralPath $location) { $root = [IO.File]::ReadAllText($location).Trim() }
$driverRoot = (Resolve-Path -LiteralPath (Join-Path $root 'steamlinkeyeinput')).Path
$control = Join-Path $root 'SteamLinkEyeInputControl.exe'
if (Test-Path -LiteralPath $control) {
    $cleanup = Start-Process -FilePath $control -ArgumentList '--unregister-vr' -WindowStyle Hidden -PassThru
    if (-not $cleanup.WaitForExit(10000)) { throw '仪表盘清理超时；请启动 SteamVR 后重试。驱动文件未删除。' }
    if ($cleanup.ExitCode -ne 0) { throw '仪表盘清理失败；请启动 SteamVR 后重试。' }
}
& (Join-Path $SteamVR 'bin\win64\vrpathreg.exe') removedriver $driverRoot
if ($LASTEXITCODE -ne 0) { throw 'SteamVR 驱动取消注册失败。' }
Write-Host '已取消注册。重启 SteamVR 后生效；PaperTracker 配置未更改。'
