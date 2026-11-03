#ifndef PORT_LANG_H
#define PORT_LANG_H
/**
 * port_lang.h - in-game languages (port_lang.c, lang/README.md).
 *
 * A language is a folder lang/<code>/ next to the exe with a language.ini,
 * and optionally the translators' own .xdelta patch for the stock (U) cart
 * (the game's own text: briefings, objectives, menus, and the accented
 * letters in its fonts) and a port.po catalogue (the port's own text: Port
 * Options, the co-op and versus screens, captions, toasts, prompts).
 *
 * Text inside the game is Latin-1: bytes 0x20..0x7E are the fonts' own
 * glyphs, 0xA0..0xFF are drawn from the language's extra glyphs, or as the
 * plain letter when it has none (textrelated.c).
 */
#include <stddef.h>
#include <stdint.h>

#define PORT_LANG_MAX 8

/* portTr(fmt) keeps the compiler checking a wrapped printf format against
 * its arguments, as if the literal were passed straight */
#if defined(__GNUC__)
#define PORT_LANG_FMTARG __attribute__((format_arg(1)))
#else
#define PORT_LANG_FMTARG
#endif

/* main.c, before romdataInit: picks the language (GE_LANG, --lang, midas.ini
 * [display] language=) and loads its catalogue */
void portLangInit(int argc, char **argv);
/* romdata.c, once the stock cart is loaded: applies the language's patch to
 * a copy and keeps its text banks and fonts */
void portLangRomLoaded(const uint8_t *rom, size_t size);

/* "en", "es", ... the language in force this run */
const char *portLangCode(void);
/* 1 = something is translated this run (catalogue or patch) */
int portLangActive(void);

/* The port's own text: the translation of `en` (trailing '\n' kept as the
 * source had it), or `en` itself. Game encoding (Latin-1). A format string
 * is only translated when the translation has the same printf conversions,
 * in the same order. */
const char *portTr(const char *en) PORT_LANG_FMTARG;
/* the same, UTF-8, for OS dialog boxes */
const char *portTrUtf8(const char *en) PORT_LANG_FMTARG;
/* textrelated.c: what a draw or a measure of `s` shows. Already-translated
 * text and game text pass through; an all-capitals string matches its
 * catalogue entry in any case and comes back in capitals. */
const char *portLangDraw(const char *s);

/* text banks: the translated copy of a bank by its ROM name (NULL = the cart's
 * own). The bytes are the patched cart's, still 1172-compressed if they were. */
const uint8_t *portLangBank(const char *name, uint32_t *size);
/* after a translated bank is rebased: its strings into the game encoding */
void portLangBankRecode(uint8_t *strings, uint32_t n);
/* a port relabel for a translated bank's slot (language.ini bank.<name>.<slot>) */
const char *portLangBankPatch(const char *bank, uint32_t slot);

/* fonts: the patched cart's font segment in the host layout the game reads
 * (0 = gothic, 1 = zurich bold), and which of its 0x21..0x7E glyphs a
 * Latin-1 byte borrows (0 = none) */
const uint8_t *portLangFontSeg(int which, uint32_t *size);
uint8_t portLangGlyphSource(uint8_t latin1);

/* the plain ASCII letter for a Latin-1 byte (0xE1 -> 'a'), for a font with
 * no glyph for it */
uint8_t portLangAsciiFallback(uint8_t latin1);

/* Port Options: the language row (key "language"); the index is into the
 * list of installed languages, English first. A change takes effect at the
 * next start. */
int portLangOptionGet(const char *key, int32_t *out);
int portLangOptionSet(const char *key, int32_t value);
int portLangIsIniKey(const char *key);
int portLangCount(void);
const char *portLangNameAt(int i);   /* game encoding, trailing '\n' */
const char *portLangCodeAt(int i);
/* 1 = the setting no longer matches the language this run started in */
int portLangRestartPending(void);

/* GE_LANG_MISSING=1: log each distinct drawn string that has no translation
 * (Data/Logs/lang-missing.txt), to find text the catalogue lacks */

/* UTF-8 -> the game encoding, into dst (always terminated) */
void portLangUtf8ToGame(const char *src, char *dst, size_t n);

#endif
