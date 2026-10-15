#!/bin/zsh
# Build and install the boot-skip override pak for SC6.
#
# It shortens the boot logo and caution cards:
#   - DB_AdvertiseInfo: every card's WaitTime becomes 0 s and every card skippable,
#   - layout/Advertise: the card fade animations become one frame (1/60 s).
# The edited assets go into ~mods/pakchunk999-WindowsNoEditor_P.pak, which the
# game loads after its own paks. The game's paks are not changed.
#
# Usage: SC6_AES_KEY_FILE=<file with the AES key string> mods/build-boot-skip.sh
# Undo:  delete Content/Paks/~mods/pakchunk999-WindowsNoEditor_P.pak
set -euo pipefail

here=${0:A:h}
repo=${here:h}
key_file=${SC6_AES_KEY_FILE:?set SC6_AES_KEY_FILE to a file holding the SC6 AES key}
paks=${SC6_PAKS:-$HOME/Games/SteamLibrary/steamapps/common/SoulCalibur 6/SoulcaliburVI/Content/Paks}
asset_dir=SoulcaliburVI/Content/UI/GameFlow/GameScenes/Advertise

# UE 4.17 uses the first 32 bytes of the ASCII key string as the AES-256 key.
key=0x$(head -c 32 $key_file | xxd -p -c 64)

work=$(mktemp -d)
trap 'rm -rf $work' EXIT
repak --aes-key $key unpack -q -f -o $work/orig -i $asset_dir $paks/pakchunk0-WindowsNoEditor.pak

mkdir -p $work/mod/$asset_dir/layout
cp $work/orig/$asset_dir/DB_AdvertiseInfo.{uasset,uexp} $work/mod/$asset_dir/
cp $work/orig/$asset_dir/layout/Advertise.{uasset,uexp} $work/mod/$asset_dir/layout/

python3 -I $repo/tools/assets/dt_dump.py $work/mod/$asset_dir/DB_AdvertiseInfo.uasset --set WaitTime=0 SkipFlag=true
python3 -I $repo/tools/assets/shrink_widget_anims.py $work/mod/$asset_dir/layout/Advertise.uasset

mkdir -p $here/build
out=$here/build/pakchunk999-WindowsNoEditor_P.pak
repak pack --version V4 -m "../../../" $work/mod $out
mkdir -p "$paks/~mods"
cp $out "$paks/~mods/"
print "installed: $paks/~mods/${out:t}"
