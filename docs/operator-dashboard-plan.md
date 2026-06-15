# Operator Dashboard Plan

The dashboard should run on the PC, not on the ESP32.

The ESP32 is only a network island and USB-mode controller:

- Ethernet side: talks to its UC at `192.168.1.100`.
- Wi-Fi side: reachable from the operator PC.
- USB side: exposes `OFF`, `M0`, or `M1`.

The PC must own the SSH sessions because SSH traffic is encrypted. The ESP32 can
proxy the bytes, but it cannot reliably know which UC menu screen is currently
visible.

## Target Experience

One operator screen shows:

- A real terminal pane for each UC.
- A compact status tile above each pane.
- A shared command box for known-safe steps.
- A per-UC command box/button for catch-up steps.
- An all-ready indicator before broadcasting the next step.

Example status tiles:

```text
UC-1  192.168.11.188  Root menu              READY
UC-2  192.168.11.189  USB installation menu  READY
UC-3  192.168.11.190  Running M1 update      WAIT 07:42
```

The operator should still be able to see the raw UC screen. The dashboard status
is an assistant, not the source of truth.

## Architecture

```text
Operator PC
  dashboard app
    SSH session UC-1 -> ESP-1:2222 -> UC-1:22
    SSH session UC-2 -> ESP-2:2222 -> UC-2:22
    SSH session UC-3 -> ESP-3:2222 -> UC-3:22
    terminal renderer
    screen recognizer
    step checklist
    ESP mode API client
```

## Screen Recognizer

The recognizer watches the terminal buffer for stable phrases.

Initial useful detections:

```text
Root menu
Root menu > system
USB installation
Root menu > pfe
display
version
ssh
device
detection card
system card
printer
Mauvais numero ou action
Remote side unexpectedly closed network connection
```

Each UC gets a state:

```text
unknown
login
root_menu
system_menu
usb_install_menu
update_running
pfe_menu
version_check
device_menu
card_detection
system_card
printer_check
disconnected
error
```

## Step Gate

Each procedure step defines the expected screen before and after the command.

Example:

```text
Step: enter system menu
Send: 2
Before: root_menu
After: system_menu
```

The dashboard should show:

- green when a UC is at the expected screen,
- yellow when it is busy or not yet caught up,
- red when it is disconnected or shows an error.

Broadcasting should be a deliberate button press. If not all UCs are ready, the
button should warn and require confirmation.

## Implementation Path

1. Keep `uc-split-console.cmd` as the immediate clean terminal workflow.
2. Build a proper dashboard that owns the SSH sessions instead of opening
   external terminal panes.
3. Use a real terminal renderer so clear-screen and menu drawing behave
   correctly.
4. Add the screen recognizer from the actual UC phrases we observe during dry
   runs.
5. Add ESP mode buttons using each ESP HTTP API:
   - `POST /mode/off`
   - `POST /mode/m0`
   - `POST /mode/m1`
6. Add a run log so we know exactly what was sent to each UC and when.

## Recommended Stack

Use a small local web app:

- Node.js backend
- `ssh2` for SSH sessions
- `node-pty` only if we decide to keep using `plink`
- `xterm.js` for real terminal panes
- a lightweight screen recognizer that reads each terminal buffer

This avoids fighting Windows Forms text boxes and gives us a browser-based
operator console that can scale from one UC to three UCs cleanly.

## Current Prototype

Start the local cockpit:

```powershell
.\scripts\start-uc-cockpit.cmd
```

Then open:

```text
http://127.0.0.1:3100
```

The prototype already provides:

- four editable lanes,
- one password field,
- connect all / disconnect,
- one xterm.js terminal viewport per lane,
- shared command sending to all ready lanes,
- per-lane catch-up sends,
- clear all views and per-lane clear buttons,
- ESP mode buttons for `OFF`, `M1`, and `M0`,
- first-pass screen recognition from terminal text.

Current fieldwork acceptance goals:

- Real terminal viewports, not fake text.
- One command bar.
- Lane checkboxes: only `ready` lanes receive `Send Ready`.
- Per-lane catch-up command.
- Clear/resync visual button.
- Password preloaded once per browser session.
- ESP mode controls beside each ESP lane.
