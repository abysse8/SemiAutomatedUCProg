param(
    [Parameter(Mandatory = $true)]
    [string[]] $Targets,

    [string] $Login = "sdk_system",
    [int] $Port = 2222,
    [switch] $DryRun
)

$expandedTargets = @()
foreach ($targetGroup in $Targets) {
    $expandedTargets += $targetGroup -split "," | ForEach-Object { $_.Trim() } | Where-Object { $_ }
}
$Targets = $expandedTargets

if ($Targets.Count -eq 0) {
    Write-Error "No UC proxy targets were provided."
    exit 1
}

$putty = Get-Command putty.exe -ErrorAction SilentlyContinue
$wt = Get-Command wt.exe -ErrorAction SilentlyContinue
$ssh = Get-Command ssh.exe -ErrorAction SilentlyContinue

for ($i = 0; $i -lt $Targets.Count; $i++) {
    $target = $Targets[$i]
    $title = "UC-$($i + 1) via $target"
    Write-Host "Opening $title on port $Port"

    if ($DryRun) {
        Write-Host "Dry run: would open $Login@$target on port $Port"
    } elseif ($putty) {
        Start-Process $putty.Source -ArgumentList @("-ssh", "$Login@$target", "-P", "$Port")
    } elseif ($wt -and $ssh) {
        Start-Process $wt.Source -ArgumentList @("new-tab", "--title", $title, "ssh", "-p", "$Port", "$Login@$target")
    } elseif ($ssh) {
        Start-Process cmd.exe -ArgumentList @("/k", "title $title && ssh -p $Port $Login@$target")
    } else {
        Write-Error "Could not find putty.exe or ssh.exe in PATH."
        exit 1
    }
}
