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

### Story scripts and "Bond"

- **Every check on "Bond" has to think about every player.** That covers Bond is dead, in a room, on screen, near a pad, holding an item, and who did something. The bug families in `coop/TEST-CHECKLIST.md` list the usual cases. *In v0.1.0, "if Bond is dead" stopped a level script for good when the host went down once, so Bunker 2's exit never ran. Before v0.8.0, one player's missed throw on Dam failed the whole mission.*
- **Test with the joiner doing the work while the host stays back.** That is how one-player checks show up.
- **Objective credit goes to whoever actually killed or broke the target, grenades, mines and explosions included.** *This was wrong twice: fixed in v0.6.14, then again in v0.8.3.*
- **State that lives across missions must be cleared when a mission starts** (`coopStageStart`). The menus run no stage ticks. *The joiner's "host's intro is over" flag was never cleared, so replaying a mission ended the joiner's intro at once.*

### HUD, menus and text

- **Check every HUD or menu change in every screen layout:** full screen, two players split (both ways), and three or four players. Check every player's view, not only player 1's. *The split screen build put the ammo display in the wrong place in local split screen.*
- **Check that new HUD items don't overlap what is already there:** the dialogue bar, objectives, the radar, partner status, notifications, the crosshair hints and name tags. *v0.8.6 had to move objectives, the radar and partner status out from under the dialogue bar. A tall stack of notifications can still reach the give hint.*
- **Check every new or changed string in Spanish, at the smallest and the largest resolution.** Spanish runs longer than English. *v0.8.6 fixed cut-off Spanish objectives, missing letters (q, í) and EFECTOS running under its volume bar.*
- **Check accented text on real Windows, not only on Linux.** *Spanish accents drew blank on Windows until v0.8.1.*
- **Anything drawn from a list that grows (chat, notifications, the party list) needs a cap on what is drawn.** *Opening chat with a full history crashed the game on Facility until v0.8.2.*

### Testing

- **The two-copy rig runs at about 20 fps under Xvfb, so it hides bugs that only show at 60 fps or more.** Anything that depends on frame rate or timing needs a cart-free test (like `coop/ci/interp_test.c`) or a run at full speed. *Before v0.2.1, partners hitched and slid at 60 fps on a jittery link, and the two-copy runs never showed it: the 8 samples kept covered 400 ms at 20 fps but only 133 ms at 60 fps.*
- **A green CI job that ran nothing is not a pass.** Open the log and look for the result lines. *Until 2026-11-01, the mission runs had a missing secret and skipped in 4 to 11 seconds with a green check. All 225 runs up to then had never tested anything.*
- **A fix that goes in late means re-running the checks it could affect,** or saying in the notes which checks ran before it. *The grenade landing fix came late in v0.8.3, and the objective checks for Control, Caverns, Jungle, Depot and Facility were not run again. That note is still in the known issues in v0.8.5.*
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
- [ ] Tested with packet loss and jitter (`NETSIM`) for anything that touches the network.
- [ ] Single player still plays as before.
- [ ] Versus still works if shared code changed.

**Screens and text**

- [ ] Every HUD or menu change checked in full screen, two-player split both ways, and three and four players, in every player's view.
- [ ] Nothing new overlaps the dialogue bar, objectives, radar, partner status, notifications or name tags.
- [ ] New and changed strings checked in Spanish, at a small and a large resolution, and accents checked on Windows.
- [ ] Modern and Classic menu layouts both checked if the menus changed.

**Zip and updates**

- [ ] The zip has everything the build needs: the exe, `steam_api.dll`, `lang/`, the README. No cart image and no game assets.
- [ ] An update from the previous release brings every new file. Check `portUpdPathAllowed` in the older builds that will do the update.

**Notes**

- [ ] `coop/release-notes/<tag>.md` is written: which versions it works with, who needs it, what's new, what's fixed, and known issues, including anything not tested.
- [ ] Each fix says which side needs it.
- [ ] A row is added to `CHANGELOG.md` with the date and the co-op protocol number.
- [ ] No claude.ai links anywhere in the notes, the commits or the PR.
