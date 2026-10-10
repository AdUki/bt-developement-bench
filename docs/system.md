# The system: image, storage, networking

What `btbench-base`, `btbench-image` and `btbench-rpi.wks` make of a booted board. The interfaces
other parts rely on are in [contracts.md](contracts.md).

## The SD card

| Partition | Size | Mounted | What |
|---|---|---|---|
| `boot` (FAT) | ≥ 256 MB | `/boot`, **read-only** | firmware, U-Boot, the good kernel; trial kernels in `/boot/bench/{test,prev}`, U-Boot's `uboot.env` |
| `root` (ext4) | packages + 512 MB | `/`, **read-only** | the image; deploys rsync into it, opened for that |
| `data` (ext4) | rest of the card | `/data`, read-write | settings, and nothing else |

The root is found by `PARTUUID=b7be4c4e-02`: the image recipe fixes the disk identifier. `/boot`
and `/data` are mounted by label with `nofail`: a damaged one costs you its function, not the board.

The card is read-only except for settings. A card that is not written does not wear out, and a
power cut (the usual way a bench is switched off) cannot corrupt the root or `/boot`.
- **The root and `/boot` are mounted read-only.** The root uses Yocto's `read-only-rootfs`;
  volatile-binds give `/var/lib`, `/var/cache`, `/var/spool` and `/srv` a copy in RAM, made at
  boot, and `/var/log` is a link into `/var/volatile`. opkg's database is in `/usr/lib/opkg`, on
  the root, so what `make pkg` installs stays known to opkg.
- **`/data` holds settings only, written when one changes:**
  - `btbench/`: Setup's files, `bluetoothd.env`, `btsnoop.env`, the HID and radio
    settings, `wifi/` (networks), `ssh/` (sshd's host keys), `wireplumber-profiles.conf`;
  - `btbench/timesync/`: timesyncd's clock, one small write per NTP sync, so a board without a
    network does not start at the image's build date;
  - `bluetooth/`: BlueZ's pairings, bind-mounted over `/var/lib/bluetooth`.
- **What changes often is in RAM,** and gone at reboot:
  - the journal (`/run/log/journal`, uncompressed, 32 MB);
  - the per-program logs (`/tmp/logs`);
  - the HCI capture ring (`/tmp/btsnoop`, 4 × 8 MB; download a file to keep it);
  - OBEX transfers (`/tmp/btbench/obex`);
  - BlueZ's device cache (each adapter's `cache/`, bound to `/run/btbench/bluetooth-cache`);
  - the rest of `/var`.

Settings that have to be in `/etc` are links to `/data`
(`/etc/wireplumber/wireplumber.conf.d/60-btbench-profiles.conf`), or are read from a fixed path in
`/run` (sshd's `HostKey /run/btbench/ssh/…`, which `btbench-sshkeys` points at `/data/btbench/ssh`,
or at `/run` for that boot if `/data` did not mount).

**Writing the root or `/boot`:** `btbench-rw open / /boot` remounts them read-write, and
`btbench-rw close` remounts them read-only. Every writer goes through it:
- `make deploy-*`, `make pkg` and `make linux-deploy`, through the Makefile's `board_rw`; the
  close runs in an EXIT trap, so a failed deploy closes too;
- `btbench-kernel` (`try`, `commit`, `rollback` open `/boot`; `prune` opens `/`);
- `btbench-btdebug keep` (its modprobe.d file).

Uses are counted, so overlapping ones do not close each other's mounts. `btbench-rw status` shows
the state; `btbench-rw reset` closes everything after a killed deploy, and so does a reboot.

The kernel will not remount read-only while a process still runs a file that a deploy replaced:
rsync renames the new copy over the old one, which lives on, deleted, until its process ends.
The close then names the units (after `make deploy-scripts`, usually `btbench-wifi.service`) and
leaves the root writable. Restart them and run `btbench-rw reset`, or reboot. Deploys that restart
what they replace (bluetooth, PipeWire, btbenchd) are not affected.

A reflash rewrites `/data` too. Deploys and kernel swaps leave it alone.

## First boot

1. `btbench-data` grows the data partition and its filesystem to the end of the card, then creates
   the directories above and binds timesyncd's clock to `/data`. Later boots find nothing to grow.
   `btbench-sshkeys` makes sshd's host keys in `/data/btbench/ssh` the first time.
2. `sshdgenkeys` makes the ssh host keys (a new set with every flash; the Makefile does not check
   them).
3. `btbench-wifi` copies the network from the device conf to `/data/btbench/wifi/`, if one was
   given, and tries it; otherwise it opens the setup AP.

Root logs in with the device conf's password, or with its ssh key (`BTBENCH_SSH_PUBKEY`). The
journal is in RAM, uncompressed (32 MB cap): reading it costs the Zero W a fraction of a second
instead of seconds, and the SD card no writes, but it starts empty at every boot.

## The USB link

Plug the Zero W's **USB** port (not PWR) into the PC. It is one composite USB device:

- **Ethernet**: NCM (`BTBENCH_GADGET=ncm`) or ECM (`ecm`, for hosts without NCM). The board is
  `10.55.0.1` and gives the PC an address from `10.55.0.2–9` over DHCP, with no router and no DNS,
  so the PC keeps its own default route and resolver. On Linux the interface shows up as
  `enx06bb…`. Its MAC comes from the board's serial number, so NetworkManager keeps the same
  "Wired connection" profile across reboots and reflashes; nothing needs configuring. If
  NetworkManager is set to ignore unknown devices, `nmcli device set <iface> managed yes`.
- **Serial console**: `/dev/ttyACM0` on the PC (`picocom /dev/ttyACM0`), a login on `ttyGS0`.
  It works when the network does not, e.g. a trial kernel without a network.

`ssh root@10.55.0.1`, or `ssh root@<mac>-<hostname>.local` (mDNS from systemd-resolved, on the USB link
and Wi-Fi). `<mac>` is the last three bytes of the board's Wi-Fi MAC address (`257bfc-btbench`):
every image has the same `/etc/hostname`, and `btbench-hostname.service` puts the MAC in front at
boot, as a transient hostname, so that several boards on one network each have a name of their
own. `BTBENCH_HOSTNAME_MAC = "0"` in the device conf keeps the plain name.

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
