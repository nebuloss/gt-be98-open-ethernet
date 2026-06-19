#!/usr/bin/env bash
# build-initramfs.sh — build a minimal busybox aarch64 initramfs for the
# BE98 QEMU baseline boot loop. RUN ON dev-build, NOT on dev-code.
#
# Produces: ~/qemu-be98/initramfs.cpio.gz
#
# The initramfs is a single static busybox (Debian arm64 busybox-static) plus a
# tiny /init that mounts proc/sys/dev, prints kernel + net state, and drops to a
# shell. It deliberately carries NO vendor firmware/blobs — it is a clean kernel
# sanity rootfs. (The WiFi effort's initramfs-bca.cpio.gz is a different, WiFi-
# specific image; do not conflate the two.)
set -euo pipefail

OUT="${OUT:-$HOME/qemu-be98/initramfs.cpio.gz}"
WORK="${WORK:-$HOME/qemu-be98/initramfs-build}"

mkdir -p "$WORK"/bb
cd "$WORK"/bb

# Need arm64 foreign-arch enabled once: sudo dpkg --add-architecture arm64 && sudo apt-get update
if [ ! -f extract/usr/bin/busybox ]; then
  echo "[*] fetching arm64 busybox-static..."
  rm -f ./*.deb
  apt-get download busybox-static:arm64
  rm -rf extract && mkdir extract
  dpkg-deb -x ./*.deb extract
fi
file extract/usr/bin/busybox | grep -q aarch64 || { echo "busybox not aarch64"; exit 1; }

# Assemble root
ROOT="$WORK/root"
rm -rf "$ROOT"
mkdir -p "$ROOT"/{bin,sbin,proc,sys,dev}
cp extract/usr/bin/busybox "$ROOT"/bin/busybox
chmod +x "$ROOT"/bin/busybox
for a in sh mount mkdir cat ls echo uname ip ifconfig dmesg poweroff sleep; do
  ln -sf busybox "$ROOT/bin/$a"
done

cat > "$ROOT/init" <<'EOF'
#!/bin/busybox sh
/bin/busybox --install -s
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev 2>/dev/null
echo
echo "=========================================="
echo " BE98 QEMU baseline initramfs - kernel up"
echo "=========================================="
uname -a
echo "--- net interfaces ---"
ip link 2>/dev/null || ifconfig -a
echo "--- BE98_BOOT_OK ---"
# Headless/CI: kernel cmdline "be98_auto=poweroff" -> clean exit after marker.
if grep -q "be98_auto=poweroff" /proc/cmdline; then
    poweroff -f
fi
exec /bin/busybox sh
EOF
chmod +x "$ROOT/init"

mkdir -p "$(dirname "$OUT")"
( cd "$ROOT" && find . | cpio -o -H newc 2>/dev/null | gzip -9 ) > "$OUT"
echo "[+] wrote $OUT"
ls -la "$OUT"
