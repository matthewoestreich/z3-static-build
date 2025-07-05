#!/usr/bin/env bash

# HOW TO CALL THIS SCRIPT
# `bash create_release.sh vX.X.X`
# Where `vX.X.X` is the version/tag.

if [ -z "$1" ]; then
  echo "Usage: $0 <version/tag>"
  exit 1
fi

git tag "$1"
git push origin "$1"
