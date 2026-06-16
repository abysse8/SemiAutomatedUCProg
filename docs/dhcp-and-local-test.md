# DHCP and Local Test Plan

## Local Behavior Test

Start the cockpit:

```powershell
.\scripts\start-uc-cockpit.cmd
```

Open:

```text
http://127.0.0.1:3100
```

Use `Local Sim` to point the three ESP lanes at built-in simulated ESP
endpoints:

```text
http://127.0.0.1:3100/sim/esp1
http://127.0.0.1:3100/sim/esp2
http://127.0.0.1:3100/sim/esp3
```

Then test:

- `M1`
- `M0`
- `OFF`
- `Check`

This validates cockpit button behavior and feedback without the ESP32 or UC.

## Avoiding DHCP Issues

Best field setup: give each ESP a stable Wi-Fi address on the site router.

Use DHCP reservations on the RUT241 or site router:

```text
ESP32 #1 -> 192.168.11.188
ESP32 #2 -> 192.168.11.189
ESP32 #3 -> 192.168.11.190
```

Reserve by ESP Wi-Fi MAC address. The ESP logs print the MAC and IP at boot.

Why DHCP reservation is preferred:

- the ESP firmware can stay identical on all three devices,
- the router prevents address conflicts,
- the cockpit can keep fixed lane URLs,
- moving devices between kits is easier to diagnose.

Fallbacks:

- Use the ESP fallback AP at `192.168.4.1` for one device at a time.
- Add static Wi-Fi IPs in firmware only if the router cannot reserve leases.
- Add discovery later by scanning the subnet for ESP `/mode` endpoints.

## Switch-Based Network View

When the maintenance computer is on the same maintenance network or commutator
side as the equipment, use the cockpit Network View:

```text
192.168.11.0/24
```

Click `Scan Network`. The cockpit uses `nmap` when installed. If `nmap` is not
available, it falls back to ping, ARP, TCP service probes, and ESP `/mode`
enrichment.

The table is meant to identify everything visible on the switch:

- UCs,
- ESP32 UC proxies,
- validators,
- peripheral devices,
- printers,
- web devices,
- industrial devices.

Repeated scans can be enabled with `Repeat On`. Each scan is written to the
audit log.

ESP proxy enrichment probes:

```text
http://<ip>/mode
```

Discovered ESP proxies can be applied to the ESP lanes with `Apply Found` or
the per-row `Use ESP` button.

If only one known device is being checked, a narrow range is faster:

```text
192.168.11.184/29
```

Verified example from the bench network:

```text
192.168.11.188 -> mode M1, usb_active true, ssh_port 2222
```

## Audit Log

Every cockpit server run writes an audit JSONL file under:

```text
logs/
```

The audit captures:

- terminal output,
- connect/disconnect events,
- commands sent by the cockpit,
- ESP scans,
- ESP mode/status actions.

Use the `Audit` button in the cockpit to open the active audit file.

## Real Field Lane Defaults

```text
Maintenance PC direct UC:
  host: 192.168.1.100
  port: 22
  login: sdk_system

ESP32 #1:
  SSH host: 192.168.11.188
  SSH port: 2222
  ESP URL: http://192.168.11.188

ESP32 #2:
  SSH host: 192.168.11.189
  SSH port: 2222
  ESP URL: http://192.168.11.189

ESP32 #3:
  SSH host: 192.168.11.190
  SSH port: 2222
  ESP URL: http://192.168.11.190
```
