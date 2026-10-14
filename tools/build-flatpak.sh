#!/usr/bin/env bash
# Builds the Flatpak and a single-file bundle in dist/.
# Usage: tools/build-flatpak.sh [--install]   (--install also installs it for your user)
set -euo pipefail
cd "$(dirname "$0")/.."
ID=io.github.jackal_kr.Easel
MANIFEST=build-aux/flatpak/$ID.yml
for t in flatpak flatpak-builder; do
  command -v "$t" >/dev/null || { echo "Missing $t — run tools/check-env.sh"; exit 1; }
done
mkdir -p dist
# Dependencies install into the user installation, which needs its own flathub remote.
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --force-clean --install-deps-from=flathub \
  --repo=dist/flatpak-repo build-flatpak "$MANIFEST"
flatpak build-bundle dist/flatpak-repo "dist/$ID.flatpak" "$ID"
ls -lh "dist/$ID.flatpak"
if [ "${1:-}" = "--install" ]; then
  flatpak --user install -y --reinstall "dist/$ID.flatpak"
  echo "Run with:  flatpak run $ID"
else
  echo "Install with:  flatpak --user install dist/$ID.flatpak   (or re-run with --install)"
fi
