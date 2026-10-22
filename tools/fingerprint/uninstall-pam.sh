#!/bin/bash
# Removes pam_xefp from /etc/pam.d/sudo_local (backup kept) and deletes the installed files.
#   sudo bash tools/fingerprint/uninstall-pam.sh
# Prints stored on the sensor are not touched (use xe-fp delete <finger> before uninstalling to remove them).
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
F=/etc/pam.d/sudo_local
if [ -f "$F" ] && grep -q pam_xefp "$F"; then
  cp -p "$F" "$F.xefp-backup.$(date +%Y%m%d%H%M%S)"
  grep -v -e pam_xefp -e "xe-fp fingerprint (added by install-pam.sh)" "$F" > "$F.new" || true
  chown root:wheel "$F.new"; chmod 444 "$F.new"; mv "$F.new" "$F"
  echo "removed pam_xefp from $F"
fi
rm -f /usr/local/lib/pam/pam_xefp.so
rm -rf /usr/local/libexec/xefp
echo "uninstalled"
