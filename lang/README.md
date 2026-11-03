# Languages

The game can show its text in another language: the game's own text (mission
names, briefings, objectives, the watch, menus) and the port's (Port Options and
every other settings page, the CO-OP and versus screens, captions, toasts, the
update and crash boxes). Spanish is the first one.

Players pick it under **Port Options > Language** (also in the CO-OP lobby's
options). It takes effect the next time the game starts. In `midas.ini` it is
`language=` under `[display]`; `GE_LANG=es` or `--lang es` override it for one
run.

## What is in a language

Each language is a folder `lang/<code>/` next to the game:

| File | What it is |
| --- | --- |
| `language.ini` | Its name in the language list, and which of the files below it has. |
| `<patch>.xdelta` | Optional. A fan translation patch for the GoldenEye 007 (USA) cart. |
| `port.po` | Optional. The port's own text, in the gettext PO format. |
| anything else | Whatever the patch's authors ask to ship with it (a readme, a license). |

`language.ini` keys:

| Key | What it does |
| --- | --- |
| `name` | The name shown in the Language row, in the language itself (UTF-8). |
| `patch` | The `.xdelta` file. |
| `strings` | The `.po` file. |
| `glyphs` | Pairs of an ASCII character and the letter the patch draws over it (see below). |
| `bank.<Bank>.<slot>` | A port relabel of one of the patch's strings (the English game has a few: the watch's "HUD" and widescreen values). |

## The game's text: the translation patch

The port never ships a patched cart or the game's text. At startup it reads the
player's own cart (`Data/baserom.u.z64`, the one it always needs), applies the
`.xdelta` to a copy in memory (`port/src/port_vcdiff.c`, a VCDIFF decoder), and
keeps two things from the result:

- the patched cart's text banks (`L*E`), served in place of the cart's own
  (`port/src/romdata.c`, `romdataFilesApply` and `romdataTextPreprocess`);
- its two fonts, for the letters the patch adds.

The copy is then freed. If the patch doesn't apply (another cart, a damaged
file), the log says why and the game runs in English.

N64 fan translations usually add their accented letters by drawing them over
punctuation the game never shows, since the cart's fonts are ASCII. Inside
the port, text is Latin-1, so `glyphs` says which of the patch's characters is
which letter: Spanish has `#º $í &ó /ú [á ]é {¡ |ñ }¿`. When a bank is loaded,
those characters become the Latin-1 letters, and `textrelated.c` draws a Latin-1
letter with the glyph the patch drew for it. A letter with no glyph is drawn as
its plain ASCII letter (É as E). Capitals have no glyphs of their own, so they
drop their accents, as the patch's own capitals do ("ELEGIR MISION").

Text the game builds in code (not in a bank), such as "One minute left", comes
from `port.po` instead.

## The port's text: port.po

`port.po` is a normal PO file:

```
msgid "Exit Game"
msgstr "Salir del juego"

msgid "%s left the mission."
msgstr "%s dejó la misión."
```

- The `msgid` is the English exactly as the game shows it, without its last
  line break. Write the `msgstr` in UTF-8; the game converts it.
- A string drawn only in capitals needs no entry of its own: "OPTIONS" finds
  the "Options" entry and is drawn in capitals.
- Keep every `%d`, `%s` and the like, in the same order. An entry whose
  conversions differ is skipped and logged, and the English is shown.
- A line that doesn't fit is the biggest risk: settings labels sit beside
  their value, so keep those short.
- `#, fuzzy` entries are skipped.

Most strings are matched when they are drawn (`textRender` and `textMeasure`
look each one up), so a new string only needs an entry. Strings built with
`sprintf` are translated where they are built: the format goes through
`portTr("...")`, which keeps the compiler checking it. OS dialog boxes use
`portTrUtf8`.

To find what is missing, run with `GE_LANG_MISSING=1`: each English string
drawn without a translation is written to `Data/Logs/lang-missing.txt` as an
empty PO entry, ready to fill in.

## Co-op with players in different languages

Only player-typed chat and names cross the network as text. Toasts, the kill
feed, objective messages and lobby rows are built on each player's own PC, in
their own language, from numbers and player names. The public games list
line ("Co-op: Dam") is sent in English and translated by each reader.
Objective text in toasts is read from each player's own text banks.

Steam and Discord rich presence stay in English, except a Versus arena's name,
which comes from the host's own text (without accents).

## Adding a language

1. Make `lang/<code>/language.ini` with a `name`.
2. If there is a fan translation patch for the USA cart, check its terms allow
   sharing it, add it with its readme, set `patch`, and work out `glyphs` by
   looking at what the patch draws over which characters (compare the patched
   cart's fonts with the original's).
3. Copy `lang/es/port.po`, keep the `msgid` lines and translate the `msgstr`
   lines.
4. Run with `GE_LANG=<code> GE_LANG_MISSING=1`, look over the screens, and
   check `Data/Logs/lang-missing.txt`.

The build copies `lang/` next to the game, and the release zip carries it.

## Spanish

`lang/es/GE-ESP-NTSC.xdelta` is Sogun and IlDucci's Spanish translation, v2.0,
for the USA cart. Its readme, `leeme.txt`, allows sharing it freely and
changing it as long as the readme goes with it, so it is shipped beside the
patch, unchanged. The port's Spanish (`port.po`) uses the patch's words for the
game's terms: Presa, Agente secreto, Mirilla, Autoapuntado, Salud, Escenario,
Nivel.
