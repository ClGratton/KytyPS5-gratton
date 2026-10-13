#!/bin/bash
# The one-time install of the root helper (the text pkexec runs as root), run without root on a fake tree, and the
# check that the helper compiled into the launcher is the helper in the source tree.
# usage: host_memory_install_test.sh <launcher> <src/launcher/host/kyty-host-memory>
set -u
LAUNCHER=$1
SOURCE=$2
FAILS=0
check() { if ! eval "$2"; then echo "FAIL: $1"; FAILS=$((FAILS + 1)); fi; }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

"$LAUNCHER" --print-host-helper > "$T/embedded"
SHA=$("$LAUNCHER" --print-host-helper --sha256)
check "the helper compiled into the launcher is the source file" "cmp -s '$T/embedded' '$SOURCE'"
check "the compiled-in hash is the SHA-256 of the source file" "[ '$SHA' = \"\$(sha256sum '$SOURCE' | cut -d' ' -f1)\" ]"

"$LAUNCHER" --print-host-helper --installer > "$T/installer"
B64=$(base64 -w0 "$SOURCE")
TARGET=/usr/local/libexec/kyty/kyty-host-memory
check "the install step starts with the root and chown switches set for production" "grep -qx 'ROOT=' '$T/installer' && grep -qx 'CHOWN=yes' '$T/installer'"

# run the install step on a fake root: rewrite the two switches
fake_install() {   # fake_install <root> <chown yes|no> <target> <sha> <b64> [args...]
	local root=$1 chown=$2
	shift 2
	sed -e "s|^ROOT=\$|ROOT=$root|" -e "s|^CHOWN=yes\$|CHOWN=$chown|" "$T/installer" > "$T/installer.fake"
	sh -c "$(cat "$T/installer.fake")" kyty-install "$@"
}

# 1. a good install
R="$T/root"; mkdir -p "$R/usr/share/polkit-1/actions"
OUT=$(fake_install "$R" no "$TARGET" "$SHA" "$B64" 2>&1); RC=$?
check "install succeeds ($RC: $OUT)" "[ $RC = 0 ]"
check "the installed helper is the source" "cmp -s '$R$TARGET' '$SOURCE'"
check "the installed helper is mode 755" "[ \"\$(stat -c %a '$R$TARGET')\" = 755 ] && [ \"\$(stat -c %a '$R/usr/local/libexec/kyty')\" = 755 ]"
POLICY=$R/usr/share/polkit-1/actions/org.kyty.host-memory.policy
check "the polkit action names the exact path with auth_admin" "grep -q 'exec.path\">$TARGET<' '$POLICY' && grep -q '<allow_active>auth_admin<' '$POLICY' && grep -q 'org.kyty.host-memory.run' '$POLICY'"
check "no temporary file is left behind" "[ -z \"\$(ls -A '$R/usr/local/libexec/kyty' | grep -v '^kyty-host-memory\$')\" ]"
if command -v xmllint >/dev/null 2>&1; then check "the polkit action is well-formed XML" "xmllint --noout '$POLICY'"; fi

# 2. a second install replaces it
echo tampered > "$R$TARGET"
fake_install "$R" no "$TARGET" "$SHA" "$B64" >/dev/null 2>&1
check "re-installing restores the helper" "cmp -s '$R$TARGET' '$SOURCE'"

# 3. the second location, and no polkit directory
R2="$T/root2"; mkdir -p "$R2"
OUT=$(fake_install "$R2" no /usr/libexec/kyty/kyty-host-memory "$SHA" "$B64" 2>&1)
check "the /usr/libexec location works without a polkit directory" "cmp -s '$R2/usr/libexec/kyty/kyty-host-memory' '$SOURCE' && grep -q 'no polkit actions directory' <<<\"\$OUT\""

# 4. refusals: nothing is installed
refuse() {   # refuse <description> <expect-text> <root> <chown> <args...>
	local what=$1 expect=$2 root=$3 chown=$4
	shift 4
	rm -rf "$root"; mkdir -p "$root"
	OUT=$(fake_install "$root" "$chown" "$@" 2>&1); RC=$?
	check "refused: $what" "[ $RC != 0 ] && grep -q '$expect' <<<\"\$OUT\" && [ ! -e '$root$TARGET' ]"
}
refuse "a hash that does not match" "does not match" "$T/r3" no "$TARGET" "0000000000000000000000000000000000000000000000000000000000000000" "$B64"
refuse "a changed helper text" "does not match" "$T/r4" no "$TARGET" "$SHA" "$(printf 'echo pwned\n' | base64 -w0)"
refuse "a path outside the two locations" "unexpected install path" "$T/r5" no "/usr/bin/kyty-host-memory" "$SHA" "$B64"
refuse "a path with .." "unexpected install path" "$T/r6" no "/usr/local/libexec/kyty/../../bin/kyty-host-memory" "$SHA" "$B64"
refuse "text that is not base64" "base64" "$T/r7" no "$TARGET" "$SHA" "@@@@"
# with the ownership check on (as in production), a directory the user owns is refused before anything is written
refuse "directories owned by a user" "not owned by root\|writable" "$T/r8" yes "$TARGET" "$SHA" "$B64"

# 5. the production text, unmodified, refuses a wrong hash before touching anything (safe without root)
OUT=$(sh -c "$(cat "$T/installer")" kyty-install "$TARGET" 0000 "$B64" 2>&1); RC=$?
check "the production install step refuses a wrong hash" "[ $RC != 0 ] && grep -q 'does not match' <<<\"\$OUT\""

# 6. the helper runs after the install with the arguments after --
R9="$T/root9"; mkdir -p "$R9"
OUT=$(fake_install "$R9" no "$TARGET" "$SHA" "$B64" -- undo --root "$T/nothing" 2>&1); RC=$?
check "the helper runs after installing, with its arguments" "[ $RC = 0 ] && grep -q 'nothing to undo' <<<\"\$OUT\""
OUT=$(fake_install "$R9" no "$TARGET" "$SHA" "$B64" -- apply --zswap-percent 0 --root "$T/nothing" 2>&1); RC=$?
check "a helper failure is the command's failure" "[ $RC != 0 ] && grep -q 'zswap-percent must be' <<<\"\$OUT\""

if [ "$FAILS" != 0 ]; then echo "host_memory_install_test: $FAILS check(s) failed"; exit 1; fi
echo "host_memory_install_test: all checks passed"
