/**
 * port_update.h - the in-game updater (port/src/port_update.c) and the pieces
 * it is built from that need no game, files or network
 * (port/src/port_update_util.c, tested by coop/ci/update_test.c).
 *
 * The updater asks GitHub for the newest release of the co-op build, and on
 * the player's word downloads its zip, checks it against the SHA-256 GitHub
 * lists for it, swaps the program files beside the exe and starts the game
 * again. It never writes the ROM, Data/Saves, midas.ini or anything else the
 * player made: see portUpdPathAllowed.
 */
#ifndef PORT_UPDATE_H
#define PORT_UPDATE_H

#include <stddef.h>

/* ---- the updater (port_update.c) ----------------------------------------- */

enum {
    PORTUPD_OFF = 0,     /* [display] auto_update 0 and nobody asked */
    PORTUPD_IDLE,        /* not checked yet */
    PORTUPD_CHECKING,
    PORTUPD_CURRENT,     /* this is the newest release */
    PORTUPD_AVAILABLE,   /* a newer one is out: portUpdateLatest() */
    PORTUPD_DOWNLOADING, /* portUpdateProgress() percent */
    PORTUPD_INSTALLING,  /* checking the zip and swapping the files */
    PORTUPD_READY,       /* installed: portUpdateRestart() starts it */
    PORTUPD_FAILED       /* portUpdateError() says why; a check or install can be tried again */
};

void portUpdateBoot(int argc, char **argv); /* main.c: wait out the old copy, tidy up after the last update */
void portUpdateTick(void);            /* boss.c, once a frame: the startup check */
void portUpdateCheck(int force);      /* ask GitHub now (force: even with auto_update off, or checked lately) */
int  portUpdateState(void);
int  portUpdateProgress(void);        /* 0..100 while downloading */
const char *portUpdateOwn(void);      /* this build's release, "v0.2.1", or "" when it doesn't know */
const char *portUpdateLatest(void);   /* the newest release's tag once checked, else "" */
const char *portUpdateError(void);    /* why the last check or install failed */
int  portUpdateNewer(const char *tag);/* 1: tag is a newer release than ours (or ours is unknown) */
int  portUpdateHasDownload(void);     /* the newest release has a zip for this system */
void portUpdateInstall(void);         /* download and install portUpdateLatest() */
void portUpdateRestart(void);         /* start the new build and quit this one */
const char *portUpdateJustUpdated(void); /* the release an update's restart brought us to, else "" */
int  portUpdateEnabled(void);         /* [display] auto_update */

/* the launch prompt (front.c, on the Files screen): asked once a run, never for a skipped release */
enum {
    PORTUPD_ANSWER_UPDATE = 0,  /* Update Now: download and install, then restart */
    PORTUPD_ANSWER_LATER,       /* Not Now: ask again next launch */
    PORTUPD_ANSWER_SKIP         /* Skip This Version: not again until a newer one */
};
int  portUpdatePromptWanted(void);    /* 1: a newer release with a download, not skipped, not answered yet */
void portUpdatePromptAnswer(int answer);

/* ---- no game, files or network (port_update_util.c) ---------------------- */

#define PORTUPD_MAX_ASSETS 16

struct PortUpdAsset {
    char name[128];
    char url[512];      /* browser_download_url */
    char sha256[65];    /* from "digest": "sha256:<hex>", "" when GitHub gave none */
    long size;
};

struct PortUpdRelease {
    char tag[48];
    char page[256];     /* html_url */
    int nassets;
    struct PortUpdAsset assets[PORTUPD_MAX_ASSETS];
};

/* a releases/latest answer; 0 on success */
int portUpdParseRelease(const char *json, size_t len, struct PortUpdRelease *out);
/* "v1.2.3" against "v1.2.4": -1, 0 or 1; -2 when either is not a version */
int portUpdCompareVersions(const char *a, const char *b);
/* 1 when a path inside the zip (after "GoldenEye-Coop/") may be written over */
int portUpdPathAllowed(const char *rel);

void portUpdSha256(const void *data, size_t len, char hex[65]);
unsigned long portUpdCrc32(const void *data, size_t len);
/* raw deflate; returns the bytes written, or -1 on a bad stream or a full out */
long portUpdInflate(const unsigned char *in, size_t inlen, unsigned char *out, size_t outcap);

/* a zip held in memory: count the entries, then read each one */
struct PortUpdZipEntry {
    char name[260];
    int dir;                 /* a folder entry, nothing to write */
    unsigned long size;      /* uncompressed */
};
int portUpdZipCount(const unsigned char *zip, size_t len);     /* -1: not a zip we can read */
int portUpdZipEntry(const unsigned char *zip, size_t len, int i, struct PortUpdZipEntry *e);
/* the entry's bytes into out (e->size of them, CRC checked); 0 on success */
int portUpdZipExtract(const unsigned char *zip, size_t len, int i, unsigned char *out);

#endif
