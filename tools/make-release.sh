#!/usr/bin/env bash
# Builds the installer: release\WallpaperPlus-Setup-<version>.exe.
#   tools/make-release.sh           version 0.0.0-<commit> (plus -dirty with uncommitted changes)
#   tools/make-release.sh 1.2.3     version 1.2.3
# Needs Inno Setup 6 (winget install JRSoftware.InnoSetup). A running player is asked to quit so
# the build can replace its exe; close the settings window yourself first. Published releases are
# built by .github/workflows/build.yml when a vX.Y.Z tag is pushed, not by this script.
set -eo pipefail
cd "$(dirname "$0")/.."

iscc=$(command -v ISCC.exe || true)
for dir in "$LOCALAPPDATA/Programs/Inno Setup 6" "$PROGRAMFILES (x86)/Inno Setup 6" "$PROGRAMFILES/Inno Setup 6"; do
    [ -z "$iscc" ] && [ -f "$dir/ISCC.exe" ] && iscc="$dir/ISCC.exe"
done
if [ -z "$iscc" ]; then
    echo "Inno Setup 6 not found: winget install JRSoftware.InnoSetup" >&2
    exit 1
fi

version=$1
if [ -z "$version" ]; then
    version="0.0.0-$(git rev-parse --short HEAD)"
    [ -n "$(git status --porcelain)" ] && version="$version-dirty"
fi

if [ -f build/WallpaperPlus.exe ]; then
    build/WallpaperPlus.exe --quit
    sleep 2  # it exits once it has put the normal wallpaper back
fi
# Only build.bat's closing "Build OK" counts: otherwise build\ still holds the previous exes.
out=$(powershell -NoProfile -Command "& '.\build.bat'" 2>&1) || true
if ! grep -q "Build OK" <<<"$out"; then
    echo "$out" >&2
    echo "Build failed (see above); no installer made." >&2
    exit 1
fi
grep -E "error|warning|Build OK" <<<"$out"
rm -f build/obj/*/*.obj

"$iscc" //Q "//DAppVersion=$version" installer/WallpaperPlus.iss
echo "Installer: $(cygpath -w "$(pwd)/release/WallpaperPlus-Setup-$version.exe")"
