#!/bin/bash
# make_zip.sh - pack the co-op Windows build into GoldenEye-Coop-win32.zip
#
# Usage (from the repo root, after building build_win):
#   coop/package/make_zip.sh <steam_api.dll> [out.zip]
#
# Layout (the one handed to friends):
#   GoldenEye-Coop/ProjectMidas.exe SDL2.dll steam_api.dll steam_appid.txt
#                  Host co-op.bat Join co-op.bat README-COOP.txt
#                  gamecontrollerdb.txt gamecontrollerdefault.txt midas_ini_help.txt
#                  Data/README.txt Data/shaders_base.txt
#
# steam_api.dll is the 32-bit one from Steamworks.NET Standalone 2025.164.1
# (Windows-x86, SHA-256 fdafb7c6...). Never packs a ROM.
set -euo pipefail

steamdll=${1:?usage: $0 <steam_api.dll> [out.zip]}
out=$(realpath -m "${2:-GoldenEye-Coop-win32.zip}")
build=build_win

for f in "$build/ProjectMidas.exe" "$build/SDL2.dll" "$steamdll"; do
    [ -f "$f" ] || { echo "missing $f - build first" >&2; exit 1; }
done

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
dir="$stage/GoldenEye-Coop"
mkdir -p "$dir/Data"

cp "$build/ProjectMidas.exe" "$build/SDL2.dll" "$dir/"
cp "$steamdll" "$dir/steam_api.dll"
printf '480\r\n' > "$dir/steam_appid.txt"   # Spacewar, see README-COOP.txt
for b in "Host co-op.bat" "Join co-op.bat"; do
    sed 's/\r$//; s/$/\r/' "coop/package/$b" > "$dir/$b"
done
cp coop/package/README-COOP.txt "$dir/"
cp tools/gamecontrollerdb.txt tools/gamecontrollerdefault.txt tools/midas_ini_help.txt "$dir/"
cp tools/Data_README.txt "$dir/Data/README.txt"
cp tools/shaders_base.txt "$dir/Data/shaders_base.txt"

rm -f "$out"
(cd "$stage" && zip -q -r -X "$out" GoldenEye-Coop)
echo "wrote $out"
