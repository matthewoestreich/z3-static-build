#!/usr/bin/env bash

# For removing tags remotely and locally after something goes wrong.

# HOW TO CALL THIS SCRIPT
# `bash delete_tag.sh vX.X.X`
# Where `vX.X.X` is the version/tag.

if [ -z "$1" ]; then
  echo "Usage: $0 <version/tag>"
  exit 1
fi

git tag -d "$1"
git push --delete origin "$1"