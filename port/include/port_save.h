#ifndef PORT_SAVE_H
#define PORT_SAVE_H

#include <stdio.h>

/* Versioned save files (port/src/port_save.c).
 *
 * A text save under Data/Saves starts with a header line:
 *     #ge-save <kind> <version> <build>
 * Reading gives back the version it was written with (1 for a file from
 * before versioning, which has no header), so the owner can read old
 * layouts; the next write is in the current one. A write goes to a .tmp file
 * that replaces the save only once it is complete, so a crash mid-write
 * keeps the old save. Before a newer build first touches the saves, the
 * whole folder is copied to Data/Saves/Backups/<old build>/.
 *
 * Owners: coop_stats.c (coop_campaign.txt), coop_look.c (coop_character.txt).
 * A new kind of save just picks a new file name and starts at version 1. */

/* Open name (a file name in Data/Saves) to read. *version gets the version
 * the file was written with. legacy, if not NULL, is an older place the file
 * was kept (relative to the game folder); a file found only there is read
 * from there and moved into Data/Saves. NULL when there is no such file. */
FILE *portSaveOpenRead(const char *name, const char *legacy, int *version);

/* Start writing name in Data/Saves at version: the header is written, the
 * caller writes the body and then calls portSaveCommit. NULL on failure. */
FILE *portSaveOpenWrite(const char *name, const char *kind, int version);

/* Close a file from portSaveOpenWrite and put it in place. 1 on success. */
int portSaveCommit(FILE *f);

/* The full path of a file in Data/Saves (a static buffer). */
const char *portSavePath(const char *name);

/* At start: back the saves up when this is a different build from the one
 * that wrote them last. */
void portSaveBootCheck(void);

#endif
