#!/bin/bash
# Installs xe-fp (root-owned helper) and pam_xefp.so from the bundle staged by build.sh.
#   sudo bash tools/fingerprint/install-pam.sh [DIST] [--enable-sudo]
# DIST defaults to tools/build/fingerprint/dist. Without --enable-sudo only the files are copied and /etc/pam.d is
# untouched. With it, one line is prepended to /etc/pam.d/sudo_local (a backup is kept).
# Undo: sudo bash tools/fingerprint/uninstall-pam.sh
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../build/fingerprint/dist"; ENABLE=0
for a in "$@"; do case "$a" in --enable-sudo) ENABLE=1;; -*) echo "unknown option: $a"; exit 2;; *) SRC="$a";; esac; done
[ -x "$SRC/xefp/xe-fp" ] && [ -f "$SRC/pam/pam_xefp.so" ] || { echo "no bundle in $SRC: run tools/fingerprint/build.sh first"; exit 1; }

install -d -o root -g wheel -m 755 /usr/local/libexec /usr/local/libexec/xefp /usr/local/libexec/xefp/lib
install -d -o root -g wheel -m 755 /usr/local/lib /usr/local/lib/pam
install -o root -g wheel -m 755 "$SRC/xefp/xe-fp" /usr/local/libexec/xefp/xe-fp
for f in "$SRC"/xefp/lib/*.dylib; do install -o root -g wheel -m 644 "$f" /usr/local/libexec/xefp/lib/; done
install -o root -g wheel -m 644 "$SRC/pam/pam_xefp.so" /usr/local/lib/pam/pam_xefp.so
echo "installed /usr/local/libexec/xefp/xe-fp and /usr/local/lib/pam/pam_xefp.so"

LINE="auth       sufficient     /usr/local/lib/pam/pam_xefp.so timeout=10"
if [ $ENABLE = 1 ]; then
  F=/etc/pam.d/sudo_local
  if [ -f "$F" ]; then cp -p "$F" "$F.xefp-backup.$(date +%Y%m%d%H%M%S)"; fi
  if [ -f "$F" ] && grep -q pam_xefp "$F"; then
    echo "sudo_local already references pam_xefp"
  else
    # prepend so it runs first; keep any existing lines
    { echo "# xe-fp fingerprint (added by install-pam.sh)"; echo "$LINE"; [ -f "$F" ] && cat "$F"; } > "$F.new"
    chown root:wheel "$F.new"; chmod 444 "$F.new"; mv "$F.new" "$F"
    echo "added to $F:"; cat "$F"
  fi
  echo "TEST IN A NEW TERMINAL (keep this one open):  sudo -k; sudo -v"
else
  echo "To enable it for sudo, re-run with --enable-sudo, or add this line at the top of /etc/pam.d/sudo_local:"
  echo "  $LINE"
fi
