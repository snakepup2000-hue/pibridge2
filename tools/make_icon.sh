#!/bin/sh
# Regenerates app/Resources/AppIcon.icns from tools/make_icon.swift.
# Run after editing the artwork; build_app.sh copies the .icns into the bundle.
set -e
cd "$(dirname "$0")/.."

ACCENT=${1:-amber}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

swiftc -O tools/make_icon.swift -o "$WORK/make_icon"
"$WORK/make_icon" "$WORK/base.png" "$ACCENT"

# lockFocus renders at the display's backing scale, so normalise to 1024.
sips -z 1024 1024 "$WORK/base.png" --out "$WORK/icon.png" >/dev/null

mkdir -p "$WORK/AppIcon.iconset"
for s in 16 32 128 256 512; do
    sips -z $s $s "$WORK/icon.png" --out "$WORK/AppIcon.iconset/icon_${s}x${s}.png" >/dev/null
    sips -z $((s * 2)) $((s * 2)) "$WORK/icon.png" \
        --out "$WORK/AppIcon.iconset/icon_${s}x${s}@2x.png" >/dev/null
done

iconutil -c icns "$WORK/AppIcon.iconset" -o app/Resources/AppIcon.icns
echo "wrote app/Resources/AppIcon.icns ($ACCENT)"
