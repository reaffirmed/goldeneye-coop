# Changelog

GoldenEye Coop releases, newest first. Each release's full notes, including which players need it and what is still open, are in [`coop/release-notes/`](coop/release-notes/) and on the [Releases](https://github.com/reaffirmed/goldeneye-coop/releases) page.

"Co-op protocol" is the network version. Players can only play together on releases with the same number; the game offers the update when they differ.

| Release | Date | Co-op protocol | Highlights |
| --- | --- | --- | --- |
| [v0.7.1](coop/release-notes/v0.7.1.md) | 2026-11-03 | 17 | The update offer comes when the game opens; Surface 2 alarm guards, Streets casualty failure and body fades fixed for joiners. |
| [v0.7.0](coop/release-notes/v0.7.0.md) | 2026-11-02 | 17 | Everyone must update. A long dropout no longer splits the game in two; saves and updates survive a crash; clearer first-run messages. |
| [v0.6.15](coop/release-notes/v0.6.15.md) | 2026-11-01 | 16 | Escorts follow one player; big fights no longer freeze everyone; versus drops the hit marker and hit-direction arc. |
| [v0.6.14](coop/release-notes/v0.6.14.md) | 2026-10-31 | 16 | Objective credit goes to whoever did it; no arrivals during an ending; players face where they aim. |
| [v0.6.13](coop/release-notes/v0.6.13.md) | 2026-10-30 | 16 | Partners fade out up close; modern captions; the game asks at launch when a new version is out. |
| [v0.6.12](coop/release-notes/v0.6.12.md) | 2026-10-30 | 16 | Rejoining after a crash works; drop-ins see what is lying about; tank fixes for joiners. |
| [v0.6.11](coop/release-notes/v0.6.11.md) | 2026-10-30 | 16 | Joiners see guards' shots land, left-hand guns, falling drops and hats, and planes and helicopters as the host does. |
| [v0.6.10](coop/release-notes/v0.6.10.md) | 2026-10-30 | 16 | The host's console and gadget presses reach joiners; a partner's tank runs guards over; guard hats, yelps and thuds on the joiner. |
| [v0.6.9](coop/release-notes/v0.6.9.md) | 2026-10-29 | 16 | Guards' grenades go off on the joiner's screen. |
| [v0.6.8](coop/release-notes/v0.6.8.md) | 2026-10-29 | 16 | A shot, thrown or dropped item moves the same for every player. |
| [v0.6.7](coop/release-notes/v0.6.7.md) | 2026-10-29 | 16 | Online versus: chat, toasts, the weapon menu and the host's rules for everyone; leaving a match no longer ends it. |
| [v0.6.6](coop/release-notes/v0.6.6.md) | 2026-10-29 | 15 | Guard animations smooth on the joiner (patrols, far-off and idle guards). |
| [v0.6.5](coop/release-notes/v0.6.5.md) | 2026-10-29 | 14 | Running guards no longer look choppy on the joiner. |
| [v0.6.4](coop/release-notes/v0.6.4.md) | 2026-10-29 | 14 | Votes work again; guards and partners smooth above 60 fps. |
| [v0.6.3](coop/release-notes/v0.6.3.md) | 2026-10-29 | 14 | Statue: Janus's guards no longer open fire because of an absent partner. |
| [v0.6.2](coop/release-notes/v0.6.2.md) | 2026-10-29 | 14 | Guards hit a partner as often as the host; same guard faces and partner guns on every screen. |
| [v0.6.1](coop/release-notes/v0.6.1.md) | 2026-10-29 | 14 | Smarter guards paused: every mission plays with the Classic guards. |
| [v0.6.0](coop/release-notes/v0.6.0.md) | 2026-10-28 | 14 | Dynamic guards (Smarter AI): alert levels, suppression, grenades and callouts. |
| [v0.5.3](coop/release-notes/v0.5.3.md) | 2026-10-28 | 14 | Accessibility in Options; Smarter guards hide from every player. |
| [v0.5.2](coop/release-notes/v0.5.2.md) | 2026-10-28 | 14 | Classic co-op menu; All Options from the CO-OP screen. |
| [v0.5.1](coop/release-notes/v0.5.1.md) | 2026-10-28 | 14 | Friendly Fire, drone guns, unseen checks and mission countdowns fixed for joiners and host changes. |
| [v0.5.0](coop/release-notes/v0.5.0.md) | 2026-10-28 | 14 | Optional Smarter guard AI; online versus plays properly; Statue and Facility fixes for joiners. |
| [v0.4.0](coop/release-notes/v0.4.0.md) | 2026-10-27 | 13 | Player Scaling and Friendly Fire levels; cameras and guards react to every player. |
| [v0.3.0](coop/release-notes/v0.3.0.md) | 2026-10-27 | 12 | In-game updater, spectator camera, mission and cutscene votes. |
| [v0.2.1](coop/release-notes/v0.2.1.md) | 2026-10-27 | 12 | Players far apart stay in step; connection readout. |
| [v0.2.0](coop/release-notes/v0.2.0.md) | 2026-10-27 | 12 | Lives, respawn and bleed-out rules; cutscenes show your own character; new CO-OP screen. |
| [v0.1.0](coop/release-notes/v0.1.0.md) | 2026-10-26 | 11 | First release: 2-4 player story co-op and versus over Steam. |

## Project Midas 1.0.2b

GoldenEye Coop is a fork of [Project Midas](https://github.com/murkantor/Project-Midas) 1.0.2b, and every release above is built on it. Its changes in that version are kept below for reference; later Project Midas releases are listed in [its own repository](https://github.com/murkantor/Project-Midas).

### New
- Added a Weapon FOV option to keep the gun at its normal size at any FOV.
- Added a HUD Scale option.
- Added an Armor Pickup HUD option.
- Added a Setting Help box that explains the selected option.
- Added an FPS counter option.
- Added an option to mute audio when the window isn't focused.
- Added an option to keep controllers working when the window isn't focused.
- Added an option to stop Aim + Down from crouching.
- Added look up/down settings for mouse and keyboard.
- Added bindable watch up/down keys.
- Added Fast Pause for a faster watch.
- Added support for Project64 save files.

### Fixed
- Fixed auto aim not centring when pressing aim.
- Fixed the Dam truck getting stuck.
- Fixed Bond getting stuck on guards and objects at high frame rates.
- Fixed crackling and uneven gunfire audio (thanks JayBeest).
- Fixed audio not starting on some systems.
- Fixed the crosshair moving in big steps.
- Fixed aim and sniper zoom stuttering at high frame rates.
- Fixed debris, glass and screen animations stuttering at high frame rates.
- Fixed the Wide HUD drifting.
- Fixed Classic Mode on the end credits.
- Fixed Classic Mode N64 AA turning off other Classic Mode parts.
- Fixed level scripts not starting after skipping the intro swirl.
- Fixed the watch inventory skipping items with keys or the D-pad.
- Fixed zoom and lean controls with N64-style pads (thanks JayBeest).
- Fixed target time cheats not saving with Unlock All.
- Fixed building with GCC 15 (thanks Paul Grandperrin).
- Fixed high memory use at startup.
