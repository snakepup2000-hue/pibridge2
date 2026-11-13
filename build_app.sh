#!/bin/sh
# Builds PiBridge2.app (the GUI rewrite) and embeds the engine binary.
set -e
cd "$(dirname "$0")"

./build.sh

# The CommandLineTools SDK does not ship the SwiftUIMacros plugin, so @State /
# @StateObject cannot expand there. Xcode's toolchain does.
XC=/Applications/Xcode.app/Contents/Developer
SDK=$XC/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
SWIFTC=$XC/Toolchains/XcodeDefault.xctoolchain/usr/bin/swiftc

APP=build/PiBridge2.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

"$SWIFTC" -sdk "$SDK" -O -target arm64-apple-macos13.0 \
    -o "$APP/Contents/MacOS/PiBridge2" \
    app/PiBridgeApp.swift app/Engine.swift

cp app/Resources/Info.plist "$APP/Contents/Info.plist"
cp app/Resources/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"
cp build/pibridge "$APP/Contents/Resources/pibridge"
chmod +x "$APP/Contents/Resources/pibridge"

# Ad-hoc sign the bundle, including the embedded engine, or macOS kills it.
codesign --force --deep --sign - "$APP" >/dev/null 2>&1 || true
echo "built $APP"
