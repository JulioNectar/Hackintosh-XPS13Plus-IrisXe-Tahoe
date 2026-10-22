#!/bin/bash
# Builds the fingerprint tools for the Goodix MOC sensor (27c6:63bc) from source: no Homebrew, no sudo.
#   xe-fp        libfprint command-line tool (list / enroll / verify / identify / delete)
#   pam_xefp.so  PAM module for sudo (runs the root-owned xe-fp helper installed by install-pam.sh)
# It downloads libusb 1.0.29, glib 2.84.4, libgusb 0.3.10 and libfprint (pinned commit), applies
# xefp-libfprint-macos.patch to libfprint (goodixmoc driver only), installs them into WORKDIR/deps and stages a
# relocatable bundle in WORKDIR/dist (xefp/xe-fp + xefp/lib/*.dylib, pam/pam_xefp.so).
#   bash tools/fingerprint/build.sh [WORKDIR]      default: tools/build/fingerprint (git-ignored)
# Needs: Command Line Tools (clang, git, python3), curl, network. meson/ninja/pkg-config are taken from PATH,
# or installed into a Python venv inside WORKDIR. Re-running skips the dependency steps already done
# (delete WORKDIR/stamp-* to redo one).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
W="${1:-$HERE/../build/fingerprint}"
mkdir -p "$W"; W="$(cd "$W" && pwd)"
S="$W/src"; D="$W/deps"; mkdir -p "$S" "$D"

LIBUSB_URL=https://github.com/libusb/libusb/releases/download/v1.0.29/libusb-1.0.29.tar.bz2
LIBUSB_SHA=5977fc950f8d1395ccea9bd48c06b3f808fd3c2c961b44b0c2e6e29fc3a70a85
GLIB_URL=https://download.gnome.org/sources/glib/2.84/glib-2.84.4.tar.xz
GLIB_SHA=8a9ea10943c36fc117e253f80c91e477b673525ae45762942858aef57631bb90
GUSB_GIT=https://github.com/hughsie/libgusb.git;                GUSB_TAG=0.3.10
FPRINT_GIT=https://gitlab.freedesktop.org/libfprint/libfprint.git; FPRINT_REV=6f9479c3d55f847c1b3769f28ceb99227f9858cf

export MACOSX_DEPLOYMENT_TARGET=13.0
MIN=-mmacosx-version-min=13.0
export PATH="$D/bin:$PATH" PKG_CONFIG_PATH="$D/lib/pkgconfig"

# build tools
if ! command -v meson >/dev/null || ! command -v ninja >/dev/null || ! command -v pkg-config >/dev/null; then
  if [ ! -x "$W/venv/bin/meson" ]; then
    python3 -m venv "$W/venv"
    "$W/venv/bin/python3" -m pip install -q --upgrade pip
    "$W/venv/bin/python3" -m pip install -q meson ninja pkgconf
  fi
  export PATH="$W/venv/bin:$PATH"
fi
echo "meson $(meson --version), ninja $(ninja --version), pkg-config $(pkg-config --version)"

fetch() { # url sha256 file
  [ -f "$3" ] || curl -fL --retry 3 -o "$3" "$1"
  echo "$2  $3" | shasum -a 256 -c - >/dev/null || { echo "checksum mismatch: $3"; rm -f "$3"; exit 1; }
}
done_() { [ -f "$W/stamp-$1" ]; }
mark() { touch "$W/stamp-$1"; }

# 1. libusb
if ! done_ libusb; then
  fetch $LIBUSB_URL $LIBUSB_SHA "$S/libusb.tar.bz2"
  rm -rf "$S/libusb-1.0.29"; tar -xf "$S/libusb.tar.bz2" -C "$S"
  (cd "$S/libusb-1.0.29" && ./configure -q --prefix="$D" CFLAGS="-O2 -arch x86_64 $MIN" && make -j8 install >/dev/null)
  mark libusb
fi

# 2. glib (pcre2, libffi and proxy-libintl come in as meson subprojects)
if ! done_ glib; then
  fetch $GLIB_URL $GLIB_SHA "$S/glib.tar.xz"
  rm -rf "$S/glib-2.84.4"; tar -xf "$S/glib.tar.xz" -C "$S"
  (cd "$S/glib-2.84.4" && meson setup _b --prefix="$D" --libdir=lib --buildtype=release -Dtests=false \
     -Dintrospection=disabled -Dnls=disabled -Ddocumentation=false -Dman-pages=disabled -Dlibelf=disabled \
     -Dsysprof=disabled -Dglib_debug=disabled -Dc_args=$MIN -Dobjc_args=$MIN >/dev/null && ninja -C _b install >/dev/null)
  mark glib
fi

# 3. libgusb (0.4.x needs json-glib, so the 0.3 branch)
if ! done_ gusb; then
  rm -rf "$S/libgusb"; git -c advice.detachedHead=false clone -q --depth 1 --branch $GUSB_TAG $GUSB_GIT "$S/libgusb"
  (cd "$S/libgusb" && meson setup _b --prefix="$D" --libdir=lib --buildtype=release -Dtests=false \
     -Dintrospection=false -Dvapi=false -Ddocs=false >/dev/null && ninja -C _b install >/dev/null)
  mark gusb
fi

# 4. libfprint + our patch, goodixmoc driver only
if ! done_ libfprint; then
  rm -rf "$S/libfprint"; git clone -q $FPRINT_GIT "$S/libfprint"
  git -C "$S/libfprint" -c advice.detachedHead=false checkout -q $FPRINT_REV
  git -C "$S/libfprint" apply "$HERE/xefp-libfprint-macos.patch"
  (cd "$S/libfprint" && meson setup _b --prefix="$D" --libdir=lib --buildtype=debugoptimized -Ddrivers=goodixmoc \
     -Dintrospection=false -Ddoc=false -Dudev_rules=disabled -Dudev_hwdb=disabled -Dinstalled-tests=false \
     -Dgtk-examples=false >/dev/null && ninja -C _b install >/dev/null)
  mark libfprint
fi

# 5. xe-fp and pam_xefp.so
CFLAGS=(-O2 -Wall -arch x86_64 $MIN)
clang "${CFLAGS[@]}" "$HERE/xe-fp.c" $(pkg-config --cflags --libs libfprint-2 glib-2.0 gio-unix-2.0) \
  -Wl,-rpath,"$D/lib" -o "$W/xe-fp"
clang "${CFLAGS[@]}" -Wno-incompatible-pointer-types-discards-qualifiers -bundle "$HERE/pam_xefp.c" -lpam \
  -o "$W/pam_xefp.so"
clang "${CFLAGS[@]}" "$HERE/pamtest.c" -lpam -o "$W/pamtest"

# 6. relocatable bundle: dist/xefp/{xe-fp,lib/*.dylib}, every reference through @rpath
OUT="$W/dist/xefp"
rm -rf "$W/dist"; mkdir -p "$OUT/lib" "$W/dist/pam"
LIBS=(libfprint-2.2 libgusb.2 libgio-2.0.0 libglib-2.0.0 libgobject-2.0.0 libgmodule-2.0.0 libusb-1.0.0 libintl.8 libffi.7)
cp "$W/xe-fp" "$OUT/xe-fp"
for l in "${LIBS[@]}"; do cp "$D/lib/$l.dylib" "$OUT/lib/"; chmod u+w "$OUT/lib/$l.dylib"; done
fix() { for l in "${LIBS[@]}"; do install_name_tool -change "$D/lib/$l.dylib" "@rpath/$l.dylib" "$1" 2>/dev/null; done; }
for l in "${LIBS[@]}"; do
  f="$OUT/lib/$l.dylib"
  install_name_tool -id "@rpath/$l.dylib" "$f" 2>/dev/null
  fix "$f"
  install_name_tool -add_rpath @loader_path "$f" 2>/dev/null || true
done
fix "$OUT/xe-fp"
install_name_tool -delete_rpath "$D/lib" "$OUT/xe-fp"
install_name_tool -add_rpath @executable_path/lib "$OUT/xe-fp"
cp "$W/pam_xefp.so" "$W/dist/pam/"
for f in "$OUT/xe-fp" "$OUT"/lib/*.dylib "$W/dist/pam/pam_xefp.so"; do codesign -f -s - "$f" 2>/dev/null; done
if otool -L "$OUT/xe-fp" "$OUT"/lib/*.dylib | grep -q "$D"; then echo "ERROR: build-tree path left in the bundle"; exit 1; fi
echo "built: $OUT/xe-fp and $W/dist/pam/pam_xefp.so"
echo "next:  $OUT/xe-fp list ; then: sudo bash $HERE/install-pam.sh $W/dist"
