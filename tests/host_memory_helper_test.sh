#!/bin/bash
# Test of the launcher's root helper (src/launcher/host/kyty-host-memory) without root and without touching the
# system: --root writes the files under a temporary directory and skips every system command.
# usage: host_memory_helper_test.sh <helper> <hostMemoryPolicy.h> <hostMemoryProbe.h>
set -u
HELPER=$1
POLICY_H=$2
PROBE_H=$3
FAILS=0
check() { if ! eval "$2"; then echo "FAIL: $1"; FAILS=$((FAILS + 1)); fi; }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

mk_root() {   # a 16 GB laptop: zram + a disk swapfile, zswap off
	local r="$1"
	mkdir -p "$r/proc" "$r/sys/module/zswap/parameters" "$r/sys/block/zram0" "$r/run/systemd/system" "$r/usr/lib/systemd/system-generators"
	printf 'MemTotal: 15387372 kB\nMemAvailable: 9437184 kB\n' > "$r/proc/meminfo"
	printf 'Filename Type Size Used Priority\n/dev/zram0 partition 15387368 1024 100\n/swap/swapfile file 16252928 0 -2\n' > "$r/proc/swaps"
	echo N > "$r/sys/module/zswap/parameters/enabled"
	echo lz4 > "$r/sys/module/zswap/parameters/compressor"
	echo 20 > "$r/sys/module/zswap/parameters/max_pool_percent"
	echo N > "$r/sys/module/zswap/parameters/shrinker_enabled"
	echo "2147483648 700000000 800000000 0 800000000 0 0 0 0" > "$r/sys/block/zram0/mm_stat"
	: > "$r/usr/lib/systemd/system-generators/zram-generator"
}

R="$T/root"
mk_root "$R"
UNIT="$R/etc/systemd/system/kyty-host-memory.service"
BOOT="$R/var/lib/kyty-host-memory/boot.sh"
STATE="$R/var/lib/kyty-host-memory/state"
DROPIN="$R/etc/systemd/zram-generator.conf.d/99-kyty-host-memory.conf"

# 1. dry run writes nothing and says what it would do
OUT=$("$HELPER" apply --zswap-percent 21 --compressor zstd --disable-zram --dry-run --root "$R" 2>&1); RC=$?
check "dry run succeeds" "[ $RC = 0 ]"
check "dry run prints the unit" "grep -q 'would write .*kyty-host-memory.service' <<<\"\$OUT\""
check "dry run prints the zram drop-in" "grep -q 'zram-size = 0' <<<\"\$OUT\""
check "dry run writes nothing" "[ ! -e '$UNIT' ] && [ ! -e '$BOOT' ] && [ ! -e '$DROPIN' ]"

# 2. apply with a swapfile and zram off
OUT=$("$HELPER" apply --zswap-percent 21 --compressor zstd --disable-zram --swapfile-mib 8192 --root "$R" 2>&1); RC=$?
check "apply succeeds ($RC)" "[ $RC = 0 ]"
check "unit installed and runs boot.sh" "grep -q '^ExecStart=/bin/sh /var/lib/kyty-host-memory/boot.sh' '$UNIT' && grep -q 'WantedBy=multi-user.target' '$UNIT' && grep -q 'Type=oneshot' '$UNIT'"
check "boot.sh sets the zswap parameters" "grep -q 'echo zstd > \$P/compressor' '$BOOT' && grep -q 'echo 21 > \$P/max_pool_percent' '$BOOT' && grep -q 'echo Y > \$P/enabled' '$BOOT'"
check "boot.sh turns on the swapfile before zram goes off" "grep -n swapon '$BOOT' | head -1 | cut -d: -f1 | { read a; b=\$(grep -n 'swapoff' '$BOOT' | head -1 | cut -d: -f1); [ \"\$a\" -lt \"\$b\" ]; }"
check "boot.sh swaps zram off only with a disk swap present" "grep -q 'zram swap only goes off when a disk swap' '$BOOT'"
check "boot.sh is valid sh" "sh -n '$BOOT'"
check "zram drop-in disables zram0" "grep -q '^\[zram0\]' '$DROPIN' && grep -q '^zram-size = 0' '$DROPIN'"
check "state records the previous zswap settings" "grep -q '^prev_zswap_enabled=N' '$STATE' && grep -q '^prev_zswap_compressor=lz4' '$STATE' && grep -q '^prev_zswap_pool=20' '$STATE' && grep -q '^prev_zswap_shrinker=N' '$STATE'"
check "state records the swapfile and the zram device" "grep -q '^swapfile=/var/lib/kyty-host-memory/swap/swapfile' '$STATE' && grep -q '^zram_disabled=zram0' '$STATE'"
check "nothing outside the root was written" "[ -z \"\$(find '$T' -mindepth 1 -maxdepth 1 ! -name root)\" ]"

# 3. a second apply keeps the first record as the "previous" state
echo Y > "$R/sys/module/zswap/parameters/enabled"; echo zstd > "$R/sys/module/zswap/parameters/compressor"
"$HELPER" apply --zswap-percent 25 --root "$R" >/dev/null 2>&1
check "second apply keeps the saved previous state" "grep -q '^prev_zswap_enabled=N' '$STATE' && grep -q '^prev_zswap_compressor=lz4' '$STATE'"
check "second apply updates the boot settings" "grep -q 'echo 25 > \$P/max_pool_percent' '$BOOT'"

# 4. undo removes everything
OUT=$("$HELPER" undo --root "$R" 2>&1); RC=$?
check "undo succeeds" "[ $RC = 0 ]"
check "undo removes the unit, drop-in, boot.sh and state" "[ ! -e '$UNIT' ] && [ ! -e '$DROPIN' ] && [ ! -e '$BOOT' ] && [ ! -e '$STATE' ]"
OUT=$("$HELPER" undo --root "$R" 2>&1); RC=$?
check "a second undo is a no-op" "[ $RC = 0 ] && grep -q 'nothing to undo' <<<\"\$OUT\""

# 5. zswap only (no zram change): no drop-in
R2="$T/root2"; mk_root "$R2"
"$HELPER" apply --zswap-percent 21 --root "$R2" >/dev/null 2>&1
check "zswap only: no zram drop-in and no zram line in boot.sh" "[ ! -e '$R2/etc/systemd/zram-generator.conf.d/99-kyty-host-memory.conf' ] && ! grep -q swapoff '$R2/var/lib/kyty-host-memory/boot.sh'"

# 6. a host without zram-generator: zram goes off at boot through the unit instead
R3="$T/root3"; mk_root "$R3"; rm "$R3/usr/lib/systemd/system-generators/zram-generator"
"$HELPER" apply --zswap-percent 21 --disable-zram --root "$R3" >/dev/null 2>&1
check "no zram-generator: boot.sh switches zram off" "grep -q swapoff '$R3/var/lib/kyty-host-memory/boot.sh'"

# 6b. zram that is off now but configured in zram-generator: the drop-in covers every configured device
R6="$T/root6"; mk_root "$R6"
printf 'Filename Type Size Used Priority\n/swap/swapfile file 16252928 0 -2\n' > "$R6/proc/swaps"
mkdir -p "$R6/usr/lib/systemd/zram-generator.conf.d"
printf '[zram0]\nzram-size = ram\n\n[zram1]\nzram-size = 1024\n' > "$R6/usr/lib/systemd/zram-generator.conf.d/90-test.conf"
"$HELPER" apply --zswap-percent 21 --disable-zram --root "$R6" >/dev/null 2>&1
check "drop-in covers every configured zram device" "grep -q '^\[zram0\]' '$R6/etc/systemd/zram-generator.conf.d/99-kyty-host-memory.conf' && grep -q '^\[zram1\]' '$R6/etc/systemd/zram-generator.conf.d/99-kyty-host-memory.conf'"

# 7. argument validation
for bad in "--zswap-percent 0" "--zswap-percent 99" "--zswap-percent x" "--zswap-percent 20 --compressor zstd;reboot" "--zswap-percent 20 --swapfile-mib 10" "--zswap-percent 20 --nope" ""; do
	R4="$T/root4"; rm -rf "$R4"; mk_root "$R4"
	# shellcheck disable=SC2086
	"$HELPER" apply $bad --root "$R4" >/dev/null 2>&1; RC=$?
	check "rejected: apply $bad" "[ $RC != 0 ] && [ ! -e '$R4/etc/systemd/system/kyty-host-memory.service' ]"
done
"$HELPER" frobnicate --root "$R" >/dev/null 2>&1; check "unknown command is rejected" "[ \$? != 0 ]"

# 8. an emulator-less system without zswap
R5="$T/root5"; mk_root "$R5"; rm -rf "$R5/sys/module/zswap"
"$HELPER" apply --zswap-percent 21 --root "$R5" >/dev/null 2>&1; check "no zswap in the kernel: refused" "[ \$? != 0 ]"

# 9. not root and not a dry run: refused
if [ "$(id -u)" != 0 ]; then
	"$HELPER" apply --zswap-percent 21 >/dev/null 2>&1; check "a normal user cannot run apply" "[ \$? != 0 ]"
fi

# 10. the constants and paths agree with the C++ side
hdr() { sed -n "s/.*$1[^0-9]*= *\([0-9]*\);.*/\1/p" "$2" | head -1; }
HP=$(sed -n 's/.*kDiskReserveMiB *= *\([0-9]*\);.*/\1/p' "$POLICY_H" | head -1)
HS=$(sed -n 's/^DISK_RESERVE_MIB=\([0-9]*\).*/\1/p' "$HELPER")
check "disk reserve agrees with hostMemoryPolicy.h ($HP vs $HS)" "[ -n '$HP' ] && [ '$HP' = '$HS' ]"
HM=$(sed -n 's/.*kSwapoffMarginMiB *= *\([0-9]*\);.*/\1/p' "$POLICY_H" | head -1)
HMS=$(sed -n 's/^SWAPOFF_MARGIN_MIB=\([0-9]*\).*/\1/p' "$HELPER")
check "swapoff margin agrees ($HM vs $HMS)" "[ -n '$HM' ] && [ '$HM' = '$HMS' ]"
for pair in "kUnitPath:UNIT" "kZramDropinPath:DROPIN" "kStateDir:STATE_DIR" "kSwapDir:SWAP_DIR"; do
	cname=${pair%%:*}; sname=${pair##*:}
	cval=$(sed -n "s|.*$cname\[\] *= *\"\([^\"]*\)\";.*|\1|p" "$PROBE_H" | head -1)
	case $sname in
		UNIT) sval=/etc/systemd/system/kyty-host-memory.service ;;
		DROPIN) sval=/etc/systemd/zram-generator.conf.d/99-kyty-host-memory.conf ;;
		STATE_DIR) sval=$(sed -n 's/^STATE_DIR=\(.*\)/\1/p' "$HELPER") ;;
		SWAP_DIR) sval=$(sed -n 's/^SWAP_DIR=\$STATE_DIR\(.*\)/\1/p' "$HELPER"); sval="$(sed -n 's/^STATE_DIR=\(.*\)/\1/p' "$HELPER")$sval" ;;
	esac
	check "path $cname agrees with the helper ($cval vs $sval)" "[ -n '$cval' ] && [ '$cval' = '$sval' ]"
done
check "helper unit and drop-in paths are the ones the C++ probe looks for" "grep -q '^UNIT=/etc/systemd/system/\$UNIT_NAME' '$HELPER' && grep -q '^DROPIN=\$DROPIN_DIR/99-kyty-host-memory.conf' '$HELPER'"

if command -v shellcheck >/dev/null 2>&1; then
	check "shellcheck is clean" "shellcheck -S warning '$HELPER' >/dev/null"
fi

if [ "$FAILS" != 0 ]; then echo "host_memory_helper_test: $FAILS check(s) failed"; exit 1; fi
echo "host_memory_helper_test: all checks passed"
