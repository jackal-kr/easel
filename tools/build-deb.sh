#!/usr/bin/env bash
# Builds an unsigned .deb into dist/.  Usage: tools/build-deb.sh
set -euo pipefail
cd "$(dirname "$0")/.."
for t in dpkg-buildpackage fakeroot dh; do
  command -v "$t" >/dev/null || { echo "Missing $t — run tools/check-env.sh"; exit 1; }
done
mkdir -p dist
# -b binary only, -us -uc unsigned; builds in-tree, writes ../easel_*.deb
dpkg-buildpackage -b -us -uc
version=$(dpkg-parsechangelog -S Version)
mv -v ../easel_${version}_*.deb ../easel-dbgsym_${version}_*.ddeb dist/ 2>/dev/null || true
mv -v ../easel_${version}_*.buildinfo ../easel_${version}_*.changes dist/ 2>/dev/null || true
fakeroot dh_clean >/dev/null 2>&1 || true
rm -rf obj-*
echo
ls -1 dist/*.deb
dpkg-deb --info dist/easel_${version}_*.deb | sed -n '1,14p'
echo "Install with:  sudo apt install ./$(ls dist/easel_${version}_*.deb | head -1)"
