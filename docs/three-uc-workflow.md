# Three UC Workflow

Each UC keeps its default Ethernet address:

```text
UC Ethernet IP: 192.168.1.100
UC SSH port:   22
```

Each ESP32-P4 is a separate proxy island:

```text
ESP Ethernet IP toward its UC: 192.168.1.101
ESP Wi-Fi IP from router:     assigned by DHCP
ESP SSH proxy port:           2222
```

Because the UC network is isolated behind each ESP, three UCs can all use
`192.168.1.100` at the same time. Your PC connects to the three different ESP
Wi-Fi IPs instead.

## Open Sessions

Replace the sample IPs with the ESP Wi-Fi IPs printed in their serial logs or
shown by the router DHCP client list. Pass one IP for the testbench, three IPs
for a three-UC run, or any other number of connected ESP/UC pairs.

```powershell
.\scripts\open-uc-sessions.ps1 -Targets 192.168.11.188,192.168.11.189,192.168.11.190
```

If PowerShell blocks script execution on the PC, use the `.cmd` wrapper instead:

```powershell
.\scripts\open-uc-sessions.cmd -Targets 192.168.11.188,192.168.11.189,192.168.11.190
```

For the single testbench UC:

```powershell
.\scripts\open-uc-sessions.cmd -Targets 192.168.11.188
```

To check target parsing without opening terminal windows:

```powershell
.\scripts\open-uc-sessions.cmd -Targets 192.168.11.188,192.168.11.189 -DryRun
```

When PuTTY is installed, the launcher opens PuTTY without forcing a window title
because some PuTTY builds reject the title argument.

The launcher defaults to `sdk_system`. Use another login only if the UC asks for
it. Current known candidates:

```text
sdk_system
admin
```

## Update Run

1. Connect each ESP Ethernet port to one UC.
2. Power the UCs.
3. Open the three SSH sessions.
4. Keep the terminals side by side and run the same menu steps on UC-1, UC-2,
   and UC-3 before moving to the next step.
5. Leave the ESP mode in OFF unless you intentionally switch to M0 or M1.

The fallback ESP AP stays available at `192.168.4.1:2222`, but normal operation
should use the ESP Wi-Fi IP from the router.

## Broadcast Console

For practicing the numbered UC menu flow from one operator prompt, use the
broadcast console. It opens one persistent SSH session per ESP target and sends
each line you type to all connected UCs.

Dry run:

```powershell
.\scripts\broadcast-uc-console.cmd -Targets 192.168.11.188 -DryRun
```

One testbench UC:

```powershell
.\scripts\broadcast-uc-console.cmd -Targets 192.168.11.188
```

Three UCs:

```powershell
.\scripts\broadcast-uc-console.cmd -Targets 192.168.11.188,192.168.11.189,192.168.11.190
```

If password auth is required and `plink.exe` is installed with PuTTY:

```powershell
.\scripts\broadcast-uc-console.cmd -Targets 192.168.11.188 -AskPassword
```

At the `uc-all` prompt:

```text
2
6
0
:targets
:send 1 0
:q
```

`:send N TEXT` sends only to one target, which is useful when one UC is slower
or lands on a different prompt than the others.

## Operator Window

For real update work, use the operator window so every UC is visible at once.
It opens one pane per ESP target and keeps a shared command box at the bottom.

```powershell
.\scripts\uc-operator-console.cmd -Targets 192.168.11.188
```

If the pane shows the username line but no password prompt or menu, relaunch
with password prompting:

```powershell
.\scripts\uc-operator-console.cmd -Targets 192.168.11.188 -AskPassword
```

For three devices:

```powershell
.\scripts\uc-operator-console.cmd -Targets 192.168.11.188,192.168.11.189,192.168.11.190
```

Press Enter or click `Send All` to send the command box to every UC. Each pane
also has its own send button for catching up a single UC before continuing.

## Split Terminal

For the cleanest UC menu display, use real Windows Terminal panes. This preserves
terminal behavior better than the GUI console and also sets the pane code page
to UTF-8 before connecting.

```powershell
.\scripts\uc-split-console.cmd -Targets 192.168.11.188
```

Three devices:

```powershell
.\scripts\uc-split-console.cmd -Targets 192.168.11.188,192.168.11.189,192.168.11.190
```

This is the best view for keeping the UCs visually aligned. Use it when you want
maximum confidence in the screen state; use the operator window when you want a
single shared command box.
