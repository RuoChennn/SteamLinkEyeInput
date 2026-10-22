param([ValidateSet('On','Off','Status')][string]$Action='Status')
$ErrorActionPreference = 'Stop'
$installationRoot = $PSScriptRoot
$location = Join-Path $PSScriptRoot 'install-location.txt'
if (Test-Path -LiteralPath $location) { $installationRoot = [IO.File]::ReadAllText($location).Trim() }
$root = Join-Path $installationRoot 'steamlinkeyeinput'
if ($Action -ne 'Status') {
    $path = Join-Path $root 'steamlinkeyeinput.ini'
    $content = [IO.File]::ReadAllText($path)
    $value = if ($Action -eq 'On') { '1' } else { '0' }
    $content = [regex]::Replace($content, '(?m)^enabled=[01]\r?$', "enabled=$value")
    $temporary = Join-Path $root ('config-' + [guid]::NewGuid().ToString('N') + '.tmp')
    $backup = Join-Path $root 'steamlinkeyeinput.ini.previous'
    try {
        [IO.File]::WriteAllText($temporary,$content,[Text.UTF8Encoding]::new($false))
        $replaced = $false
        for ($attempt=0; $attempt -lt 20; $attempt++) {
            try { [IO.File]::Replace($temporary,$path,$backup); $replaced=$true; break }
            catch [IO.IOException] { Start-Sleep -Milliseconds 10 }
        }
        if (-not $replaced) { throw '配置文件暂时被占用，请重试。' }
    } finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary } }
    Write-Host "已设置 enabled=$value，插件将在 0.5 秒内应用。"
} else {
    $statusPath = Join-Path $root 'status.json'
    if (Test-Path -LiteralPath $statusPath) { Get-Content -LiteralPath $statusPath -Raw }
    else { Write-Host '尚无状态文件，请启动已安装此插件的 SteamVR。' }
}
