param([ValidateSet('Release','Debug')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
Push-Location $PSScriptRoot
try {
    cmake -S . -B build -A x64
    if($LASTEXITCODE -ne 0){throw 'CMake configure failed'}
    cmake --build build --config $Configuration --parallel
    if($LASTEXITCODE -ne 0){throw 'Build failed'}
    ctest --test-dir build -C $Configuration --output-on-failure
    if($LASTEXITCODE -ne 0){throw 'Tests failed'}
    cmake --install build --config $Configuration --prefix dist/SteamLinkEyeInput
    if($LASTEXITCODE -ne 0){throw 'Packaging failed'}
} finally {Pop-Location}
