#!/bin/sh
# Fails if a source file using _() / N_() is missing from po/POTFILES.in, or a .po is incomplete/invalid.
cd "$(dirname "$0")/.." || exit 1
rc=0
for f in $(grep -rlE '\b(_|N_)\("' src data --include=*.c --include=*.ui); do
  grep -qx "$f" po/POTFILES.in || { echo "missing from POTFILES.in: $f"; rc=1; }
done
for p in po/*.po; do
  msgfmt --check --statistics -o /dev/null "$p" || rc=1
  [ -z "$(msgattrib --untranslated --fuzzy --no-obsolete "$p" | grep -v '^#' | grep -A1 '^msgid "[^"]')" ] || { echo "untranslated/fuzzy in $p"; rc=1; }
done
exit $rc
