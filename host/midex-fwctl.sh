#!/bin/sh
# midex-fwctl.sh - install and switch the MIDEX firmware path (class vs stock).
#
#   install     copy the uploader, firmware, udev rule and systemd unit into place
#   class       use the class-compliant firmware (our uploader; blacklist stock driver)
#   stock       use the original Steinberg firmware (in-tree snd_usb_midex driver)
#   status      show what is installed and which path is active
#   uninstall   remove everything this script installed
#
# Run as root. After `class`/`stock`, power-cycle (or re-plug) the MIDEX so it
# re-enters its loader PID and the chosen path claims it.
set -eu

# --- install locations -------------------------------------------------------
BIN_DST=/usr/local/bin/midex-fw-upload
FW_DST=/usr/local/lib/midex/midex-class-r1.ihx
RULE_DST=/etc/udev/rules.d/99-midex-class.rules
UNIT_DST=/etc/systemd/system/midex-upload@.service
BLACKLIST_DST=/etc/modprobe.d/midex-class.conf
STOCK_MODULE=snd_usb_midex

# --- sources (relative to this script) ---------------------------------------
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BIN_SRC=$HERE/build/midex-fw-upload
FW_SRC=$HERE/../firmware/midex-class-r1.ihx
RULE_SRC=$HERE/install/99-midex-class.rules
UNIT_SRC=$HERE/install/midex-upload@.service
BLACKLIST_SRC=$HERE/install/midex-class-blacklist.conf

need_root() {
	[ "$(id -u)" -eq 0 ] || { echo "error: run as root (sudo $0 $1)" >&2; exit 1; }
}

reload_udev() { udevadm control --reload && udevadm trigger --subsystem-match=usb; }

cmd_install() {
	need_root install
	[ -f "$BIN_SRC" ] || { echo "error: $BIN_SRC not built (run: cmake -S . -B build && cmake --build build)" >&2; exit 1; }
	[ -f "$FW_SRC" ]  || { echo "error: $FW_SRC not built (run: make -C ../firmware class)" >&2; exit 1; }
	install -Dm755 "$BIN_SRC"  "$BIN_DST"
	install -Dm644 "$FW_SRC"   "$FW_DST"
	install -Dm644 "$RULE_SRC" "$RULE_DST"
	install -Dm644 "$UNIT_SRC" "$UNIT_DST"
	systemctl daemon-reload
	reload_udev
	echo "installed. Now choose a firmware path:  $0 class   |   $0 stock"
}

cmd_class() {
	need_root class
	[ -f "$BIN_DST" ] || { echo "error: not installed yet (run: $0 install)" >&2; exit 1; }
	install -Dm644 "$BLACKLIST_SRC" "$BLACKLIST_DST"
	systemctl unmask midex-upload@.service 2>/dev/null || true
	modprobe -r "$STOCK_MODULE" 2>/dev/null || true
	reload_udev
	echo "class path active. Power-cycle the MIDEX to upload the class firmware."
}

cmd_stock() {
	need_root stock
	rm -f "$BLACKLIST_DST"
	# Mask the upload unit so a stray udev trigger cannot re-flash the device.
	systemctl mask midex-upload@.service 2>/dev/null || true
	reload_udev
	modprobe "$STOCK_MODULE" 2>/dev/null || true
	echo "stock path active. Power-cycle the MIDEX; snd_usb_midex will claim it."
}

cmd_status() {
	echo "installed files:"
	for f in "$BIN_DST" "$FW_DST" "$RULE_DST" "$UNIT_DST"; do
		[ -e "$f" ] && echo "  [x] $f" || echo "  [ ] $f"
	done
	echo "active path:"
	if [ -e "$BLACKLIST_DST" ]; then
		echo "  class  (snd_usb_midex blacklisted, upload unit armed)"
	else
		echo "  stock  (snd_usb_midex allowed)"
	fi
	systemctl is-enabled midex-upload@.service >/dev/null 2>&1 || true
	masked=$(systemctl is-enabled midex-upload@.service 2>/dev/null || true)
	echo "  midex-upload@.service: ${masked:-not installed}"
	echo "connected MIDEX:"
	lsusb -d 0a4e: 2>/dev/null | sed 's/^/  /' || echo "  (lsusb unavailable)"
}

cmd_uninstall() {
	need_root uninstall
	systemctl unmask midex-upload@.service 2>/dev/null || true
	rm -f "$BIN_DST" "$FW_DST" "$RULE_DST" "$UNIT_DST" "$BLACKLIST_DST"
	rmdir /usr/local/lib/midex 2>/dev/null || true
	systemctl daemon-reload
	reload_udev
	echo "uninstalled. (snd_usb_midex left as-is; modprobe it if you want stock.)"
}

case "${1:-}" in
	install)   cmd_install   ;;
	class)     cmd_class     ;;
	stock)     cmd_stock     ;;
	status)    cmd_status    ;;
	uninstall) cmd_uninstall ;;
	*) echo "usage: $0 {install|class|stock|status|uninstall}" >&2; exit 2 ;;
esac
