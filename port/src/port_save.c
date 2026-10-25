/**
 * Versioned save files: see port/include/port_save.h.
 *
 * Data/Saves/build.txt names the build (its commit) that last ran with these
 * saves. When another build starts, it first copies every file in
 * Data/Saves, plus the co-op files older builds kept beside the game, to
 * Data/Saves/Backups/<that build>-<date>/, and keeps the newest SAVE_BACKUPS
 * backups. Going back to an older zip is then a matter of copying a backup
 * back.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#endif
#include "port_save.h"
#include "port_buildinfo.h"

extern const char *portRootPath(const char *name);
extern void portConLogf(const char *fmt, ...);

#define SAVE_DIR      "Data/Saves"
#define SAVE_HEADER   "#ge-save "
#define SAVE_BACKUPS  5
#define SAVE_OPEN_MAX 4

/* files older builds kept beside the game; the backup takes them too */
static const char *const sLegacy[] = { "coop_campaign.txt", "coop_character.txt" };

static void saveMkdir(const char *path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static void saveEnsureDir(void)
{
    saveMkdir(portRootPath("Data"));
    saveMkdir(portRootPath(SAVE_DIR));
}

const char *portSavePath(const char *name)
{
    static char path[1300];

    snprintf(path, sizeof(path), "%s/%s", portRootPath(SAVE_DIR), name);
    return path;
}

/* replace to with from (from is gone afterwards) */
static int saveReplace(const char *from, const char *to)
{
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(from, to) == 0;
#endif
}

static int saveCopy(const char *from, const char *to)
{
    char buf[8192];
    FILE *in = fopen(from, "rb");
    FILE *out;
    size_t n;

    if (in == NULL)
    {
        return 0;
    }
    out = fopen(to, "wb");
    if (out == NULL)
    {
        fclose(in);
        return 0;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    {
        fwrite(buf, 1, n, out);
    }
    fclose(in);
    fclose(out);
    return 1;
}

/* ---- reading -------------------------------------------------------------- */

FILE *portSaveOpenRead(const char *name, const char *legacy, int *version)
{
    char path[1300];
    char line[160];
    FILE *f;

    *version = 0;
    snprintf(path, sizeof(path), "%s", portSavePath(name));
    f = fopen(path, "r");
    if (f == NULL && legacy != NULL)
    {
        char old[1300];

        snprintf(old, sizeof(old), "%s", portRootPath(legacy));
        f = fopen(old, "r");
        if (f != NULL)
        {   /* an older build's place: move it in with the other saves */
            fclose(f);
            saveEnsureDir();
            if (saveReplace(old, path))
            {
                portConLogf("ge: saves: moved %s into %s\n", legacy, SAVE_DIR);
                f = fopen(path, "r");
            }
            else
            {
                f = fopen(old, "r");    /* read it there; the next write lands in Data/Saves */
            }
        }
    }
    if (f == NULL)
    {
        return NULL;
    }
    if (fgets(line, sizeof(line), f) != NULL && strncmp(line, SAVE_HEADER, strlen(SAVE_HEADER)) == 0)
    {
        char kind[32];
        int v = 0;

        if (sscanf(line + strlen(SAVE_HEADER), "%31s %d", kind, &v) == 2 && v > 0)
        {
            *version = v;
        }
    }
    else
    {
        fseek(f, 0, SEEK_SET);  /* no header: written before saves had versions */
    }
    if (*version == 0)
    {
        *version = 1;
    }
    return f;
}

/* ---- writing -------------------------------------------------------------- */

static struct
{
    FILE *f;
    char path[1300];
} sOpen[SAVE_OPEN_MAX];

FILE *portSaveOpenWrite(const char *name, const char *kind, int version)
{
    char tmp[1320];
    FILE *f;
    int i;

    for (i = 0; i < SAVE_OPEN_MAX && sOpen[i].f != NULL; i++)
    {
    }
    if (i == SAVE_OPEN_MAX)
    {
        return NULL;
    }
    saveEnsureDir();
    snprintf(sOpen[i].path, sizeof(sOpen[i].path), "%s", portSavePath(name));
    snprintf(tmp, sizeof(tmp), "%s.tmp", sOpen[i].path);
    f = fopen(tmp, "w");
    if (f == NULL)
    {
        portConLogf("ge: saves: can't write %s\n", tmp);
        return NULL;
    }
    fprintf(f, SAVE_HEADER "%s %d %s\n", kind, version, PORT_BUILD_COMMIT);
    sOpen[i].f = f;
    return f;
}

int portSaveCommit(FILE *f)
{
    char tmp[1320];
    int i, ok;

    for (i = 0; i < SAVE_OPEN_MAX && sOpen[i].f != f; i++)
    {
    }
    if (f == NULL || i == SAVE_OPEN_MAX)
    {
        return 0;
    }
    sOpen[i].f = NULL;
    ok = fflush(f) == 0 && !ferror(f);
    ok = (fclose(f) == 0) && ok;
    snprintf(tmp, sizeof(tmp), "%s.tmp", sOpen[i].path);
    if (!ok || !saveReplace(tmp, sOpen[i].path))
    {
        portConLogf("ge: saves: couldn't save %s (the old one is kept)\n", sOpen[i].path);
        remove(tmp);
        return 0;
    }
    return 1;
}

/* ---- backups -------------------------------------------------------------- */

/* every plain file in dir, copied into to; the count */
static int saveCopyDir(const char *dir, const char *to)
{
    char from[1300], dst[1300];
    int n = 0;
#ifdef _WIN32
    char pat[1300];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(pat, sizeof(pat), "%s/*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && strcmp(fd.cFileName, "build.txt") != 0)
        {
            snprintf(from, sizeof(from), "%s/%s", dir, fd.cFileName);
            snprintf(dst, sizeof(dst), "%s/%s", to, fd.cFileName);
            n += saveCopy(from, dst);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    struct stat st;

    if (d == NULL)
    {
        return 0;
    }
    while ((e = readdir(d)) != NULL)
    {
        snprintf(from, sizeof(from), "%s/%s", dir, e->d_name);
        if (stat(from, &st) == 0 && S_ISREG(st.st_mode) && strcmp(e->d_name, "build.txt") != 0)
        {
            snprintf(dst, sizeof(dst), "%s/%s", to, e->d_name);
            n += saveCopy(from, dst);
        }
    }
    closedir(d);
#endif
    return n;
}

static void saveRemoveDir(const char *dir)
{
    char path[1300];
#ifdef _WIN32
    char pat[1300];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(pat, sizeof(pat), "%s/*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                snprintf(path, sizeof(path), "%s/%s", dir, fd.cFileName);
                DeleteFileA(path);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
#else
    DIR *d = opendir(dir);
    struct dirent *e;

    if (d != NULL)
    {
        while ((e = readdir(d)) != NULL)
        {
            if (e->d_name[0] != '.')
            {
                snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
                unlink(path);
            }
        }
        closedir(d);
    }
    rmdir(dir);
#endif
}

/* oldest backups out, newest SAVE_BACKUPS kept (named <build>-<date>-<time>,
 * so they are ordered by the date part) */
static void savePruneBackups(const char *base)
{
    static char names[32][64];
    static char keys[32][32];
    char path[1300];
    int n = 0, i, j;
#ifdef _WIN32
    char pat[1300];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(pat, sizeof(pat), "%s/*", base);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        return;
    }
    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.' && n < 32
            && strlen(fd.cFileName) < 64)
        {
            snprintf(names[n++], 64, "%s", fd.cFileName);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(base);
    struct dirent *e;

    if (d == NULL)
    {
        return;
    }
    while ((e = readdir(d)) != NULL)
    {
        if (e->d_name[0] != '.' && n < 32 && strlen(e->d_name) < 64)
        {
            snprintf(names[n++], 64, "%s", e->d_name);
        }
    }
    closedir(d);
#endif
    for (i = 0; i < n; i++)
    {   /* the date and time after the build's name */
        const char *dash = strchr(names[i], '-');

        snprintf(keys[i], sizeof(keys[i]), "%s", dash != NULL ? dash + 1 : "");
    }
    for (i = 1; i < n; i++)
    {
        for (j = i; j > 0 && strcmp(keys[j - 1], keys[j]) > 0; j--)
        {
            char t[64];

            memcpy(t, names[j], 64);
            memcpy(names[j], names[j - 1], 64);
            memcpy(names[j - 1], t, 64);
            memcpy(t, keys[j], 32);
            memcpy(keys[j], keys[j - 1], 32);
            memcpy(keys[j - 1], t, 32);
        }
    }
    for (i = 0; i + SAVE_BACKUPS < n; i++)
    {
        snprintf(path, sizeof(path), "%s/%s", base, names[i]);
        saveRemoveDir(path);
    }
}

void portSaveBootCheck(void)
{
    char last[64] = "";
    char base[1300], dir[1400], stamp[32];
    FILE *f;
    int n, i;
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);

    saveEnsureDir();
    f = fopen(portSavePath("build.txt"), "r");
    if (f != NULL)
    {
        if (fgets(last, sizeof(last), f) == NULL)
        {
            last[0] = '\0';
        }
        fclose(f);
        last[strcspn(last, "\r\n")] = '\0';
    }
    if (strcmp(last, PORT_BUILD_COMMIT) == 0)
    {
        return;
    }
    snprintf(base, sizeof(base), "%s/Backups", portRootPath(SAVE_DIR));
    if (lt != NULL)
    {
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", lt);
    }
    else
    {
        snprintf(stamp, sizeof(stamp), "%lu", (unsigned long) t);
    }
    snprintf(dir, sizeof(dir), "%s/%s-%s", base, last[0] != '\0' ? last : "older", stamp);
    saveMkdir(base);
    saveMkdir(dir);
    n = saveCopyDir(portRootPath(SAVE_DIR), dir);
    for (i = 0; i < (int) (sizeof(sLegacy) / sizeof(sLegacy[0])); i++)
    {
        char to[1500];

        snprintf(to, sizeof(to), "%s/%s", dir, sLegacy[i]);
        n += saveCopy(portRootPath(sLegacy[i]), to);
    }
    if (n == 0)
    {
        saveRemoveDir(dir);     /* a fresh install: nothing to keep */
    }
    else
    {
        portConLogf("ge: saves: new build (%s, last %s): %d file(s) backed up to %s\n",
                    PORT_BUILD_COMMIT, last[0] != '\0' ? last : "unknown", n, dir);
        savePruneBackups(base);
    }
    f = fopen(portSavePath("build.txt"), "w");
    if (f != NULL)
    {
        fprintf(f, "%s\n", PORT_BUILD_COMMIT);
        fclose(f);
    }
}
