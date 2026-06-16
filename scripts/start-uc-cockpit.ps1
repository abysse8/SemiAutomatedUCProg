param(
    [int] $Port = 3100,
    [string] $HostAddress = "0.0.0.0"
)

$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
$env:UC_DASHBOARD_PORT = "$Port"
$env:UC_DASHBOARD_HOST = "$HostAddress"
Set-Location $repo
Write-Host "Starting UC cockpit on $HostAddress port $Port"
Write-Host "Local: http://127.0.0.1:$Port"
$ips = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object {
        $_.IPAddress -notlike "127.*" -and
        $_.IPAddress -notlike "169.254.*" -and
        $_.PrefixOrigin -ne "WellKnown"
    } |
    Select-Object -ExpandProperty IPAddress -Unique
foreach ($ip in $ips) {
    Write-Host "LAN:   http://$ip`:$Port"
}
Write-Host "Leave this window open while using the dashboard."
node app\server.js
