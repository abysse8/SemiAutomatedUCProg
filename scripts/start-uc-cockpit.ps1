param(
    [int] $Port = 3100
)

$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
$env:UC_DASHBOARD_PORT = "$Port"
Set-Location $repo
Write-Host "Starting UC cockpit at http://127.0.0.1:$Port"
Write-Host "Leave this window open while using the dashboard."
node app\server.js
