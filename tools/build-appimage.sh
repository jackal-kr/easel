#!/usr/bin/env bash
# Builds a portable AppImage into dist/.  Usage: tools/build-appimage.sh
# Needs curl and a normal build toolchain (see tools/check-env.sh). On first use it downloads
# linuxdeploy and its GTK and AppImage plugins into .cache/ (nothing is installed system-wide).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
CACHE=$ROOT/.cache/appimage
APPDIR=$ROOT/build-appimage/AppDir
ARCH=$(uname -m)
for t in meson ninja curl file; do
  command -v "$t" >/dev/null || { echo "Missing $t — run tools/check-env.sh"; exit 1; }
done
mkdir -p "$CACHE" dist

fetch() {  # fetch <url> <file>
  [ -x "$CACHE/$2" ] || { echo "Downloading $2"; curl -fL --retry 3 -o "$CACHE/$2" "$1"; chmod +x "$CACHE/$2"; }
}
fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage" linuxdeploy
fetch "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh" linuxdeploy-plugin-gtk.sh

fetch "https://github.com/linuxdeploy/linuxdeploy-plugin-appimage/releases/download/continuous/linuxdeploy-plugin-appimage-$ARCH.AppImage" linuxdeploy-plugin-appimage

# Run the AppImage tools without FUSE so this also works in containers.
export APPIMAGE_EXTRACT_AND_RUN=1

rm -rf build-appimage
meson setup build-appimage --prefix=/usr --buildtype=release >/dev/null
ninja -C build-appimage
DESTDIR="$APPDIR" meson install -C build-appimage >/dev/null

# Settings schema and relocation hook: the app finds its catalogs, help and schema inside the image.
glib-compile-schemas "$APPDIR/usr/share/glib-2.0/schemas"
mkdir -p "$APPDIR/apprun-hooks"
cat > "$APPDIR/apprun-hooks/easel.sh" <<'H'
export EASEL_LOCALEDIR="$APPDIR/usr/share/locale"
export EASEL_DOCDIR="$APPDIR/usr/share/doc/easel"
export XDG_DATA_DIRS="$APPDIR/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export GSETTINGS_SCHEMA_DIR="$APPDIR/usr/share/glib-2.0/schemas${GSETTINGS_SCHEMA_DIR:+:$GSETTINGS_SCHEMA_DIR}"
H

version=$(meson introspect build-appimage --projectinfo | python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])')
export PATH="$CACHE:$PATH" DEPLOY_GTK_VERSION=4 LDAI_OUTPUT="dist/Easel-$version-$ARCH.AppImage" OUTPUT="dist/Easel-$version-$ARCH.AppImage" VERSION="$version"
ID=io.github.jackal_kr.Easel
"$CACHE/linuxdeploy" --appdir "$APPDIR" --plugin gtk \
  --desktop-file "$APPDIR/usr/share/applications/$ID.desktop" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/$ID.svg" \
  --output appimage
