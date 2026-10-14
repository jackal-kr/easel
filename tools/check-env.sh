#!/usr/bin/env bash
# Checks that this machine can build, test, debug, package and publish Easel.
# Exit status 1 if something REQUIRED is missing; optional items only warn.
cd "$(dirname "$0")/.."

red=$'\e[31m'; grn=$'\e[32m'; ylw=$'\e[33m'; dim=$'\e[2m'; off=$'\e[0m'
fail=0; warn=0
ok()   { printf "  %s✔%s %s\n" "$grn" "$off" "$1"; }
bad()  { printf "  %s✘%s %s\n      %s→ %s%s\n" "$red" "$off" "$1" "$dim" "$2" "$off"; fail=1; }
meh()  { printf "  %s!%s %s\n      %s→ %s%s\n" "$ylw" "$off" "$1" "$dim" "$2" "$off"; warn=1; }

# need <required|optional> <command> <apt package> <purpose>
need() {
  if command -v "$2" >/dev/null 2>&1; then ok "$2 $(command -v "$2" >/dev/null && echo "")($4)"
  elif [ "$1" = required ]; then bad "$2 missing ($4)" "sudo apt install $3"
  else meh "$2 missing ($4)" "sudo apt install $3"; fi
}
# lib <pkg-config name> <min version> <apt package>
lib() {
  if pkg-config --exists "$1" 2>/dev/null; then
    v=$(pkg-config --modversion "$1")
    if pkg-config --atleast-version="$2" "$1"; then ok "$1 $v (need >= $2)"
    else bad "$1 $v is too old (need >= $2)" "sudo apt install $3  (or use the Flatpak build)"; fi
  else bad "$1 not found (need >= $2)" "sudo apt install $3"; fi
}

echo "System"
. /etc/os-release 2>/dev/null; ok "${PRETTY_NAME:-unknown OS} ($(uname -m))"

echo "Build tools"
need required gcc build-essential "C compiler"
need required meson meson "build system"
need required ninja ninja-build "build backend"
need required pkg-config pkgconf "finds libraries"
need required glib-compile-schemas libglib2.0-dev-bin "compiles the settings schema"
need required xgettext gettext "extracts translatable strings"
need required msgfmt gettext "compiles translations"

echo "Libraries (development files)"
lib gtk4 4.14 libgtk-4-dev
lib libadwaita-1 1.8 libadwaita-1-dev
lib gdk-pixbuf-2.0 2.0 libgdk-pixbuf-2.0-dev
lib glib-2.0 2.76 libglib2.0-dev

echo "Run, test, debug"
if [ -n "$WAYLAND_DISPLAY$DISPLAY" ]; then ok "display available (canvas tests run, app can start)"
else meh "no display (canvas tests will be skipped)" "run inside a desktop session"; fi
need optional gdb gdb "debugger (F5 in VS Code)"
need optional clangd clangd "code navigation in VS Code"
if [ -f build/build.ninja ]; then ok "build/ is configured"
else meh "build/ not configured" "meson setup build   (task: Build: configure)"; fi

echo "Git and GitHub"
need required git git "version control"
if git remote get-url origin >/dev/null 2>&1; then ok "remote origin: $(git remote get-url origin)"
else bad "no git remote 'origin'" "git remote add origin https://github.com/jackal-kr/easel.git"; fi
if command -v gh >/dev/null 2>&1; then
  if gh auth status >/dev/null 2>&1; then ok "gh logged in"; else meh "gh not logged in" "gh auth login"; fi
else meh "gh missing (optional, for GitHub CLI)" "sudo apt install gh"; fi
git config user.name >/dev/null && git config user.email >/dev/null && ok "git identity set" \
  || bad "git user.name / user.email not set" "git config --global user.name 'Name'; git config --global user.email you@example.com"

echo "Packaging: .deb"
need optional dpkg-buildpackage dpkg-dev "builds the .deb"
need optional fakeroot fakeroot "builds the .deb"
need optional dh debhelper "builds the .deb"
need optional desktop-file-validate desktop-file-utils "validates easel.desktop"
need optional appstreamcli appstream "validates the metainfo"
need optional lintian lintian "checks the .deb (optional)"

echo "Packaging: AppImage"
need optional curl curl "downloads linuxdeploy on first use"
need optional file file "used by linuxdeploy"

echo "Packaging: Flatpak"
need optional flatpak flatpak "runs Flatpaks"
need optional flatpak-builder flatpak-builder "builds the Flatpak"
if command -v flatpak >/dev/null 2>&1; then
  rt=$(grep -E '^runtime-version' build-aux/flatpak/*.yml | head -1 | sed "s/.*: *'\?\([0-9.]*\)'\?/\1/")
  for r in org.gnome.Platform org.gnome.Sdk; do
    if flatpak info "$r//$rt" >/dev/null 2>&1; then ok "$r//$rt installed"
    else meh "$r//$rt not installed" "flatpak install flathub $r//$rt   (the packaging task installs it)"; fi
  done
  flatpak remotes 2>/dev/null | grep -q flathub && ok "flathub remote configured" \
    || meh "flathub remote missing" "flatpak remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo"
fi

echo
if [ $fail -ne 0 ]; then echo "${red}Required items are missing — see the arrows above.${off}"; exit 1; fi
[ $warn -ne 0 ] && echo "${ylw}Ready to build; optional items above need attention for debugging or packaging.${off}" \
                || echo "${grn}Everything is set.${off}"
