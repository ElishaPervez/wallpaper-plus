#!/usr/bin/env bash
# Builds a shareable zip: release\WallpaperPlus-<commit>.zip holding the two programs, the
# settings UI and a short readme. Nothing from build\ that's personal (settings, library,
# thumbnails, logs) goes in. A running player is asked to quit so the build can replace its exe;
# close the settings window yourself first.
set -eo pipefail
cd "$(dirname "$0")/.."
if [ -f build/WallpaperPlus.exe ]; then
    build/WallpaperPlus.exe --quit
    sleep 2  # it exits once it has put the normal wallpaper back
fi
# Only build.bat's closing "Build OK" counts: otherwise build\ still holds the previous exes.
out=$(powershell -NoProfile -Command "& '.\build.bat'" 2>&1) || true
if ! grep -q "Build OK" <<<"$out"; then
    echo "$out" >&2
    echo "Build failed (see above); no release made." >&2
    exit 1
fi
grep -E "error|warning|Build OK" <<<"$out"
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
