#!/bin/sh
# Build the receiver and put it on the Pi. Run from anywhere in the repo.
set -eu
cd "$(dirname "$0")"

./build-static.sh
scp -o ConnectTimeout=8 hqdac-aarch64 "${PI_HOST:-pi}:~/hqdac"
# BusyBox head has no -1, so take the first line with sed.
ssh "${PI_HOST:-pi}" 'chmod +x ~/hqdac; echo "installed: $(~/hqdac -D hw:CARD=R2R,DEV=0 --caps | sed -n 1p)"'
