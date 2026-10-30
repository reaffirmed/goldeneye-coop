# GoldenEye Coop

**Play the GoldenEye 007 story missions online with up to three friends, or take them on in the original multiplayer arenas.**

[![Latest release](https://img.shields.io/github/v/release/reaffirmed/goldeneye-coop?label=download)](https://github.com/reaffirmed/goldeneye-coop/releases/latest)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-informational)
![Players](https://img.shields.io/badge/players-1--4%20online-success)

<p>
  <img src="docs/images/ping-a-guard.png" width="49%" alt="A player marks a guard as ENEMY with a ping on Dam">
  <img src="docs/images/voice-chat.png" width="49%" alt="A partner's Bond on Dam with a name tag and a speaker icon while they talk">
</p>

GoldenEye Coop is an online co-op and versus edition of GoldenEye 007 for PC. Two to four players, each on their own PC with the whole screen, play the solo campaign together over Steam: the same guards, doors, alarms, objectives and explosions on every screen, with revives, shared pickups, voice chat and drop-in. The original multiplayer is online too.

It started as a fork of [Project Midas](https://github.com/murkantor/Project-Midas), the PC port of the GoldenEye N64 decompilation, and is still built on it. Everything Project Midas does for solo play is here; GoldenEye Coop adds the online game on top. See [Credits](#credits).

This is an unofficial fan project. It contains no game assets: every player needs their own GoldenEye 007 (USA) cart image.

## Features

### Story co-op

- **2-4 players over Steam.** Host from the CO-OP screen, invite Steam friends, or list a public game with an optional password. Nobody opens ports or types IP addresses.
- **One shared mission.** The host's game runs the level, and every player sees the same guards, doors, alarms, destroyed objects, thrown grenades and mines, dropped guns and explosions. Pickups and objectives count for the whole team.
- **Revive or respawn.** The host picks the rules: revive a downed partner, respawn on a timer, a bleed-out time, and lives per mission. If everyone is down at once, the mission fails.
- **Drop in and host migration.** Join a game already in a mission and come in beside a partner. If the host leaves or crashes, the next player takes over and the mission carries on.
- **Play as anyone.** Pick a character from the multiplayer cast, or Mission Bond. Cutscenes show each player their own character.
- **Talk and point.** Positional voice chat over Steam Voice, text chat, and a ping key that marks a spot or a guard for everyone.
- **Tuned for more players.** Guard Strength, Player Scaling for 3 and 4 players, and Friendly Fire set to Off, Reduced or Full.
- **Together after the mission.** A team stats page, then a vote on what's next: replay, next mission or back to the lobby. Skipping a cutscene is a vote too.
- **Spectate and photo mode.** Watch a partner while you're down, fly a free camera, or take pictures in any mission.

### Versus

- **The original multiplayer, online,** for 2-4 players on their own screens.
- Every arena, including Statue and Cradle, and every scenario: Normal, You Only Live Twice, The Living Daylights, The Man with the Golden Gun, Licence to Kill, and the 2 v 1, 2 v 2 and 3 v 1 team games.
- Game length and weapon sets as in the original, plus a hit marker, a hit direction arrow and a kill feed.

### Around the game

- **Steam and Discord presence.** Friends see what you're playing ("Co-op: Dam, 00 Agent (2/4)") and can join from Steam, or ask to join from Discord.
- **In-game updater.** The game checks GitHub for a new release and installs it from the CO-OP screen, checksum verified. Saves and settings are never touched.
- **Accessibility.** Sound captions with direction, colour-blind player colours, text size up to 200%, screen shake off, and toggle crouch and walk.
- **Bug reports in one key.** F8 saves a zip with the log, co-op state, settings and a screenshot, ready to paste into Discord. Crashes save a report too.
- **Keyboard and mouse, or any pad,** with Xbox, PlayStation, N64 or keyboard button prompts.
- Everything from Project Midas: widescreen, high frame rates, FOV, HUD and control options, and Project64 saves.

## Screenshots

| | |
| --- | --- |
| ![The CO-OP lobby's Game Setup tab](docs/images/lobby-game-setup.png) | ![Intro cutscene with each player seeing their own character](docs/images/cutscenes-own-character.jpg) |
| The host sets the rules on the lobby's Game Setup tab. | Cutscenes play in sync, each player as their own character. |
| ![A 2 v 2 versus match on four screens](docs/images/versus-2v2.jpg) | ![Sound captions at 150% text size](docs/images/sound-captions.png) |
| A 2 v 2 versus match, one screen per player. | Sound captions say what you heard and where it came from. |

![The host reviving a downed joiner, host screen left, joiner screen right](docs/images/revive.png)
*Reviving a partner who is bleeding out: the host's screen on the left, the joiner's on the right.*

## Download and play

1. Download `GoldenEye-Coop-win32.zip` from the [latest release](https://github.com/reaffirmed/goldeneye-coop/releases/latest) and unzip it to a folder you own (not Program Files: the game saves and updates in its own folder).
2. Put your GoldenEye 007 (USA) cart image in the `Data` folder. See `Data/README.txt` for the accepted formats.
3. Have Steam running and signed in. Nobody has to own anything on Steam: the game connects as Valve's free "Spacewar" test app, so friends see you as playing Spacewar.
4. Run `ProjectMidas.exe` and pick **CO-OP** from the main menu.

Every player in a game needs a compatible version. `README-COOP.txt` in the zip is the full player guide.

### Updating

From v0.3.0 on, the game updates itself. When a new release is out, the top of the CO-OP screen's PLAY tab shows **Update to vX - Install**; pick it, then **Restart Now**. You can also check from the OPTIONS tab. If a firewall blocks GitHub, download the new zip and unzip it over your folder.

<img src="docs/images/updater.png" width="480" alt="The PLAY tab offering an update">

### Playing co-op

1. The host picks **Host a Game** and invites friends from the **INVITE** tab, or sets the lobby to Public so anyone can find it under **FIND GAMES**.
2. Friends accept the Steam invite or pick the host's game, then **Ready Up**.
3. The host sets the rules on **GAME SETUP** (When Down, Respawn, Lives, Guards, Player Scaling, Friendly Fire, Host Leaves), then picks **Select Mission** or **Continue**. Everyone follows into the mission.

The campaign is the host's: beating a mission together counts for everyone, and target times unlock cheats as in solo.

| Key (default) | What it does |
| --- | --- |
| Middle mouse | Ping a spot or a guard |
| G | Give the partner you face ammo for your gun |
| T | Text chat |
| V | Push to talk |
| P | Photo mode |
| 1-6 | Weapon menu |
| F8 | Save a bug report |

Every key can be rebound in the controls menu.

### Playing versus

Meet in the lobby as for co-op. The host sets **Mode** to Versus on **GAME SETUP**, picks the arena, scenario, game length and weapons, then **Start Match**.

## Building from source

The build runs in Docker, which has every tool and library both builds need. You also need the GoldenEye 007 (USA) cart as `baserom.u.z64` in the repository root:

```
sha1: abe01e4aeb033b6c0836819f549c791b26cfde83
```

### On Windows: use WSL

The commands below are for a Linux shell. On Windows, run them in WSL:

1. Install WSL with Ubuntu (`wsl --install` in PowerShell), and turn on WSL integration in Docker Desktop (Settings > Resources > WSL integration).
2. Open the Ubuntu terminal and clone the repository inside WSL, not under `/mnt/c` (much faster):
   ```
   cd ~
   git clone https://github.com/reaffirmed/goldeneye-coop.git
   cd goldeneye-coop
   ```
3. Copy your cart in, for example from your Windows Downloads folder:
   ```
   cp /mnt/c/Users/<you>/Downloads/baserom.u.z64 .
   ```

### Build steps

1. Build the image (once):
   ```
   docker build --network=host --build-arg HOST_UID=$(id -u) --build-arg HOST_GID=$(id -g) -t goldeneye .
   ```
2. Start a container in the repository:
   ```
   docker run --rm -it -v $(pwd):/home/dev/project goldeneye
   ```
3. Extract the assets (once):
   ```
   ./scripts/extract_baserom.u.sh
   make convert_props convert_chrs convert_guns combine_images
   ```
4. Build for Windows (x86):
   ```
   cmake -B build_win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake -DCMAKE_BUILD_TYPE=Release -DGE_VIDEO=ON -DSDL2_MINGW_ROOT=$HOME/sdl2-mingw/SDL2-2.30.11/i686-w64-mingw32
   cmake --build build_win -j$(nproc)
   ```
5. Build for Linux:
   ```
   cmake -B build_pc -DCMAKE_BUILD_TYPE=Release -DGE_VIDEO=ON -DSDL2_LINUX_ROOT=$HOME/sdl2-linux-i686
   cmake --build build_pc -j$(nproc)
   ```

| Build | Game | Next to it |
| --- | --- | --- |
| Windows | `build_win/ProjectMidas.exe` | `SDL2.dll`, the controller files, `Data/` |
| Linux | `build_pc/ProjectMidas` | the controller files, `Data/` |

Online play also needs the 32-bit Steam API library next to the game: `steam_api.dll` on Windows (the release zips use the one from [Steamworks.NET Standalone](https://github.com/rlabrecque/Steamworks.NET), Windows-x86) or `libsteam_api.so` on Linux, plus a `steam_appid.txt` containing `480`. `coop/package/make_zip.sh` packs a Windows build into the same zip the releases ship.

To play, put a copy of `baserom.u.z64` in that build's `Data/` folder. The Linux build needs 32-bit SDL2 and OpenGL on the machine that runs it. On Ubuntu or Debian:

```
sudo dpkg --add-architecture i386
sudo apt update
sudo apt install libsdl2-2.0-0:i386 libgl1:i386
```

## Files next to the game

| Path | What it is |
| --- | --- |
| `midas.ini` | Settings. Written on first run; delete it to restore the defaults. See `midas_ini_help.txt`. |
| `Data/` | Your cart image goes here. |
| `Data/Saves/` | Save file (`eeprom.bin`) and the co-op campaign record and character pick (`coop_*.txt`). |
| `Data/Saves/Backups/` | A copy of the saves from before each new build first ran, newest five kept. |
| `Data/Profiles/` | Player profiles and lifetime multiplayer stats. |
| `Data/Logs/ge_direct.log` | The run log on Windows. |
| `Data/Crashes/`, `Data/Reports/` | Crash reports and F8 bug reports, newest ten kept. |
| `Photos/` | Pictures from photo mode. |

## Known issues

- Three and four players are less play-tested than two.
- On some Linux systems, levels draw as a few large blank triangles. Starting the game with `GE_FULLHEIGHT=0 GE_EDGEINSET=1 ./ProjectMidas` works around it.
- Each release's notes list what's still open: see [Releases](https://github.com/reaffirmed/goldeneye-coop/releases).

## Contributing

Bug reports are most useful with the F8 report zip attached. Pull requests follow [CONTRIBUTING.md](CONTRIBUTING.md), including its rules for AI-assisted changes. Fixes that aren't specific to co-op or versus belong upstream in [Project Midas](https://github.com/murkantor/Project-Midas) too, so every player of the port gets them.

The co-op design and testing notes are in [`coop/NOTES.md`](coop/NOTES.md), the release notes in [`coop/release-notes/`](coop/release-notes/), and the test checklist in [`coop/TEST-CHECKLIST.md`](coop/TEST-CHECKLIST.md).

## AI disclaimer

Claude was used in the making of this project, as it was in Project Midas. If you don't agree with this, please do not download either the executables or the source.

## Credits

GoldenEye Coop would not exist without the people below. Thank you.

- **[Project Midas](https://github.com/murkantor/Project-Midas)** by murkantor and contributors: the PC port this project is built on (forked at 1.0.2b), including the renderer integration, audio, input, options, saves and everything that makes the game run on PC.
- **[GoldenEye decompilation](https://github.com/n64decomp/007)** by KholdFuzion and contributors, which Project Midas is built from, and the **[GoldenEye docs](https://github.com/kholdfuzion/goldeneye_docs)**.
- **[GoldenEye 007 Plus](https://github.com/Joshua-1248/GoldenEye-007-Plus)** by Joshua: its split-screen campaign co-op showed how guards can choose between several Bonds. Our guard targeting is ported from it; see the provenance table in [`coop/NOTES.md`](coop/NOTES.md).
- **[Perfect Dark decompilation](https://github.com/n64decomp/perfect_dark)** by Ryan Dwyer and contributors, the **[Perfect Dark PC port](https://github.com/fgsfdsfgs/perfect_dark)** by fgsfds and contributors, and **[Perfect Dark netplay](https://github.com/murkantor/perfect_dark_netplay)** by murkantor.
- **[The DOOM III Network Architecture](https://mrelusive.com/publications/papers/The-DOOM-III-Network-Architecture.pdf)** by J.M.P. van Waveren (id Software, 2006).
- **Steamworks** by Valve, used through the Spacewar test app, and **[Steamworks.NET](https://github.com/rlabrecque/Steamworks.NET)** by Riley Labrecque for the packaged `steam_api.dll`. **Discord** Rich Presence.
- Everyone who play-tested the co-op builds and sent F8 reports.

Project Midas also credits [ENet](https://github.com/lsalzman/enet), [xdelta3](https://github.com/jmacd/xdelta), [qemu-irix](https://github.com/n64decomp/qemu-irix), [pj64raphnetraw](https://github.com/raphnet/pj64raphnetraw), the 1964 emulator and its mouse injector; see [its README](https://github.com/murkantor/Project-Midas#acknowledgements).

### Bundled third-party code

Each keeps its own license. The full license texts are in [`tools/THIRD_PARTY_NOTICES.txt`](tools/THIRD_PARTY_NOTICES.txt).

- [Fast3D renderer](https://github.com/Emill/n64-fast3d-engine) by Emill and MaikelChan (MIT), in `port/fast3d`
- [glad](https://github.com/Dav1dde/glad) by David Herberth, with the Khronos platform header by The Khronos Group (MIT-style), in `port/fast3d/glad`
- [SDL2](https://github.com/libsdl-org/SDL) by Sam Lantinga and contributors (zlib), linked at runtime
- [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) by Gabriel Jacobo and contributors (zlib), in `tools/gamecontrollerdb.txt`
- [SpectNetIDE](https://github.com/Dotneteer/spectnetide) by Istvan Novak (MIT), Z80 opcode comments in `src/game/spectrum_hw.h`
- [inflate.c](https://www.gnu.org/software/gzip/) by Mark Adler (public domain), in `src/inflate` and `src/game/zlib.c`
- [gzip 1.2.4](https://www.gnu.org/software/gzip/) by Jean-loup Gailly (GPL-2.0+), build tool in `tools/gzipsrc`
- [puff](https://github.com/madler/zlib/tree/develop/contrib/puff) by Mark Adler (zlib), build tool in `tools/extractor`
- [armips](https://github.com/Kingcom/armips) by Kingcom (MIT), build tool in `tools/armips.cpp`
- [rabbitizer](https://github.com/Decompollaborate/rabbitizer) by Decompollaborate (MIT), build tool in `tools/ido5.3_recomp/rabbitizer`
- [ido-static-recomp](https://github.com/decompals/ido-static-recomp) by Emill and decompals (no license published), build tool in `tools/ido5.3_recomp`

## License

The port code, from Project Midas and from GoldenEye Coop, is MIT licensed; see [LICENSE](LICENSE), including its "Scope" section. The license does not cover the decompiled game code, GoldenEye itself or its assets, and bundled third-party code keeps its own license. The executable and `midas.ini` still carry Project Midas's names for compatibility with existing installs and the updater; "Project Midas" is murkantor's name for their port and is not licensed.

GoldenEye 007 is owned by Rare, Nintendo, MGM, EON and the other rights holders. GoldenEye Coop is not affiliated with or endorsed by any of them, or by Valve or Discord. This project contains no game assets.
