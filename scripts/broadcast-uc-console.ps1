param(
    [Parameter(Mandatory = $true)]
    [string[]] $Targets,

    [string] $Login = "sdk_system",
    [int] $Port = 2222,
    [switch] $UseOpenSsh,
    [switch] $AskPassword,
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

function Convert-SecureStringToPlainText {
    param([securestring] $Secure)

    if (-not $Secure) {
        return $null
    }

    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($Secure)
    try {
        return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
    } finally {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
    }
}

function Join-ProcessArguments {
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

$Targets = Expand-Targets $Targets
if ($Targets.Count -eq 0) {
    Write-Error "No UC proxy targets were provided."
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

$plainPassword = $null
if ($AskPassword) {
    if ($backend -ne "plink") {
        Write-Error "-AskPassword requires plink.exe. Use PuTTY/plink, Pageant, or key auth."
        exit 1
    }
    Write-Warning "Plink receives the password as a process argument. Use this only on the bench PC."
    $plainPassword = Convert-SecureStringToPlainText (Read-Host "UC password for $Login" -AsSecureString)
}

Write-Host "UC broadcast console"
Write-Host "Backend: $backend"
Write-Host "Login:   $Login"
Write-Host "Port:    $Port"
Write-Host "Targets: $($Targets -join ', ')"
Write-Host ""
Write-Host "Type one UC menu command per line. It will be sent to every connected target."
Write-Host "Local commands: :q exits, :targets shows targets, :send N TEXT sends only to target number N."
Write-Host ""

if ($DryRun) {
    Write-Host "Dry run only; no SSH sessions opened."
    exit 0
}

$sessions = @()

try {
    for ($i = 0; $i -lt $Targets.Count; $i++) {
        $target = $Targets[$i]
        $label = "UC-$($i + 1)"

        if ($backend -eq "plink") {
            $argsList = @("-ssh", "-t", "-P", "$Port")
            if ($plainPassword) {
                $argsList += @("-pw", $plainPassword)
            }
            $argsList += "$Login@$target"
        } else {
            $argsList = @("-tt", "-p", "$Port", "$Login@$target")
        }

        $psi = [Diagnostics.ProcessStartInfo]::new()
        $psi.FileName = $backendPath
        $psi.Arguments = Join-ProcessArguments $argsList
        $psi.UseShellExecute = $false
        $psi.RedirectStandardInput = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.CreateNoWindow = $true

        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $psi
        [void]$process.Start()

        $session = [pscustomobject]@{
            Index = $i + 1
            Label = $label
            Target = $target
            Process = $process
        }
        $sessions += $session

        $outEvent = "uc_out_$($process.Id)"
        $errEvent = "uc_err_$($process.Id)"
        Register-ObjectEvent -InputObject $process -EventName OutputDataReceived -SourceIdentifier $outEvent -Action {
            if ($EventArgs.Data) {
                Write-Host "[$($Event.MessageData.Label) $($Event.MessageData.Target)] $($EventArgs.Data)"
            }
        } -MessageData $session | Out-Null
        Register-ObjectEvent -InputObject $process -EventName ErrorDataReceived -SourceIdentifier $errEvent -Action {
            if ($EventArgs.Data) {
                if ($EventArgs.Data -match '^Using username ') {
                    Write-Host "[$($Event.MessageData.Label) $($Event.MessageData.Target) local] $($EventArgs.Data)"
                } else {
                    Write-Host "[$($Event.MessageData.Label) $($Event.MessageData.Target) err] $($EventArgs.Data)"
                }
            }
        } -MessageData $session | Out-Null

        $process.BeginOutputReadLine()
        $process.BeginErrorReadLine()
        Write-Host "Opened $label via $target"
    }

    while ($true) {
        $line = Read-Host "uc-all"
        if ($line -eq ":q") {
            break
        }
        if ($line -eq ":targets") {
            foreach ($session in $sessions) {
                $state = if ($session.Process.HasExited) { "exited" } else { "running" }
                Write-Host "$($session.Index): $($session.Target) ($state)"
            }
            continue
        }
        if ($line -match "^:send\s+(\d+)\s+(.*)$") {
            $index = [int]$Matches[1]
            $text = $Matches[2]
            $session = $sessions | Where-Object { $_.Index -eq $index } | Select-Object -First 1
            if (-not $session) {
                Write-Warning "No target number $index"
                continue
            }
            if (-not $session.Process.HasExited) {
                $session.Process.StandardInput.WriteLine($text)
            }
            continue
        }

        foreach ($session in $sessions) {
            if ($session.Process.HasExited) {
                Write-Warning "$($session.Label) $($session.Target) has exited; command not sent there."
                continue
            }
            $session.Process.StandardInput.WriteLine($line)
        }
    }
} finally {
    foreach ($session in $sessions) {
        if (-not $session.Process.HasExited) {
            try {
                $session.Process.StandardInput.WriteLine("exit")
                Start-Sleep -Milliseconds 150
            } catch {
            }
        }
        if (-not $session.Process.HasExited) {
            $session.Process.Kill()
        }
        $session.Process.Dispose()
    }
    Get-EventSubscriber | Where-Object { $_.SourceIdentifier -like "uc_out_*" -or $_.SourceIdentifier -like "uc_err_*" } | Unregister-Event
}
