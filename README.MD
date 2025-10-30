# rpi-dpidac — Raspberry Pi 5 DPI→VGA DAC bridge

Advanced DRM bridge and device-tree overlay for driving VGA over the RP1 DPI (Pi 5). Provides a stable EDID and selectable RGB DPI bus formats.

---

## System requirements

Tested in:

* Raspberry Pi OS Lite (64‑bit)
* Release date: 2025‑10‑30
* Last updated: 2025‑10‑30
* Kernel: 6.12.x
* Debian 12 (bookworm)
* Raspberry Pi 5B/500

---

## Features

* **EDID**

  * Base EDID exposes classic PC **Established Timings**: 1024×768@75/70/60, 800×600@75/60, 640×480@75/60, 720×400@70.
  * CTA‑861 block advertises TV modes: **1080p60/50**, **720p60/50**.
  * For VGA sinks that ignore CTA on analog, the driver injects 1080p/720 fixed modes at probe so they still appear.
* **Preferred mode selection**

  * Choose by overlay (`mode=`) or module param (`pref_mode=`). Accepts `WxH@R`, `WxH`, or `1080p60`/`720p50` style.
  * Default preferred 1080p60 is implemented via DRM flag.
* **RGB DPI bus formats**

  * `rgb565`, `rgb565-padhi`, `bgr666`, `bgr666-padhi`, `rgb666-padhi`, `bgr888`, `rgb888`.
* **Force an exact timing**

  * `force_mode=` bypasses EDID and exposes exactly one mode (e.g. porch/polarity experiments).

---

## Quick start

1. Install system updates (recommended):

```bash
sudo apt update
sudo apt full-upgrade
sudo rpi-eeprom-update -a
sudo reboot
```

2. Build and install using DKMS (recommended for upgrades)

* Place sources under `/usr/src/rpi-dpidac-1.0/`:

  * `rpi-dpidac.c`, minimal `Makefile` (`obj-m += rpi-dpidac.o`), `dkms.conf`.
* Or use the provided helper script `dkms_rpi_dpidac.sh 1.0` which copies into `/usr/src`, then runs `dkms add/build/install`.

Verify:

```bash
dkms status
modinfo -n rpi-dpidac   # should point to /updates/dkms
```

3. Deploy the overlay

```bash
dtc -@ -O dtb -o vc4-kms-dpi-dpidac.dtbo vc4-kms-dpi-dpidac.dts
sudo cp vc4-kms-dpi-dpidac.dtbo /boot/firmware/overlays/
```

4. Enable in `/boot/firmware/config.txt`

```ini
dtoverlay=vc4-kms-dpi-dpidac
```

Reboot to apply the overlay.

---

## Development: fast local build and reload

Build without DKMS:

```bash
make -j"$(nproc)" -C /lib/modules/$(uname -r)/build M="$PWD" modules
```

Reload:

```bash
sudo modprobe -r rpi-dpidac 2>/dev/null || true
sudo insmod ./rpi-dpidac.ko pref_mode=1024x768@60 busfmt=rgb888
# or
sudo modprobe rpi-dpidac pref_mode=1024x768@60 busfmt=rgb888
```

If unload fails, stop the display manager, then retry.

Recompile and copy the overlay if the DTS changed, then reboot to apply DT changes:

```bash
dtc -@ -O dtb -o vc4-kms-dpi-dpidac.dtbo vc4-kms-dpi-dpidac.dts
sudo cp vc4-kms-dpi-dpidac.dtbo /boot/firmware/overlays/
```

---

## Overlay parameters

Overlay name: **`vc4-kms-dpi-dpidac`**

* **RGB bus format** (one of):

  * `rgb565`, `rgb565-padhi`, `bgr666`, `bgr666-padhi`, `rgb666-padhi`, `bgr888`, `rgb888`
* **Preferred mode**:

  * `mode=<string>` where `<string>` is `WxH@R`, `WxH`, or `1080p60`/`720p50`.
* Optional low-level properties if needed:

  * `bus-format=<hex>` and `bus-flags=<hex>` (advanced users)

Examples:

```ini
# 24‑bit RGB, 1080p50 preferred
dtoverlay=vc4-kms-dpi-dpidac,rgb888,mode=1080p50

# 24‑bit padded 18‑bit, 1024×768@60 preferred
dtoverlay=vc4-kms-dpi-dpidac,rgb666-padhi,mode=1024x768@60
```

---

## Module parameters

* `pref_mode=<string>`
  Preferred mode flag applied after enumeration. Same formats as `mode=`. Example: `pref_mode=1024x768@60`.

* `busfmt=<enum>`
  Same choices as overlay RGB convenience names. Example: `busfmt=bgr666-padhi`.

* `force_mode="<video string>"`
  Override everything and publish one mode. Accepts kernel `video=` syntax, e.g.:
  `force_mode="1024x768@60 -hsync -vsync"`.

Examples:

```bash
sudo modprobe rpi-dpidac pref_mode=1080p50 busfmt=rgb888
sudo modprobe rpi-dpidac force_mode="1024x768@60 -hsync -vsync"
```

---

## EDID details

* **Base EDID**: Analog input, gamma 2.2, Established Timings set for:

  * 1024×768@75/70/60, 800×600@75/60, 640×480@75/60, 720×400@70.
* **CTA‑861 extension**: SVDs for 1080p60/50 and 720p60/50. One may be marked native if chosen via `mode=`/`pref_mode=`.
* **Analog caveat**: some stacks ignore CTA video blocks on VGA. The driver adds 1080p/720 as fixed timings after EDID parse so they are still offered.

Dump and decode EDID:

```bash
CONNECT=$(ls /sys/class/drm | grep -E 'card[0-9]-DPI|card[0-9]-VGA' | head -n1)
cat /sys/class/drm/$CONNECT/edid > edid.bin
edid-decode edid.bin
```

---

## Troubleshooting

Logs:

```bash
dmesg | grep -i dpidac
```

List DRM connectors and modes:

```bash
modetest -c -p -M vc4
```

Confirm the loaded object:

```bash
modinfo -n rpi-dpidac
```

If you previously installed a non‑DKMS copy, remove it to avoid duplication and run `depmod`.

---

## Naming

* Overlay: **vc4-kms-dpi-dpidac**
* Module: **rpi-dpidac**

---

## License and authors

* License: GPL‑2.0‑or‑later
* Authors: Hugh Cole‑Baker, cpasjuste, Ruben Tomas Alonso (RTA)
