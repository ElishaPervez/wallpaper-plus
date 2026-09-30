#!/usr/bin/env bash
# Builds a shareable zip: release\WallpaperPlus-<commit>.zip holding the two programs, the
# settings UI and a short readme. Nothing from build\ that's personal (settings, library,
# thumbnails, logs) goes in. Both programs must be closed first, or the build can't replace them.
set -e
cd "$(dirname "$0")/.."
powershell -NoProfile -Command "& '.\build.bat'" 2>&1 | grep -E "error|warning|Build OK"
rm -f build/obj/*/*.obj

version=$(git rev-parse --short HEAD)
[ -n "$(git status --porcelain)" ] && version="$version-dirty"
stage=release/WallpaperPlus
rm -rf "$stage" && mkdir -p "$stage"
cp build/WallpaperPlus.exe build/WallpaperPlusSettings.exe "$stage/"
cp -r ui "$stage/ui"
cp res/README-release.txt "$stage/README.txt"

zip="release/WallpaperPlus-$version.zip"
rm -f "$zip"
powershell -NoProfile -Command "Compress-Archive -Path '$stage' -DestinationPath '$zip' -CompressionLevel Optimal"
echo "Release: $(cygpath -w "$(pwd)/$zip")"
