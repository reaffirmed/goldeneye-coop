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
#include <io.h>
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
    int tries;

    /* a virus scanner or the search indexer can hold the old file open for a
     * moment: try again before giving up */
    for (tries = 0; tries < 5; tries++)
    {
        if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            return 1;
        }
        Sleep(20);
    }
    return 0;
#else
    return rename(from, to) == 0;
#endif
}

/* everything written so far on its way to the disk, not just to the OS */
static int saveSync(FILE *f)
{
    if (fflush(f) != 0)
    {
        return 0;
    }
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

/* 1 only when every byte made it */
static int saveCopy(const char *from, const char *to)
{
    char buf[8192];
    FILE *in = fopen(from, "rb");
    FILE *out;
    size_t n;
    int ok = 1;

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
        if (fwrite(buf, 1, n, out) != n)
        {
            ok = 0;
            break;
        }
    }
    if (ferror(in))
    {
        ok = 0;
    }
    fclose(in);
    if (fclose(out) != 0)
    {
        ok = 0;
    }
    return ok;
}

/* ---- whole-file writes ------------------------------------------------------ */

#define SAVE_WRITES_MAX 8

static struct
{
    FILE *f;
    char path[1300];
} sWrites[SAVE_WRITES_MAX];

FILE *portFileWriteBegin(const char *path, const char *mode)
{
    char tmp[1320];
    FILE *f;
    int i;

    if (path == NULL || strlen(path) >= sizeof(sWrites[0].path))
    {
        return NULL;
    }
    for (i = 0; i < SAVE_WRITES_MAX && sWrites[i].f != NULL; i++)
    {
    }
    if (i == SAVE_WRITES_MAX)
    {
        portConLogf("ge: saves: too many files open to write %s\n", path);
        return NULL;
    }
    snprintf(sWrites[i].path, sizeof(sWrites[i].path), "%s", path);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, mode);
    if (f == NULL)
    {
        portConLogf("ge: saves: can't write %s\n", tmp);
        return NULL;
    }
    sWrites[i].f = f;
    return f;
}

int portFileWriteEnd(FILE *f)
{
    char tmp[1320];
    int i, ok;

    for (i = 0; f != NULL && i < SAVE_WRITES_MAX && sWrites[i].f != f; i++)
    {
    }
    if (f == NULL || i == SAVE_WRITES_MAX)
    {
        return 0;
    }
    sWrites[i].f = NULL;
    ok = !ferror(f) && saveSync(f);
    ok = (fclose(f) == 0) && ok;
    snprintf(tmp, sizeof(tmp), "%s.tmp", sWrites[i].path);
    if (!ok || !saveReplace(tmp, sWrites[i].path))
    {
        portConLogf("ge: saves: couldn't save %s (the old one is kept)\n", sWrites[i].path);
        remove(tmp);
        return 0;
    }
    return 1;
}

void portFileWriteAbort(FILE *f)
{
    char tmp[1320];
    int i;

    for (i = 0; f != NULL && i < SAVE_WRITES_MAX && sWrites[i].f != f; i++)
    {
    }
    if (f == NULL || i == SAVE_WRITES_MAX)
    {
        return;
    }
    sWrites[i].f = NULL;
    fclose(f);
    snprintf(tmp, sizeof(tmp), "%s.tmp", sWrites[i].path);
    remove(tmp);
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

FILE *portSaveOpenWrite(const char *name, const char *kind, int version)
{
    char path[1300];
    FILE *f;

    saveEnsureDir();
    snprintf(path, sizeof(path), "%s", portSavePath(name));
    f = portFileWriteBegin(path, "w");
    if (f != NULL)
    {
        fprintf(f, SAVE_HEADER "%s %d %s\n", kind, version, PORT_BUILD_COMMIT);
    }
    return f;
}

int portSaveCommit(FILE *f)
{
    return portFileWriteEnd(f);
}

/* ---- backups -------------------------------------------------------------- */

/* not copied into a backup: the build stamp, and a .tmp a killed write left
 * behind (portFileWriteBegin starts that file over, so it is never needed) */
static int saveSkipName(const char *name)
{
    size_t len = strlen(name);

    return strcmp(name, "build.txt") == 0 || (len > 4 && strcmp(name + len - 4, ".tmp") == 0);
}

/* every plain file in dir, copied into to; the count. *failed counts the
 * ones that couldn't be copied whole */
static int saveCopyDir(const char *dir, const char *to, int *failed)
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
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !saveSkipName(fd.cFileName))
        {
            snprintf(from, sizeof(from), "%s/%s", dir, fd.cFileName);
            snprintf(dst, sizeof(dst), "%s/%s", to, fd.cFileName);
            if (saveCopy(from, dst))
            {
                n++;
            }
            else
            {
                (*failed)++;
            }
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
        if (stat(from, &st) == 0 && S_ISREG(st.st_mode) && !saveSkipName(e->d_name))
        {
            snprintf(dst, sizeof(dst), "%s/%s", to, e->d_name);
            if (saveCopy(from, dst))
            {
                n++;
            }
            else
            {
                (*failed)++;
            }
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
    int n, i, failed = 0;
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
    n = saveCopyDir(portRootPath(SAVE_DIR), dir, &failed);
    for (i = 0; i < (int) (sizeof(sLegacy) / sizeof(sLegacy[0])); i++)
    {
        char to[1500];
        FILE *probe = fopen(portRootPath(sLegacy[i]), "rb");

        if (probe == NULL)
        {
            continue;   /* most installs never had it */
        }
        fclose(probe);
        snprintf(to, sizeof(to), "%s/%s", dir, sLegacy[i]);
        if (saveCopy(portRootPath(sLegacy[i]), to))
        {
            n++;
        }
        else
        {
            failed++;
        }
    }
    if (failed > 0)
    {   /* a half backup is no backup: keep the old ones, and try again next
         * launch (build.txt stays as it was) */
        portConLogf("ge: saves: new build (%s, last %s): %d file(s) couldn't be backed up to %s (disk full?); "
                    "will try again next start\n", PORT_BUILD_COMMIT, last[0] != '\0' ? last : "unknown", failed, dir);
        saveRemoveDir(dir);
        return;
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
    f = portFileWriteBegin(portSavePath("build.txt"), "w");
    if (f != NULL)
    {
        fprintf(f, "%s\n", PORT_BUILD_COMMIT);
        portFileWriteEnd(f);
    }
}
