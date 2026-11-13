#!/bin/sh
# Builds and installs PiBridge2 into /Applications, beside the original
# PiBridge.app, which stays as the working fallback.
set -e
cd "$(dirname "$0")"
./build_app.sh
rm -rf /Applications/PiBridge2.app
cp -R build/PiBridge2.app /Applications/PiBridge2.app
codesign -v /Applications/PiBridge2.app
echo "installed /Applications/PiBridge2.app"
