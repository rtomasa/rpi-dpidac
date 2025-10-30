#!/usr/bin/env bash
set -euo pipefail

NAME="rpi-dpidac"
VER="${1:-1.0}"         # version = folder & dkms.conf version
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
DST="/usr/src/${NAME}-${VER}"

sudo rm -rf "$DST"
sudo install -d "$DST"
# Copy the files DKMS needs
sudo install -m 0644 "$SRC_DIR/rpi-dpidac.c" "$DST/"
sudo install -m 0644 "$SRC_DIR/Makefile"     "$DST/"
sudo install -m 0644 "$SRC_DIR/dkms.conf"    "$DST/"

# Re-add/build/install
sudo dkms remove -m "$NAME" -v "$VER" --all >/dev/null 2>&1 || true
sudo dkms add    -m "$NAME" -v "$VER"
sudo dkms build  -m "$NAME" -v "$VER"
sudo dkms install -m "$NAME" -v "$VER"

echo "DKMS status:"
dkms status | grep -E "^${NAME}/|^${NAME}:" || true

# Optional: build & install overlay if present (set INSTALL_DTBO=1 to enable)
if [[ "${INSTALL_DTBO:-0}" = "1" && -f "$SRC_DIR/vc4-kms-dpi-dpidac.dts" ]]; then
  DTBO="vc4-kms-dpi-dpidac.dtbo"
  TMP="$(mktemp -d)"
  dtc -@ -O dtb -o "$TMP/$DTBO" "$SRC_DIR/vc4-kms-dpi-dpidac.dts"
  for d in /boot/firmware/overlays /boot/overlays; do
    if [[ -d "$d" ]]; then
      sudo install -m 0644 "$TMP/$DTBO" "$d/"
      echo "Installed $DTBO to $d"
      break
    fi
  done
  rm -rf "$TMP"
fi
