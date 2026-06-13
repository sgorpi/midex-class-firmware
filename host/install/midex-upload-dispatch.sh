#!/bin/sh
# midex-upload-dispatch.sh <usb-kernel-name>
#
# Picks the right class-compliant firmware image by the appearing device's USB
# product (loader) ID and uploads it. Called by midex-upload@.service, whose
# instance name (%i) is the USB device's kernel name (e.g. 1-2) set from the udev
# rule. This is what makes auto-upload revision-aware: the same loader-PID handoff
# serves MIDEX8 r1 and r2 (and, once built, MIDEX3).
set -eu

dev=${1:?usage: $0 <usb-kernel-name>}
sys=/sys/bus/usb/devices/$dev
pid=$(cat "$sys/idProduct" 2>/dev/null || true)
fwdir=/usr/local/lib/midex

case "$pid" in
	1000) img=$fwdir/midex-class-r1.ihx ;;   # MIDEX8 r1 (AN2131)
	1010) img=$fwdir/midex-class-r2.ihx ;;   # MIDEX8 r2 (CY7C646 FX)
	# 1100) img=$fwdir/midex-class-mx3.ihx ;; # MIDEX3 (not built yet)
	*) echo "midex-upload: no class image for product '0a4e:$pid' ($dev)" >&2
	   exit 0 ;;
esac

[ -f "$img" ] || { echo "midex-upload: image $img not installed" >&2; exit 1; }
exec /usr/local/bin/midex-fw-upload -d "0a4e:$pid" "$img"
