#!/bin/bash
# The in-game updater end to end, on one copy over the mock Steam:
# a copy of the build in a folder laid out like the release zip (with a cart
# image, saves and a midas.ini of its own), coop/mock/update_server.py
# playing GitHub with a "v0.2.2" zip of the same build, and the game claiming
# to be v0.2.1 (GE_UPDATE_FAKEVER). GE_UPDATE_AUTO=restart takes the update
# as soon as the startup check finds it and restarts into it.
#
# The zip also carries a midas.ini, a save, a cart image and a ../ path:
# the update must leave all four alone. Checked after the run: the new exe
# and README are in place, the .old files are gone, the ROM, saves and ini
# are byte for byte what they were, and the restarted game logged the update.
#
#   ROOT=<repo> OUT=<dir> coop/docker/update.sh      (Docker: the defaults)
# AUTO=install stops before the restart, AUTO=none only shows the offer;
# DIGEST=bad|none must refuse it;
# SHOTS are seconds; TAB=4 shows the CONTROLS tab.
# PROMPT=update|later|skip starts on the Files screen instead and answers the
# launch prompt with the keyboard (xdotool) at ASK seconds: Update Now must
# install and restart, Not Now and Skip This Version must leave the game as
# it was, and Skip must remember the release (Data/Update/skip.txt).
ROOT=${ROOT:-/home/dev/project}
OUT=${OUT:-/out}
cd "$ROOT/build_pc" || exit 1
mkdir -p "$OUT"
work=$(mktemp -d)
inst="$work/GoldenEye-Coop"
mkdir -p "$inst/Data/Saves" "$inst/mock" "$work/new/GoldenEye-Coop/Data/Saves"

# the installed game
cp ProjectMidas "$inst/"
cp mock/libsteam_api.so "$inst/mock/"
cp "$ROOT/coop/package/README-COOP.txt" "$inst/"
cp "$ROOT/tools/gamecontrollerdb.txt" "$ROOT/tools/gamecontrollerdefault.txt" "$inst/"
cp Data/*.z64 "$inst/Data/"
printf 'old saves\n' > "$inst/Data/Saves/eeprom.bin"
printf '#ge-save coop_campaign 2 test\nsteam:1 dam agent A\n' > "$inst/Data/Saves/coop_campaign.txt"
printf '[display]\nauto_update=1\nmusic_volume=35\n' > "$inst/midas.ini"
# the game itself writes eeprom.bin and midas.ini as it runs; the cart and the
# campaign it never touches here
keep() { (cd "$inst" && sha256sum Data/*.z64 Data/Saves/coop_campaign.txt); }
keep > "$work/kept.sha"

# the "new release": the same exe with a marker on the end, a new README, and
# files an update must never write
new="$work/new/GoldenEye-Coop"
cp ProjectMidas "$new/" && printf 'v0.2.2 marker' >> "$new/ProjectMidas"
{ cat "$ROOT/coop/package/README-COOP.txt"; echo "v0.2.2 readme"; } > "$new/README-COOP.txt"
cp "$ROOT/tools/Data_README.txt" "$new/Data/README.txt"
printf 'auto_update=0\n' > "$new/midas.ini"
printf 'zip save\n' > "$new/Data/Saves/eeprom.bin"
printf 'not a cart\n' > "$new/Data/baserom.u.z64"
(cd "$work/new" && zip -q -r -X ../GoldenEye-Coop-v0.2.2-linux.zip GoldenEye-Coop)
python3 - "$work/GoldenEye-Coop-v0.2.2-linux.zip" <<'EOF'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "a") as z:
    z.writestr("GoldenEye-Coop/../escaped.txt", "outside\n")
EOF
newsha=$(sha256sum "$new/ProjectMidas" | cut -d' ' -f1)

python3 "$ROOT/coop/mock/update_server.py" "$work/GoldenEye-Coop-v0.2.2-linux.zip" --digest "${DIGEST:-good}" \
    --rate "${RATE:-1500000}" 2> "$OUT/update_server.log" &
srv=$!
rm -rf /tmp/gecoop-mock
Xvfb :1 -screen 0 1280x720x24 >/dev/null 2>&1 &
sleep 1
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_NOMSGBOX=1 LD_LIBRARY_PATH="$inst/mock"
export GE_UPDATE_API=http://127.0.0.1:8765/releases/latest GE_UPDATE_FAKEVER=v0.2.1
if [ -n "$PROMPT" ]; then
    export GE_MENU=select   # the Files screen, where the prompt is
    [ "$PROMPT" = update ] && AUTO=restart || AUTO=none
else
    export GE_MENU=lobby
    [ "${AUTO:-restart}" != none ] && export GE_UPDATE_AUTO=${AUTO:-restart}
fi
[ -n "$TAB" ] && export GE_COOP_LOBBYTAB=$TAB
cd "$inst"
DISPLAY=:1 GE_MOCK_STEAMID=76561190000000101 GE_MOCK_FRIEND=76561190000000202 \
    timeout "${RUNSECS:-60}" "$inst/ProjectMidas" > "$OUT/update.log" 2>&1 &
if [ -n "$PROMPT" ]; then
    answer() {   # Update Now is the first answer, Not Now the second, Skip the third
        local W k
        W=$(DISPLAY=:1 xdotool search --name . | tail -1)
        DISPLAY=:1 xdotool windowfocus "$W" 2>/dev/null; DISPLAY=:1 xdotool windowactivate "$W" 2>/dev/null
        case "$PROMPT" in later) k="d Return" ;; skip) k="d d Return" ;; *) k="Return" ;; esac   # D is C-right
        for k in $k; do DISPLAY=:1 xdotool keydown "$k"; sleep 0.08; DISPLAY=:1 xdotool keyup "$k"; sleep 0.3; done
    }
    for t in ${SHOTS:-12 16 18 30}; do
        if [ -z "$asked" ] && [ "$t" -gt "${ASK:-14}" ]; then sleep $((${ASK:-14} - ${prev:-0})); prev=${ASK:-14}; answer; asked=1; fi
        sleep $((t - ${prev:-0})); prev=$t; DISPLAY=:1 import -window root "$OUT/update_$t.png"
    done
else
    for t in ${SHOTS:-10 14 18 40}; do sleep $((t - ${prev:-0})); prev=$t; DISPLAY=:1 import -window root "$OUT/update_$t.png"; done
fi
sleep $(( ${RUNSECS:-60} - prev + 2 ))
pkill -f "$inst/ProjectMidas"; sleep 1
if [ "$PROMPT" = skip ]; then   # once more: the skipped release must not ask again
    DISPLAY=:1 GE_MOCK_STEAMID=76561190000000101 GE_MOCK_FRIEND=76561190000000202 \
        timeout 16 "$inst/ProjectMidas" > "$OUT/update2.log" 2>&1
    DISPLAY=:1 import -window root "$OUT/update_skipped_again.png" 2>/dev/null
fi
kill $srv; pkill Xvfb

# what the update did
fail=0
check() { if eval "$2"; then echo "ok:   $1"; else echo "FAIL: $1"; fail=1; fi; }
(cd "$inst" && sha256sum -c --quiet "$work/kept.sha") && echo "ok:   cart image and campaign save untouched" || { echo "FAIL: a kept file changed"; fail=1; }
check "nothing written outside the game folder" "[ ! -e '$work/escaped.txt' ]"
check "no ini, save or cart from the zip" "grep -q 'music_volume=35' '$inst/midas.ini' && ! grep -q 'auto_update=0' '$inst/midas.ini' && ! grep -q 'zip save' '$inst/Data/Saves/eeprom.bin'"
if [ -n "$PROMPT" ]; then
    check "the launch prompt was answered: $PROMPT" "grep -q 'launch prompt for v0.2.2' '$OUT/update.log'"
fi
if [ "$PROMPT" = skip ]; then
    check "the skipped release is remembered" "grep -qx 'v0.2.2' '$inst/Data/Update/skip.txt'"
    check "the next launch doesn't ask about it" "grep -q 'skipping v0.2.2' '$OUT/update2.log' && grep -q 'v0.2.2 is out' '$OUT/update2.log' && ! grep -q 'launch prompt' '$OUT/update2.log'"
fi
if [ "${AUTO:-restart}" = none ]; then
    check "nothing installed without the player's word" "! grep -q 'v0.2.2 marker' '$inst/ProjectMidas'"
elif [ "${DIGEST:-good}" = good ]; then
    check "the new exe is in place" "[ \"\$(sha256sum < '$inst/ProjectMidas' | cut -d' ' -f1)\" = $newsha ]"
    check "the new README is in place" "grep -q 'v0.2.2 readme' '$inst/README-COOP.txt'"
    check "the log says it was checked" "grep -q 'checked (sha256' '$OUT/update.log'"
    check "Data/README.txt added" "[ -f '$inst/Data/README.txt' ]"
    if [ "${AUTO:-restart}" = restart ]; then
        check "the restarted game says so" "grep -q 'now on v0.2.2 (was v0.2.1)' '$OUT/update.log'"
        check "the .old files are gone" "[ -z \"\$(find '$inst' -name '*.old')\" ]"
    else
        check "the old exe waits as .old" "[ -f '$inst/ProjectMidas.old' ]"
    fi
else
    check "the zip was refused" "grep -q 'checksum' '$OUT/update.log'"
    check "the old exe stays" "! grep -q 'v0.2.2 marker' '$inst/ProjectMidas'"
fi
grep 'ge: update:' "$OUT/update.log"
rm -rf "$work"
exit $fail
