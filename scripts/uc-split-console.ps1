param(
    [Parameter(Mandatory = $true)]
    [string[]] $Targets,

    [string] $Login = "sdk_system",
    [int] $Port = 2222,
    [switch] $UseOpenSsh,
    [switch] $DryRun
)

function Expand-Targets {
    param([string[]] $RawTargets)

    $expanded = @()
    foreach ($targetGroup in $RawTargets) {
        $expanded += $targetGroup -split "," | ForEach-Object { $_.Trim() } | Where-Object { $_ }
    }
    return $expanded
}

function Join-CmdArguments {
    param([string[]] $Arguments)

    $quoted = @()
    foreach ($arg in $Arguments) {
        if ($arg -match '^[A-Za-z0-9_./:@=-]+$') {
            $quoted += $arg
        } else {
            $quoted += '"' + ($arg -replace '"', '\"') + '"'
        }
    }
    return $quoted -join " "
}

function New-UcPaneCommand {
    param(
        [string] $BackendPath,
        [string] $Backend,
        [string] $Target,
        [string] $Login,
        [int] $Port
    )

    if ($Backend -eq "plink") {
        $connect = Join-CmdArguments @($BackendPath, "-ssh", "-t", "-P", "$Port", "-l", $Login, $Target)
    } else {
        $connect = Join-CmdArguments @($BackendPath, "-tt", "-p", "$Port", "$Login@$Target")
    }

    return "chcp 65001 > nul & title $Target & $connect"
}

$Targets = Expand-Targets $Targets
if ($Targets.Count -eq 0) {
    Write-Error "No UC proxy targets were provided."
    exit 1
}

$wt = Get-Command wt.exe -ErrorAction SilentlyContinue
if (-not $wt -and -not $DryRun) {
    Write-Error "Windows Terminal (wt.exe) was not found. Install/open Windows Terminal, or use uc-operator-console.cmd."
    exit 1
}

$backend = $null
$backendPath = $null
if (-not $UseOpenSsh) {
    $plink = Get-Command plink.exe -ErrorAction SilentlyContinue
    if ($plink) {
        $backend = "plink"
        $backendPath = $plink.Source
    }
}

if (-not $backend) {
    $ssh = Get-Command ssh.exe -ErrorAction SilentlyContinue
    if ($ssh) {
        $backend = "ssh"
        $backendPath = $ssh.Source
    }
}

if (-not $backend) {
    Write-Error "Could not find plink.exe or ssh.exe in PATH."
    exit 1
}

$wtArgs = @()
for ($i = 0; $i -lt $Targets.Count; $i++) {
    $target = $Targets[$i]
    $title = "UC-$($i + 1) $target"
    $cmd = New-UcPaneCommand -BackendPath $backendPath -Backend $backend -Target $target -Login $Login -Port $Port

    if ($i -eq 0) {
        $wtArgs += @("new-tab", "--title", $title, "cmd.exe", "/k", $cmd)
    } else {
        $split = if ($i % 2 -eq 1) { "--horizontal" } else { "--vertical" }
        $wtArgs += @(";", "split-pane", $split, "--title", $title, "cmd.exe", "/k", $cmd)
    }
}

Write-Host "Opening Windows Terminal split console"
Write-Host "Backend: $backend"
Write-Host "Login:   $Login"
Write-Host "Port:    $Port"
Write-Host "Targets: $($Targets -join ', ')"

if ($DryRun) {
    Write-Host ""
    Write-Host "wt.exe $($wtArgs -join ' ')"
    exit 0
}

$wtPath = if ($wt) { $wt.Source } else { "wt.exe" }
Start-Process $wtPath -ArgumentList $wtArgs
