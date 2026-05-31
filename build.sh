#!/usr/bin/env bash
# Build StellaPD and install the simulator .so as a libcrankemu core into the
# Playdate SDK's shared disk, where a libcrankemu frontend (e.g. CrankBoy) can
# pdll-load it at /Shared/Emulation/cores/StellaPD.so.
set -euo pipefail

cd "$(dirname "$0")"

if [[ -z "${PLAYDATE_SDK_PATH:-}" ]]; then
    echo "build.sh: PLAYDATE_SDK_PATH is not set" >&2
    exit 1
fi

make "$@"

emu_dir="$PLAYDATE_SDK_PATH/Disk/Shared/Emulation"
dest_dir="$emu_dir/cores"
dest="$dest_dir/StellaPD.so"
src="StellaPD.pdx/pdex.so"

if [[ ! -f "$src" ]]; then
    echo "build.sh: $src not found after build" >&2
    exit 1
fi

# Don't auto-create the Emulation root -- if it's missing the user probably
# hasn't set up the libcrankemu frontend yet. cores/ is ours to manage.
if [[ ! -d "$emu_dir" ]]; then
    echo "build.sh: $emu_dir does not exist; refusing to create it" >&2
    exit 1
fi
mkdir -p "$dest_dir"
cp "$src" "$dest"
echo "installed: $dest"
