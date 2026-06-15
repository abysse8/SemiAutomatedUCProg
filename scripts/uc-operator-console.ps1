param(
    [Parameter(Mandatory = $true)]
    [string[]] $Targets,

    [string] $Login = "sdk_system",
    [int] $Port = 2222,
    [switch] $AskPassword,
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

function Append-ConsoleText {
    param(
        [System.Windows.Forms.RichTextBox] $Box,
        [string] $Text
    )

    $Box.AppendText($Text)
    $Box.SelectionStart = $Box.TextLength
    $Box.ScrollToCaret()
}

function Send-SessionLine {
    param(
        $Session,
        [string] $Line
    )

    if ($Session.Process.HasExited) {
        Append-ConsoleText $Session.Box "`r`n[local] session has exited; not sent: $Line`r`n"
        return
    }
    $Session.Process.StandardInput.WriteLine($Line)
    Append-ConsoleText $Session.Box "`r`n> $Line`r`n"
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

if ($DryRun) {
    Write-Host "UC operator console"
    Write-Host "Backend: $backend"
    Write-Host "Login:   $Login"
    Write-Host "Port:    $Port"
    Write-Host "Targets: $($Targets -join ', ')"
    exit 0
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$form = [System.Windows.Forms.Form]::new()
$form.Text = "UC Operator Console"
$form.Width = 1320
$form.Height = 850
$form.StartPosition = "CenterScreen"

$root = [System.Windows.Forms.TableLayoutPanel]::new()
$root.Dock = "Fill"
$root.RowCount = 3
$root.ColumnCount = 1
$root.RowStyles.Add([System.Windows.Forms.RowStyle]::new([System.Windows.Forms.SizeType]::Absolute, 42)) | Out-Null
$root.RowStyles.Add([System.Windows.Forms.RowStyle]::new([System.Windows.Forms.SizeType]::Percent, 100)) | Out-Null
$root.RowStyles.Add([System.Windows.Forms.RowStyle]::new([System.Windows.Forms.SizeType]::Absolute, 58)) | Out-Null
$form.Controls.Add($root)

$status = [System.Windows.Forms.Label]::new()
$status.Dock = "Fill"
$status.TextAlign = "MiddleLeft"
$status.Padding = [System.Windows.Forms.Padding]::new(10, 0, 0, 0)
$status.Font = [System.Drawing.Font]::new("Segoe UI", 10)
$authHint = if ($backend -eq "plink" -and -not $plainPassword) { "    Auth: interactive prompts may be hidden; relaunch with -AskPassword if login stalls" } else { "" }
$status.Text = "Backend: $backend    Login: $Login    Port: $Port$authHint    Targets: $($Targets -join ', ')"
$root.Controls.Add($status, 0, 0)

$cols = [Math]::Ceiling([Math]::Sqrt($Targets.Count))
$rows = [Math]::Ceiling($Targets.Count / $cols)
$grid = [System.Windows.Forms.TableLayoutPanel]::new()
$grid.Dock = "Fill"
$grid.ColumnCount = $cols
$grid.RowCount = $rows
for ($c = 0; $c -lt $cols; $c++) {
    $grid.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new([System.Windows.Forms.SizeType]::Percent, 100 / $cols)) | Out-Null
}
for ($r = 0; $r -lt $rows; $r++) {
    $grid.RowStyles.Add([System.Windows.Forms.RowStyle]::new([System.Windows.Forms.SizeType]::Percent, 100 / $rows)) | Out-Null
}
$root.Controls.Add($grid, 0, 1)

$commandPanel = [System.Windows.Forms.TableLayoutPanel]::new()
$commandPanel.Dock = "Fill"
$commandPanel.ColumnCount = 3
$commandPanel.RowCount = 1
$commandPanel.Padding = [System.Windows.Forms.Padding]::new(8)
$commandPanel.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new([System.Windows.Forms.SizeType]::Percent, 100)) | Out-Null
$commandPanel.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new([System.Windows.Forms.SizeType]::Absolute, 110)) | Out-Null
$commandPanel.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new([System.Windows.Forms.SizeType]::Absolute, 110)) | Out-Null
$root.Controls.Add($commandPanel, 0, 2)

$commandBox = [System.Windows.Forms.TextBox]::new()
$commandBox.Dock = "Fill"
$commandBox.Font = [System.Drawing.Font]::new("Consolas", 13)
$commandPanel.Controls.Add($commandBox, 0, 0)

$sendAllButton = [System.Windows.Forms.Button]::new()
$sendAllButton.Text = "Send All"
$sendAllButton.Dock = "Fill"
$sendAllButton.Font = [System.Drawing.Font]::new("Segoe UI", 10)
$commandPanel.Controls.Add($sendAllButton, 1, 0)

$clearButton = [System.Windows.Forms.Button]::new()
$clearButton.Text = "Clear"
$clearButton.Dock = "Fill"
$clearButton.Font = [System.Drawing.Font]::new("Segoe UI", 10)
$commandPanel.Controls.Add($clearButton, 2, 0)

$sessions = @()

for ($i = 0; $i -lt $Targets.Count; $i++) {
    $target = $Targets[$i]
    $label = "UC-$($i + 1)"

    $pane = [System.Windows.Forms.TableLayoutPanel]::new()
    $pane.Dock = "Fill"
    $pane.RowCount = 3
    $pane.ColumnCount = 1
    $pane.Padding = [System.Windows.Forms.Padding]::new(6)
    $pane.RowStyles.Add([System.Windows.Forms.RowStyle]::new([System.Windows.Forms.SizeType]::Absolute, 28)) | Out-Null
    $pane.RowStyles.Add([System.Windows.Forms.RowStyle]::new([System.Windows.Forms.SizeType]::Percent, 100)) | Out-Null
    $pane.RowStyles.Add([System.Windows.Forms.RowStyle]::new([System.Windows.Forms.SizeType]::Absolute, 36)) | Out-Null

    $header = [System.Windows.Forms.Label]::new()
    $header.Dock = "Fill"
    $header.TextAlign = "MiddleLeft"
    $header.Font = [System.Drawing.Font]::new("Segoe UI", 10, [System.Drawing.FontStyle]::Bold)
    $header.Text = "$label  $target"
    $pane.Controls.Add($header, 0, 0)

    $box = [System.Windows.Forms.RichTextBox]::new()
    $box.Dock = "Fill"
    $box.ReadOnly = $true
    $box.BackColor = [System.Drawing.Color]::FromArgb(18, 18, 18)
    $box.ForeColor = [System.Drawing.Color]::FromArgb(214, 245, 214)
    $box.Font = [System.Drawing.Font]::new("Consolas", 10)
    $box.WordWrap = $false
    $pane.Controls.Add($box, 0, 1)

    $sendOneButton = [System.Windows.Forms.Button]::new()
    $sendOneButton.Text = "Send to $label"
    $sendOneButton.Dock = "Fill"
    $sendOneButton.Font = [System.Drawing.Font]::new("Segoe UI", 9)
    $pane.Controls.Add($sendOneButton, 0, 2)

    $grid.Controls.Add($pane, $i % $cols, [Math]::Floor($i / $cols))

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
        Box = $box
        Header = $header
        Queue = [System.Collections.Concurrent.ConcurrentQueue[string]]::new()
    }
    $sessions += $session

    $sendOneButton.Tag = $session
    $sendOneButton.Add_Click({
        $line = $commandBox.Text
        if ($line.Length -gt 0) {
            Send-SessionLine $this.Tag $line
            $commandBox.SelectAll()
            $commandBox.Focus()
        }
    })

    Register-ObjectEvent -InputObject $process -EventName OutputDataReceived -SourceIdentifier "uc_gui_out_$($process.Id)" -Action {
        if ($EventArgs.Data -ne $null) {
            $Event.MessageData.Queue.Enqueue($EventArgs.Data + "`r`n")
        }
    } -MessageData $session | Out-Null
    Register-ObjectEvent -InputObject $process -EventName ErrorDataReceived -SourceIdentifier "uc_gui_err_$($process.Id)" -Action {
        if ($EventArgs.Data -ne $null) {
            if ($EventArgs.Data -match '^Using username ') {
                $Event.MessageData.Queue.Enqueue("[local] " + $EventArgs.Data + "`r`n")
            } else {
                $Event.MessageData.Queue.Enqueue("[err] " + $EventArgs.Data + "`r`n")
            }
        }
    } -MessageData $session | Out-Null

    $process.BeginOutputReadLine()
    $process.BeginErrorReadLine()
    Append-ConsoleText $box "[local] opening $label via $target as $Login on port $Port`r`n"
    if ($backend -eq "plink" -and -not $plainPassword) {
        Append-ConsoleText $box "[local] If this stops before the UC menu, close this window and relaunch with -AskPassword.`r`n"
    }
}

$sendAll = {
    $line = $commandBox.Text
    if ($line.Length -eq 0) {
        return
    }
    foreach ($session in $sessions) {
        Send-SessionLine $session $line
    }
    $commandBox.SelectAll()
    $commandBox.Focus()
}

$sendAllButton.Add_Click($sendAll)
$commandBox.Add_KeyDown({
    if ($_.KeyCode -eq [System.Windows.Forms.Keys]::Enter) {
        $_.SuppressKeyPress = $true
        & $sendAll
    }
})

$clearButton.Add_Click({
    foreach ($session in $sessions) {
        $session.Box.Clear()
    }
    $commandBox.Focus()
})

$timer = [System.Windows.Forms.Timer]::new()
$timer.Interval = 100
$timer.Add_Tick({
    foreach ($session in $sessions) {
        $chunk = $null
        while ($session.Queue.TryDequeue([ref]$chunk)) {
            Append-ConsoleText $session.Box $chunk
        }
        if ($session.Process.HasExited) {
            $session.Header.Text = "$($session.Label)  $($session.Target)  [exited]"
            $session.Header.ForeColor = [System.Drawing.Color]::DarkRed
        }
    }
})
$timer.Start()

$form.Add_FormClosing({
    $timer.Stop()
    foreach ($session in $sessions) {
        if (-not $session.Process.HasExited) {
            try {
                $session.Process.StandardInput.WriteLine("exit")
                Start-Sleep -Milliseconds 100
            } catch {
            }
        }
        if (-not $session.Process.HasExited) {
            $session.Process.Kill()
        }
        $session.Process.Dispose()
    }
    Get-EventSubscriber | Where-Object { $_.SourceIdentifier -like "uc_gui_out_*" -or $_.SourceIdentifier -like "uc_gui_err_*" } | Unregister-Event
})

[void]$form.ShowDialog()
