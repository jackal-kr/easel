#!/usr/bin/env bash
# Shows what will be pushed, then pushes the current branch to origin.
set -euo pipefail
cd "$(dirname "$0")/.."
branch=$(git rev-parse --abbrev-ref HEAD)
git fetch -q origin || true
echo "Branch: $branch"
if [ -n "$(git status --porcelain)" ]; then
  echo "Warning: uncommitted changes are NOT pushed:"; git status --short
fi
echo; echo "Commits to push:"
git log --oneline "origin/$branch..HEAD" 2>/dev/null || git log --oneline -5
echo
git push -u origin "$branch"
