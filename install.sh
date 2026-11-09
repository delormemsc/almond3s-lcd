#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Status screen for the Securifi Almond 3S on OpenWrt.
#   wget -qO- https://raw.githubusercontent.com/delormemsc/almond3s-lcd/main/install.sh | sh
#   wget -qO- https://raw.githubusercontent.com/delormemsc/almond3s-lcd/main/install.sh | sh -s uninstall
# The whole script is parsed first (main is called on the last line), so a cut-off
# download does nothing.

BASE=https://raw.githubusercontent.com/delormemsc/almond3s-lcd/main
BOARD=securifi,almond-3s
RELEASE=24.10.5
KERNEL=6.6.119
TMPD=/tmp/almond-lcd-install
KO=/lib/modules/$KERNEL/almond_lcd.ko
CRON='* * * * * /usr/bin/almond-lcd-collect >/dev/null 2>&1'
KEEP='/usr/bin/almond-lcd-status /usr/bin/almond-lcd-collect /usr/bin/almond-lcd-at /usr/bin/almond-lcd-modem /etc/init.d/almond-lcd /etc/rc.d/S99almond-lcd /etc/rc.d/K10almond-lcd'

die() { echo "ERROR: $*" >&2; rm -rf "$TMPD"; exit 1; }

check() {
	[ "$(id -u)" = 0 ] || die "run as root on the router"
	[ -f /etc/openwrt_release ] || die "not OpenWrt"
	. /etc/openwrt_release
	b=$(cat /tmp/sysinfo/board_name 2>/dev/null)
	[ "$b" = "$BOARD" ] || die "board is '$b', this works only on $BOARD"
	[ "$DISTRIB_RELEASE" = "$RELEASE" ] || die "OpenWrt is $DISTRIB_RELEASE, supported: $RELEASE"
	[ "$(uname -r)" = "$KERNEL" ] || die "kernel is $(uname -r), the module is built for $KERNEL"
	for c in flock sha256sum ping wget; do
		command -v "$c" >/dev/null 2>&1 || die "'$c' is missing"
	done
	if [ -c /dev/lcd ] && ! grep -q '^almond_lcd ' /proc/modules; then
		die "/dev/lcd belongs to another LCD driver, remove it first"
	fi
}

get() { # <path in the repository>
	mkdir -p "$TMPD/${1%/*}"
	wget -q -O "$TMPD/$1" "$BASE/$1" ||
		die "cannot download $1 (try: opkg update && opkg install ca-bundle libustream-mbedtls)"
}

put() { # <path in the repository> <target> <mode>
	cp "$TMPD/$1" "$2.new" && chmod "$3" "$2.new" && mv -f "$2.new" "$2" || die "cannot write $2"
}

have_modem() {
	[ "${MODEM:-auto}" != no ] && [ -c /dev/ttyUSB3 ] || return 1
	for d in /sys/bus/usb/devices/*; do
		[ "$(cat "$d/idVendor" 2>/dev/null):$(cat "$d/idProduct" 2>/dev/null)" = 2c7c:0121 ] && return 0
	done
	return 1
}

install_all() {
	check
	rm -rf "$TMPD"
	get SHA256SUMS
	for f in dist/almond_lcd.ko dist/almond-lcd-status dist/almond-lcd-at \
		 files/almond-lcd-collect files/almond-lcd-modem files/almond-lcd.init; do
		get "$f"
	done
	(cd "$TMPD" && sha256sum -c SHA256SUMS >/dev/null 2>&1) || die "checksum mismatch"

	[ -x /etc/init.d/almond-lcd ] && /etc/init.d/almond-lcd stop 2>/dev/null
	mkdir -p "/lib/modules/$KERNEL"
	put dist/almond_lcd.ko "$KO" 644
	put dist/almond-lcd-status /usr/bin/almond-lcd-status 755
	put files/almond-lcd-collect /usr/bin/almond-lcd-collect 755
	put files/almond-lcd.init /etc/init.d/almond-lcd 755

	page="no modem found, modem page is off"
	rm -f /usr/bin/almond-lcd-at /usr/bin/almond-lcd-modem
	if have_modem; then
		if pidof ModemManager >/dev/null 2>&1; then
			# он может держать тот же порт: чужую связь не трогаем
			page="ModemManager is running, modem page is off (disable it and run the installer again to enable)"
		else
			put dist/almond-lcd-at /usr/bin/almond-lcd-at 755
			put files/almond-lcd-modem /usr/bin/almond-lcd-modem 755
			page="built-in Quectel EC21 found, modem page is on"
		fi
	fi

	touch /etc/crontabs/root
	grep -qF almond-lcd-collect /etc/crontabs/root || echo "$CRON" >> /etc/crontabs/root
	/etc/init.d/cron enable
	/etc/init.d/cron restart >/dev/null 2>&1

	for f in $KEEP; do
		grep -qxF "$f" /etc/sysupgrade.conf 2>/dev/null || echo "$f" >> /etc/sysupgrade.conf
	done

	/etc/init.d/almond-lcd enable
	/etc/init.d/almond-lcd start
	sleep 3
	if ! grep -q '^almond_lcd ' /proc/modules || [ ! -c /dev/lcd ]; then
		uninstall_all
		die "the display module did not load, everything was removed again"
	fi
	rm -rf "$TMPD"
	echo "Done: the screen is running. $page."
	echo "Tap or swipe: next page. Hold about a second: dark/light theme."
}

uninstall_all() {
	[ -x /etc/init.d/almond-lcd ] && { /etc/init.d/almond-lcd stop 2>/dev/null; /etc/init.d/almond-lcd disable 2>/dev/null; }
	grep -q '^almond_lcd ' /proc/modules && rmmod almond_lcd
	rm -f "$KO" /usr/bin/almond-lcd-status /usr/bin/almond-lcd-collect /usr/bin/almond-lcd-at \
	      /usr/bin/almond-lcd-modem /etc/init.d/almond-lcd /etc/almond-lcd.theme
	if [ -f /etc/crontabs/root ]; then
		sed -i '/almond-lcd-collect/d' /etc/crontabs/root
		/etc/init.d/cron restart >/dev/null 2>&1
	fi
	[ -f /etc/sysupgrade.conf ] && sed -i '/almond-lcd/d' /etc/sysupgrade.conf
	echo "Removed."
}

main() {
	case "${1:-install}" in
		install) install_all ;;
		uninstall) uninstall_all ;;
		*) die "usage: install.sh [install|uninstall]" ;;
	esac
}

main "$@"
