#!/usr/bin/env bash
# Prints G2fresh's version: the CMake project version, which the builds carry and the packages are named after.
#
#   version.sh            # 0.10.0
#   version.sh v0.10.0    # the same, and fails unless the tag is that version (a release's tag)
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
version=$(sed -n 's/^project(G2fresh VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
if [ -z "$version" ]; then
  echo "version.sh: no project(G2fresh VERSION ...) in CMakeLists.txt" >&2
  exit 1
fi
if [ $# -gt 0 ] && [ "$1" != "v$version" ]; then
  echo "version.sh: the tag $1 is not the version in CMakeLists.txt ($version, tag v$version)" >&2
  exit 1
fi
echo "$version"
