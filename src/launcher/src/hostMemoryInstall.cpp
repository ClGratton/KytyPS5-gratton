#include "hostMemoryTrust.h"

namespace HostMemory {

// Run as root by pkexec: /bin/sh -c <this text> kyty-install <target> <sha256> <base64 of the helper> [-- args...]
// The ROOT= and CHOWN= lines are rewritten by the test to run it on a fake tree without root; in the
// launcher they are exactly as written here.
const std::string& InstallScriptText() {
	static const std::string text = R"KYTYINSTALL(
set -eu
ROOT=
CHOWN=yes
fail() { echo "error: $*" >&2; exit 1; }
TARGET=${1:?missing target}
WANT=${2:?missing hash}
B64=${3:?missing helper}
shift 3
if [ "${1:-}" = "--" ]; then shift; fi
case "$TARGET" in
	/usr/local/libexec/kyty/kyty-host-memory|/usr/libexec/kyty/kyty-host-memory) ;;
	*) fail "unexpected install path $TARGET" ;;
esac
DIR=${TARGET%/*}
umask 022
work=$(mktemp -d) || fail "no temporary directory"
trap 'rm -rf "$work"' EXIT
# The helper is checked against the hash compiled into the launcher before anything is installed.
printf '%s' "$B64" | base64 -d > "$work/helper" || fail "the helper text is not valid base64"
got=$(sha256sum "$work/helper" | cut -d' ' -f1)
[ "$got" = "$WANT" ] || fail "the helper does not match the hash compiled into the launcher"

# Every directory above the helper, as they really are, must be root-only.
check_chain() {
	[ "$CHOWN" = yes ] || return 0
	p=$(realpath "$1") || fail "cannot resolve $1"
	while :; do
		st=$(stat -c '%u %a' "$p")
		uid=${st% *}
		mode=${st#* }
		[ "$uid" = 0 ] || fail "$p is not owned by root"
		[ $((0$mode & 022)) -eq 0 ] || fail "$p is writable by group or others (mode $mode)"
		[ "$p" = / ] && break
		p=$(dirname "$p")
	done
}
anc=$ROOT$DIR
while [ ! -d "$anc" ]; do anc=$(dirname "$anc"); done
check_chain "$anc"
mkdir -p "$ROOT$DIR"
[ "$CHOWN" = yes ] && chown root:root "$ROOT$DIR"
chmod 755 "$ROOT$DIR"
check_chain "$ROOT$DIR"

cp "$work/helper" "$ROOT$DIR/.kyty-host-memory.new"
chmod 755 "$ROOT$DIR/.kyty-host-memory.new"
[ "$CHOWN" = yes ] && chown root:root "$ROOT$DIR/.kyty-host-memory.new"
mv -f "$ROOT$DIR/.kyty-host-memory.new" "$ROOT$TARGET"
echo "installed $TARGET"

# The polkit action names this exact path. Best effort: without it pkexec still asks for the
# administrator password, with the generic text.
POLICY=$ROOT/usr/share/polkit-1/actions/org.kyty.host-memory.policy
if [ -d "$ROOT/usr/share/polkit-1/actions" ]; then
	cat > "$POLICY.new" <<POLICYEOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE policyconfig PUBLIC "-//freedesktop//DTD PolicyKit Policy Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/PolicyKit/1.0/policyconfig.dtd">
<policyconfig>
  <vendor>Kyty PS5</vendor>
  <action id="org.kyty.host-memory.run">
    <description>Change the swap and memory settings for the Kyty emulator</description>
    <message>Authentication is required to change the swap and memory settings for the Kyty emulator</message>
    <defaults>
      <allow_any>auth_admin</allow_any>
      <allow_inactive>auth_admin</allow_inactive>
      <allow_active>auth_admin</allow_active>
    </defaults>
    <annotate key="org.freedesktop.policykit.exec.path">$TARGET</annotate>
    <annotate key="org.freedesktop.policykit.exec.allow_gui">true</annotate>
  </action>
</policyconfig>
POLICYEOF
	chmod 644 "$POLICY.new"
	[ "$CHOWN" = yes ] && chown root:root "$POLICY.new"
	mv -f "$POLICY.new" "$POLICY"
	echo "installed the polkit action"
else
	echo "note: no polkit actions directory (read-only /usr?); the password prompt will use the generic text"
fi

if [ $# -gt 0 ]; then
	rm -rf "$work"
	trap - EXIT
	exec "$ROOT$TARGET" "$@"
fi
)KYTYINSTALL";
	return text;
}

} // namespace HostMemory
