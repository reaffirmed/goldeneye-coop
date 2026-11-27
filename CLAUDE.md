# CLAUDE.md

Rules for AI agents (and anyone else) working on GoldenEye Coop. Most of them come from a mistake that already shipped once. Read this before you change code, and go through the release checklist at the end before anything is released.

This file adds to [`CONTRIBUTING.md`](CONTRIBUTING.md); it doesn't replace it. How co-op works and why is in [`coop/NOTES.md`](coop/NOTES.md). What has been tested, mission by mission, is in [`coop/TEST-CHECKLIST.md`](coop/TEST-CHECKLIST.md).

When a new mistake reaches players, add a rule for it under "Lessons" in the same PR as the fix: what to check, then what happened and in which release.

## Where things are

- Co-op and versus code: `src/game/coop*.c` and `port/src/coop*.c`. The network version is `COOP_VERSION` in `src/game/coop.c`.
- Updater: `port/src/port_update.c`. Release zip: `coop/package/make_zip.sh`.
- Languages: `lang/<code>/`, loaded by `port/src/port_lang.c`.
- Tests that need no cart: `coop/ci/*_test.c`. Mission runs and the two-copy rig over the mock Steam: `coop/ci/mission_runs.sh`, `coop/ci/net_scenarios.sh` and `coop/ci/checklist/`.
- CI: `.github/workflows/main.yml` (build-linux, build-windows, sanitize-linux), `coop-missions.yml` (mission runs with the cart image) and `coop-windows.yml` (the Windows zip and releases).
- Release notes: `coop/release-notes/<tag>.md`, plus a row in `CHANGELOG.md`.

## Running the real game

Run the real game before you say a change works in game. "The cart image isn't here" is almost never true in this project's cloud sessions, so look properly before you give up.

**Where the cart image is.** The game reads the GoldenEye 007 (USA) cart image, `baserom.u.z64`, SHA-1 `abe01e4aeb033b6c0836819f549c791b26cfde83`, 12,582,912 bytes. It is never in the repository. In this project's cloud sessions the owner's own copy is in the project's shared folder, under `/mnt/project-files/uploads/hearth/`. It is saved under an upload id with **no file extension**, so searching for `*.z64` finds nothing. Find it by size and hash:

    for f in /mnt/project-files/uploads/hearth/*; do
      [ "$(stat -c%s "$f")" = 12582912 ] &&
      [ "$(sha1sum < "$f" | cut -c1-40)" = abe01e4aeb033b6c0836819f549c791b26cfde83 ] && echo "$f"
    done

`/mnt/project-files/GoldenEye_007_XBLA_Retail.zip` is the Xbox 360 release. The game can't use it.

**Keep it private.** Copy the cart image only into a build's `Data/` folder, which git ignores (`build_pc/`, `build_p2/` and `*.z64` are all in `.gitignore`). Never commit it, attach it to a PR or a release, put it in an artifact, a zip or an evidence tarball, or send it anywhere outside the session. The same goes for anything extracted from it.

**Build and run on Linux (about 2 minutes on 4 cores in a fresh container).** These are the steps from `.github/workflows/coop-missions.yml`. Run them as root from the repository root:

    dpkg --add-architecture i386 && apt-get update
    apt-get install -y cmake gcc-multilib g++-multilib \
      libx11-dev:i386 libxext-dev:i386 libxrandr-dev:i386 libxcursor-dev:i386 \
      libxfixes-dev:i386 libxi-dev:i386 libxss-dev:i386 libasound2-dev:i386 \
      libudev-dev:i386 libgl-dev:i386 xvfb xauth libgl1:i386 libgl1-mesa-dri:i386 libglx-mesa0:i386
    # then build 32-bit SDL2 into ~/sdl2-linux-i686 exactly as the "Build SDL2 from source (i686)" step does
    cmake -B build_pc -DCMAKE_BUILD_TYPE=Release -DGE_VIDEO=ON -DSDL2_LINUX_ROOT="$HOME/sdl2-linux-i686"
    cmake --build build_pc -j$(nproc)
    mkdir -p build_pc/mock build_pc/Data
    gcc -m32 -shared -fPIC -O1 -o build_pc/mock/libsteam_api.so coop/mock/steam_mock.c
    cp <the file found above> build_pc/Data/baserom.u.z64

The game needs no extracted assets; the cart image in `Data/` is enough.

**Then run:**
- Two copies over the mock Steam, one or more missions: `PAR=2 coop/ci/mission_runs.sh build_pc out dam`. Each stage gets a PASS or FAIL line in `out/summary.txt`.
- The test checklist rows, and pairs, parties of 3 or 4 and solo runs with scripted steps: see `coop/ci/checklist/README.md`.
- One screenshot of one copy, with no Docker (this is how the UI gate's cells are taken; change the env and the window size per cell):

      cd build_pc
      Xvfb :21 -screen 0 640x480x24 >/dev/null 2>&1 & X=$!; sleep 1
      DISPLAY=:21 SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GE_SKIPINTRO=1 GE_NOMSGBOX=1 GE_NOINISAVE=1 \
        GE_STAGE=dam GE_SPLIT=4 GE_COOP=local GE_LANG=es timeout 60 ./ProjectMidas > run.log 2>&1 & G=$!
      sleep 45; DISPLAY=:21 import -window root split4-es.png    # import is in the imagemagick package
      wait $G; kill $X

  Wait for the game's own pid: a bare `wait` also waits for Xvfb and never returns. The log should say `GE_SPLIT: 4 local players` and `this run: es (GE_LANG)`.
- Lobbies, rejoins, versus and two-copy screenshots: the scripts in `coop/docker/`. They expect the `gerun` image (`coop/docker/Dockerfile.run` on top of the root `Dockerfile`), with the repository at `/home/dev/project` and an output folder at `/out`. To run one without Docker, change those two paths.

Keep `PAR` at most half the cores. The rig uses software GL under Xvfb, so the game runs at about 20 fps (see "Testing" below).

## Never

- Never commit ROM data, a cart image or extracted game assets.
- Never add AI-made images, textures, models, sound or music. Code only.
- Never put claude.ai session or chat links in commit messages, PR descriptions, release notes or code. Never add a `Claude-Session:` trailer to a commit. A plain "Generated with Claude Code" credit with no link is fine.
- Never change single player unless that is the point of the change.
- Never change what goes over the wire without raising `COOP_VERSION`, and talk about it in an issue first.
- Never call a check passed when it didn't run, or ran on an older build. Say "not run".

## Lessons

### Networking

- **A change to what a player sends must keep sending everything it sent before.** Check every weapon, item and event type that goes through the code you touched. *v0.6.11 stopped sending a joiner's tank shells, rockets and grenade-launcher rounds to the host, so they did no damage until v0.6.12.*
- **Every update applied from the network must ignore one that is older than what it already has.** Packets arrive out of order. *Before v0.2.1, a late guard update restarted animations, fired bursts again and flipped guards between hidden and dying.*
- **Test with packet loss and jitter, not only on a clean local link.** Use `NETSIM` in `coop/ci/mission_runs.sh`. *A lost join snapshot packet left a joiner waiting forever. It was only found once runs used 2% loss.*
- **Anything a reconnect, rejoin or host change touches needs its own test.** *Before v0.6.12, everything a rejoined player sent was dropped for the rest of the mission. Before v0.7.0, a long dropout split one game into two with two hosts.*
- **When `COOP_VERSION` stays the same, the new build has to work with the older builds on that number, both as host and as joiner.** Test it or write in the notes what wasn't tested. *An older host never sends a TALLY, so a newer joiner had to keep the old debrief flow.*

### Host and joiner

- **The host owns the world. A joiner's own copy of the level scripts must not decide anything the host decides.** That covers counters, chr bitfields, timers, objectives and failures. Before you let joiner-side code write shared state, check whether the host already sends it. *QA-10: a joiner's scripts kept their own chr bitfields, so Streets' casualty count could fail the mission on the joiner alone (1 run in 6). QA-7: a joiner's script checked its own countdown clock, which stood at 0, so on Aztec the shuttle launched with no countdown on either screen.*
- **Things that only last a moment have to reach joiners correctly both ways:** shown while they exist, gone once they don't. Check short-lived guards, bodies, effects and drops on a slow host. *QA-9: Surface 2's alarm guards remove themselves at once, but joiners kept each one for a second and showed guards the host never had. QA-11: a slow host's joiners missed a body's fade because it wasn't in the first snapshots of the fade.*
- **A takeover (a joiner becoming host) has to cope with things caught halfway through a change,** such as a guard dying, a door moving or a cutscene playing. Test a takeover at those moments. *#119: a joiner that took over crashed on a guard the old host had shown dying, because its death data was left over from an earlier action.*

### Story scripts and "Bond"

- **Every check on "Bond" has to think about every player.** That covers Bond is dead, in a room, on screen, near a pad, holding an item, and who did something. The bug families in `coop/TEST-CHECKLIST.md` list the usual cases. *In v0.1.0, "if Bond is dead" stopped a level script for good when the host went down once, so Bunker 2's exit never ran. Before v0.8.0, one player's missed throw on Dam failed the whole mission.*
- **Test with the joiner doing the work while the host stays back.** That is how one-player checks show up.
- **Objective credit goes to whoever actually killed or broke the target, grenades, mines and explosions included.** *This was wrong twice: fixed in v0.6.14, then again in v0.8.3.*
- **State that lives across missions must be cleared when a mission starts** (`coopStageStart`). The menus run no stage ticks. *The joiner's "host's intro is over" flag was never cleared, so replaying a mission ended the joiner's intro at once.*

### HUD, menus and text: the UI gate

The first UI rules here only said "check". Overlaps and misplaced HUD items still got through, because nothing showed that the check had been done, and the owner found them by looking at screenshots. *During the v0.9.0 split screen work: the ammo counter sat over the gun hand in 3 and 4 players, the objective list covered the radar, and the radar drifted away from the list. Then v0.9.0 shipped with one Weapon Menu shared by every player on the screen (#160).* So a UI change now has to show its proof.

**Any PR that changes something drawn on screen (HUD, menus, the watch, toasts, name tags, text) must attach a screenshot matrix** to the PR, or say which cells couldn't be taken and why. A cell nobody looked at is a fail, not a pass.

| Layout | How to get it |
| --- | --- |
| Solo, full screen | `GE_STAGE=<stage>` |
| Split screen, 2, 3 and 4 players | `GE_SPLIT=2`, `3`, `4` with `GE_COOP=local` |
| Online with a partner | two copies over the mock Steam (`coop/docker/two_player_split.sh`, `coop/ci/checklist/pair.sh`) |
| Mixed: split screen plus an online partner | two copies, one of them with Local Players at 2 (`GE_COOP_LOCALS=2` in the lobby) |

Take each layout:
- in English and in Spanish (`GE_LANG=es`),
- at a small and a large window (Xvfb `-screen 0 640x480x24` and `1920x1080x24`),
- in the Modern and the Classic co-op menu, when menus changed,
- with the HUD at its busiest: the dialogue bar showing, the objective list, the radar, partner rows, a stack of notifications, a REVIVE tag and the weapon menu open, all at once.

Look at every player's view in every shot, not only player 1's. For each shot, write one line in the PR saying what you checked: nothing overlaps, nothing is cut off, nothing runs off its view, and each item sits in that view's own corner.

**Then the rules each shot is checked against:**
- **Place HUD items against each player's own view,** not the whole screen or the 4:3 band. Don't reuse Rare's 3- and 4-player positions for new items. *The ammo counter used Rare's values and sat over the gun hand. The radar stayed in the 4:3 band while the list hugged the view's edge.*
- **When two items are drawn at different scales, work out offsets in each one's own scale.** *The list was drawn at half size in split screen and the radar at full size, so the gap meant to clear the radar came out half as tall.*
- **A new item must not overlap what is already there:** the dialogue bar, objectives, the radar, partner status, notifications, crosshair hints and name tags. *v0.8.6 had to move objectives, the radar and partner status out from under the dialogue bar. A tall stack of notifications can still reach the give hint.*
- **Spanish runs longer than English.** Every new or changed string is checked in Spanish at the small window. *v0.8.6 fixed cut-off Spanish objectives, missing letters (q, í) and EFECTOS running under its volume bar.*
- **Accented text is checked on real Windows,** not only on Linux. *Spanish accents drew blank on Windows until v0.8.1.*
- **Anything drawn from a list that grows (chat, notifications, the party list) has a cap on what is drawn.** *Opening chat with a full history crashed the game on Facility until v0.8.2.*

**Screenshots don't catch UI that takes input. That needs a second test.** For every UI item a player can open, press or scroll, drive it from player 2 (and 3 and 4) while player 1 does something else. Then check that only that player's view and that player's game changed. If no test switch can drive it yet, add one, as #160 did with `GE_WEAPMENU_TEST`, and a cart-free test in `coop/ci/` when the logic allows it. *The Weapon Menu bug never showed in a screenshot, because the menu looked right in each view. It only showed when one player opened it and the other fired.*

### Split screen (several players on one PC)

- **Any state that belongs to a player must be kept per player, not once per game or once per screen.** That covers menus, HUD items, input, highlights, timers, cameras and toasts. When you touch code with a `static` or global that a player's input or HUD reads or writes, ask "what if two players on this screen use this at once?" Test with two local players doing different things at the same moment. *v0.9.0 shipped with one Weapon Menu shared by every player on the screen. One player opening it opened it in every view, and the other player's next shot picked a weapon instead of firing (#160). Then v0.9.1 shipped with one tank state for the whole screen: when one player climbed into Runway's tank, every view was "in the tank", the other players were pulled into the hatch and their sticks steered it. The tank globals in bondview.c and bondview2.c are now swapped per player (`src/game/split_tank.c`).*
- **Engine code written for multiplayer can do things story mode never expects.** Read what a multiplayer path does before you reuse it in a story mission. *Rare's multiplayer code picks a new "first player" every tick. In split screen story missions that left the sky and most of the scenery black in every view, in about 1 run in 4. Its objTick moves a projectile only in its owner's pass, assuming every owner is a player, so a guard's drop (Facility's key card) hung in the air in split screen in v0.9.0 and v0.9.1.*
- **Split screen and online can be mixed (2+1, 1+2, 2+2 players on two PCs).** Test a change in at least one mixed game as well as offline split.

### Testing

- **The two-copy rig runs at about 20 fps under Xvfb, so it hides bugs that only show at 60 fps or more.** Anything that depends on frame rate or timing needs a cart-free test (like `coop/ci/interp_test.c`) or a run at full speed. *Before v0.2.1, partners hitched and slid at 60 fps on a jittery link, and the two-copy runs never showed it: the 8 samples kept covered 400 ms at 20 fps but only 133 ms at 60 fps.*
- **A green CI job that ran nothing is not a pass.** Open the log and look for the result lines. *Until 2026-11-01, the mission runs had a missing secret and skipped in 4 to 11 seconds with a green check. All 225 runs up to then had never tested anything.*
- **A fix that goes in late means re-running the checks it could affect,** or saying in the notes which checks ran before it. *The grenade landing fix came late in v0.8.3, and the objective checks for Control, Caverns, Jungle, Depot and Facility were not run again. That note is still in the known issues in v0.8.5.*
- **One clean run proves little. Many bugs here only show sometimes.** Run a check several times (8 or more for a bug seen 1 time in 4) before and after the fix, and give the counts. *The black scenery showed in 1 run in 4, the Streets casualty failure in 1 in 6, and a missed body fade in 1 in 10.*
- **A fix comes with a test that fails on the old code and passes on the new.** Use a cart-free test in `coop/ci/` that CI runs when you can, and a rig line when you can't. *#160's two-player Weapon Menu test fails 6 of 8 checks on v0.9.0 and passes all 8 with the fix.*
- **A cart-free test doesn't replace a run of the real game.** Run the game too (see "Running the real game"). When you can't, say exactly what didn't run.
- **A result from an older build never counts for the new one.** Keep it under "Earlier:" in the checklist, as `coop/TEST-CHECKLIST.md` says.
- **A rig FAIL is a lead, not a verdict.** Read the logs first, since a step that missed its target also fails a check. Keep `PAR` at most half the cores.

### Build

- **The game builds with `-w`, so warnings are hidden.** A function used without its declaration compiles as one returning an int and gives garbage. Build co-op files with warnings (`coop/ci/warnings_check.sh`) and look for `-Wimplicit-function-declaration`. *`atan2f`, `fmodf` and `fabsf` had no declaration, so "the partner aims at this guard", the give-to-partner height check and the connection jitter readout were all noise.*
- **New co-op code must not add warnings.** The ratchet in `coop/ci/warnings-baseline.txt` fails CI if it does. When you remove some, lower the baseline with `--update`.
- **Both Linux and Windows must build, and the headless build too.** *The headless (no SDL) build stopped linking once and had to be fixed.*

### Updates and the release zip

- **Any new file or folder in the zip has to get through older builds' updaters.** An updater only installs what its own `portUpdPathAllowed` allows. Check what v0.7.1 and older do with it, not only the new build. *v0.8.0 added `lang/`, and updating from v0.7.1 or older left it out, so those players only had English until the new build put the missing files in itself (#141).*
- **Test the update from the last release, not only a fresh unzip.** The CI tests in `coop/ci/update_test.c` and `update_resume_test.c` cover the file rules. They don't cover a real in-game update on Windows, so say whether you did one.

## Pull requests and commits

- Fill in `.github/pull_request_template.md`: the goal, what changed, how it was tested, and the AI boxes.
- Say which side you tested (host, joiner or both), how (two PCs over Steam, or two copies over the mock Steam), which missions or menus, and how many players.
- Say who needs the change: the host only, the joiner only, or every player.
- List what you did not test. An honest "not run" is better than a guess.
- One focused change per PR.
- End commit messages and PR descriptions without any claude.ai link and without a `Claude-Session:` trailer.

## Release checklist

Go through this on the exact commit that will be tagged. If an item can't be done, write it under "Known issues" in the release notes. Don't skip it quietly.

**Version and network**

- [ ] Decide whether `COOP_VERSION` changes. If it does, every player must update, and the notes say so at the top.
- [ ] If it stays the same, the new build was tried with the oldest build on that number, as host and as joiner, or the notes say it wasn't.

**CI**

- [ ] build-linux, build-windows and sanitize-linux are green on the release commit.
- [ ] The mission runs (`coop-missions.yml`) ran for real: the log has a result line for every stage. A skip doesn't count.
- [ ] The warnings ratchet passes.

**Play testing**

- [ ] The rows of `coop/TEST-CHECKLIST.md` that the changes touch were run again on this build, with the joiner doing the work.
- [ ] Anything fixed late was followed by a re-run of the checks it could affect.
- [ ] At least one mission played start to finish with two players, host and joiner both checked. Three or four players if the change touches the party, scaling or the HUD.
- [ ] Split screen offline (2, 3 and 4 players) and one mixed game (split screen plus an online partner) played on the release build.
- [ ] A bug that only showed sometimes was run enough times after its fix, and the notes give the counts.
- [ ] Tested with packet loss and jitter (`NETSIM`) for anything that touches the network.
- [ ] Single player still plays as before.
- [ ] Versus still works if shared code changed.

**Screens and text**

- [ ] Every PR in the release that changed something on screen has its screenshot matrix (see "the UI gate"), with every cell looked at or marked as not taken.
- [ ] One more matrix on the release build itself, with the HUD at its busiest, because PRs that each look fine can overlap once together.
- [ ] Every UI item that takes input was driven by player 2 while player 1 did something else, in split screen.
- [ ] Accents checked on real Windows, or listed as not checked.

**Zip and updates**

- [ ] The zip has everything the build needs: the exe, `steam_api.dll`, `lang/`, the README. No cart image and no game assets.
- [ ] An update from the previous release brings every new file. Check `portUpdPathAllowed` in the older builds that will do the update.

**Notes**

- [ ] `coop/release-notes/<tag>.md` is written: which versions it works with, who needs it, what's new, what's fixed, and known issues, including anything not tested.
- [ ] Each fix says which side needs it.
- [ ] A row is added to `CHANGELOG.md` with the date and the co-op protocol number.
- [ ] No claude.ai links anywhere in the notes, the commits or the PR.
