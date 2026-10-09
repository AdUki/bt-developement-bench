# The system: image, storage, networking

What `btbench-base`, `btbench-image` and `btbench-rpi.wks` make of a booted board. The interfaces
other parts rely on are in [contracts.md](contracts.md).

## The SD card

| Partition | Size | Mounted | What |
|---|---|---|---|
| `boot` (FAT) | ≥ 256 MB | `/boot`, read-write | firmware, U-Boot, the good kernel; trial kernels in `/boot/bench/{test,prev}`, U-Boot's `uboot.env` |
| `root` (ext4) | packages + 512 MB | `/`, read-write | the image; deploys rsync into it |
| `data` (ext4) | rest of the card | `/data` | the bench's state |

The root is found by `PARTUUID=b7be4c4e-02`: the image recipe fixes the disk identifier. `/boot`
and `/data` are mounted by label with `nofail`: a damaged one costs you its function, not the board.

`/data`:

- `btbench/` — settings: `audio-mode`, `bluetoothd.env`, `wifi/` (networks, policy)
- `bluetooth/` — BlueZ's pairings, bind-mounted over `/var/lib/bluetooth`
- `btsnoop/` — the HCI capture ring

A reflash rewrites `/data` too. Deploys and kernel swaps leave it alone.

## First boot

1. `btbench-data` grows the data partition and its filesystem to the end of the card, then creates
   the directories above. Later boots find nothing to grow.
2. `sshdgenkeys` makes the ssh host keys (a new set with every flash; the Makefile does not check
   them).
3. `btbench-wifi` copies the network from the device conf to `/data/btbench/wifi/`, if one was
   given, and tries it; otherwise it opens the setup AP.

Root logs in with the device conf's password, or with its ssh key (`BTBENCH_SSH_PUBKEY`). The
journal is persistent (64 MB cap): `journalctl -b -1` reads the boot before this one.

## The USB link

Plug the Zero W's **USB** port (not PWR) into the PC. It is one composite USB device:

- **Ethernet**: NCM (`BTBENCH_GADGET=ncm`) or ECM (`ecm`, for hosts without NCM). The board is
  `10.55.0.1` and gives the PC an address from `10.55.0.2–9` over DHCP, with no router and no DNS,
  so the PC keeps its own default route and resolver. On Linux the interface shows up as
  `enx06bb…`. Its MAC comes from the board's serial number, so NetworkManager keeps the same
  "Wired connection" profile across reboots and reflashes; nothing needs configuring. If
  NetworkManager is set to ignore unknown devices, `nmcli device set <iface> managed yes`.
  The link answers about 45 s after power-on, not when the PC first gets its lease:
  btbench-gadget-reconnect.service connects once more 15 s after boot finishes, because with
  `KERNEL=next` the first connection stops receiving at about that moment (the PC then logs
  `transmit queue 0 timed out` for its `enx…` interface). If the link dies that way later,
  `btbench-gadget reconnect` on the serial console brings it back.
- **Serial console**: `/dev/ttyACM0` on the PC (`picocom /dev/ttyACM0`), a login on `ttyGS0`.
  It works when the network does not, e.g. a trial kernel without a network.

`ssh root@10.55.0.1`, or `ssh root@<hostname>.local` (mDNS from systemd-resolved, on the USB link
and Wi-Fi).

`BTBENCH_GADGET=off`, or a board without a device controller, leaves the port alone: on the Zero W
that keeps it a USB host, for a Bluetooth dongle, and the board is reachable over Wi-Fi only.

## Wi-Fi

`btbench-wifi` (service `btbench-wifi`) runs Wi-Fi as either a client or an AP, never both: the
BCM43430 does not do both reliably.

With the default policy `auto`:

1. **Client.** With networks stored, `wpa_supplicant@wlan0` gets 45 s to join one of them and get
   an address.
2. **Setup AP.** With no networks, or none answering, the board scans once, then opens
   `btbench-XXXX` (the last 4 hex digits of its serial; WPA2, password `BTBENCH_AP_PSK`, channel 6).
   It is `10.42.0.1` and runs a DHCP server. Every DNS name resolves to the board, so a phone that
   joins is sent to its page, where you can add a network. The scan from before the AP came up
   is what `btbench-wifi scan` shows while the AP is up.
3. **Retry.** While the AP is up, the board tries the stored networks again every 10 min, and at
   once when one is added. If that fails it comes back to the AP and says why in `last_error`.
4. **Lost network.** A client that has lost its network for 5 min opens the AP.

```
btbench-wifi status                       # JSON: state, policy, ssid, ip, ap, networks, last_error
btbench-wifi scan
btbench-wifi add "Lab WiFi" 'passphrase'  # or no passphrase for an open network
btbench-wifi remove "Lab WiFi"
btbench-wifi mode auto|sta|ap|off         # kept across reboots
```

`sta` keeps trying as a client and never opens the AP. `ap` holds the AP. `off` powers the
interface down, e.g. to measure Bluetooth without Wi-Fi coexistence (stay on the USB link for
that).

The networks are kept in `/data/btbench/wifi/wpa_supplicant-wlan0.conf`, with SSIDs in hex and
keys derived. Use `add` and `remove` rather than editing the file. `country=` follows
`BTBENCH_WIFI_COUNTRY`.

## Bluetooth address and pairings

`btbench-bdaddr` runs before every `bluetoothd` start (it is pulled in by `bluetooth.service`).

- **Pairings.** It bind-mounts `/data/bluetooth` over `/var/lib/bluetooth`.
- **Address.** It checks the controller's address. The BCM43430A1 has no address programmed: it
  reports `43:43:A1:12:1F:AC`, or `AA:AA:AA:AA:AA:AA`, which mainline treats as "unconfigured". In
  either case the script sets `B8:27:EB:xx:xx:xx` with `btmgmt public-addr`, derived from the
  serial number the way Raspberry Pi OS's `bthelper` does. Any other address is left alone.

## Changing the scripts

The sources are in `tools/target/`. `make deploy-scripts` copies them to the board's `/usr/bin`
without restarting anything; the next `make image` carries them too.
