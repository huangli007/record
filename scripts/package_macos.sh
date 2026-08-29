#!/bin/bash
# Packages NotionRecorder.app into a self-contained, distributable bundle:
#  1. macdeployqt copies Qt frameworks + QML plugins
#  2. FFmpeg dylibs and their transitive Homebrew deps are copied and
#     re-linked against @executable_path / @loader_path
#  3. Ad-hoc codesign, verification, then a zip archive
#
# Usage: scripts/package_macos.sh [path/to/NotionRecorder.app]
set -euo pipefail

APP="${1:-build/NotionRecorder.app}"
APP="$(cd "$(dirname "$APP")" && pwd)/$(basename "$APP")"
FW="$APP/Contents/Frameworks"
BIN="$APP/Contents/MacOS/NotionRecorder"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/dist"

if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build the app first" >&2
    exit 1
fi

mkdir -p "$FW" "$OUT"

echo "==> macdeployqt (Qt frameworks + QML)"
MACDEPLOYQT="$(command -v macdeployqt 2>/dev/null || true)"
if [[ -z "$MACDEPLOYQT" && -x "$(brew --prefix 2>/dev/null)/opt/qt/bin/macdeployqt" ]]; then
    MACDEPLOYQT="$(brew --prefix)/opt/qt/bin/macdeployqt"
fi
if [[ -x "$MACDEPLOYQT" ]]; then
    "$MACDEPLOYQT" "$APP" \
        -qmldir="$ROOT/src/ui/qml" >/dev/null 2>&1 || true
else
    echo "    macdeployqt not found, skipping (expect broken bundle)" >&2
fi

echo "==> collecting Homebrew dylibs (FFmpeg + transitive deps)"
COPIED=()

collect_deps() {
    local file="$1"
    local dep name
    for dep in $(otool -L "$file" 2>/dev/null | awk '/\/opt\/homebrew\//{print $1}'); do
        name="$(basename "$dep")"
        if ! printf '%s\n' "${COPIED[@]}" | grep -qx "$name"; then
            COPIED+=("$name")
            cp -n "$dep" "$FW/$name" || true
            collect_deps "$FW/$name"
        fi
    done
}

collect_deps "$BIN"

echo "==> re-linking dylib references"
relink() {
    local file="$1" prefix="$2" dep name
    for dep in $(otool -L "$file" 2>/dev/null | awk '/\/opt\/homebrew\//{print $1}'); do
        name="$(basename "$dep")"
        install_name_tool -change "$dep" "$prefix/$name" "$file" 2>/dev/null || true
    done
    # Normalize the dylib's own id so other bundles can find it relatively.
    local id
    id="$(otool -D "$file" 2>/dev/null | tail -1)"
    if [[ "$id" == /opt/homebrew/* ]]; then
        install_name_tool -id "@loader_path/$(basename "$id")" "$file" 2>/dev/null || true
    fi
}

relink "$BIN" "@executable_path/../Frameworks"
for f in "$FW"/*.dylib; do
    relink "$f" "@loader_path"
done

echo "==> ad-hoc codesign"
codesign --force --deep --sign - "$APP" 2>/dev/null || {
    echo "    deep sign failed, trying per-file" >&2
    find "$APP" -type f \( -name "*.dylib" -o -name "NotionRecorder" \) \
        -exec codesign --force --sign - {} \; 2>/dev/null || true
    codesign --force --sign - "$APP"
}

echo "==> verification"
if otool -L "$BIN" | grep -q '/opt/homebrew'; then
    echo "error: binary still references Homebrew paths:" >&2
    otool -L "$BIN" | grep '/opt/homebrew' >&2
    exit 1
fi
codesign --verify --deep --strict "$APP" 2>/dev/null \
    && echo "    codesign verify: OK" \
    || echo "    codesign verify: WARNING (see above)"

NAME="NotionRecorder-$(defaults read "$APP/Contents/Info" CFBundleShortVersionString 2>/dev/null || echo 0.1.0)-macos"
rm -rf "$OUT/$NAME" "$OUT/$NAME.zip"
cp -R "$APP" "$OUT/$NAME.app"
(
    cd "$OUT"
    ditto -c -k --sequesterRsrc --keepParent "$NAME.app" "$NAME.zip"
)
echo "==> done: $OUT/$NAME.zip"
