/**
 * Saves and settings are replaced whole (port/src/port_save.c), never left
 * half-written, and the backup a new build takes is all or nothing:
 *
 *   gcc -O1 -Wall -Iport/include -Ibuild_pc/generated -o save_test coop/ci/save_test.c port/src/port_save.c
 *   ./save_test <an empty scratch folder>
 *
 * (build_pc/generated holds port_buildinfo.h after any configure of the game;
 * the build id itself doesn't matter here.) Linux only: a failing disk is
 * played by /dev/full and an unreadable save by /proc/self/mem.
 * Exit 1 on any failure.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "port_save.h"
#include "port_buildinfo.h"

static char sRoot[1024];
static int sFails;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); sFails++; } } while (0)

/* what main.c and port_console.c give the game */
const char *portRootPath(const char *name)
{
    static char path[4][1200];
    static int next;
    char *p = path[next++ & 3];

    snprintf(p, 1200, "%s/%s", sRoot, name);
    return p;
}

void portConLogf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    printf("  log: ");
    vprintf(fmt, ap);
    va_end(ap);
}

static char *slurp(const char *path)
{
    static char buf[4096];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (f == NULL)
    {
        return NULL;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    fputs(text, f);
    fclose(f);
}

static int exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 || lstat(path, &st) == 0;
}

static int countDirs(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int n = 0;

    if (d == NULL)
    {
        return 0;
    }
    while ((e = readdir(d)) != NULL)
    {
        n += e->d_name[0] != '.';
    }
    closedir(d);
    return n;
}

int main(int argc, char **argv)
{
    char file[1300], tmp[1320];
    const char *got;
    FILE *f;
    int version;

    if (argc < 2)
    {
        printf("usage: %s <empty scratch folder>\n", argv[0]);
        return 2;
    }
    snprintf(sRoot, sizeof(sRoot), "%s", argv[1]);
    mkdir(sRoot, 0755);

    /* 1: a plain write lands whole and leaves no .tmp */
    snprintf(file, sizeof(file), "%s/settings.ini", sRoot);
    snprintf(tmp, sizeof(tmp), "%s.tmp", file);
    put(file, "old=1\n");
    f = portFileWriteBegin(file, "w");
    CHECK(f != NULL, "write: begin");
    fputs("new=2\n", f);
    got = slurp(file);
    CHECK(got != NULL && strcmp(got, "old=1\n") == 0, "write: the old file changed before the write was done (%s)", got);
    CHECK(portFileWriteEnd(f) == 1, "write: end");
    got = slurp(file);
    CHECK(got != NULL && strcmp(got, "new=2\n") == 0, "write: got %s", got ? got : "(nothing)");
    CHECK(!exists(tmp), "write: .tmp left behind");

    /* 2: an abandoned write leaves the old file */
    f = portFileWriteBegin(file, "w");
    fputs("half=", f);
    portFileWriteAbort(f);
    got = slurp(file);
    CHECK(got != NULL && strcmp(got, "new=2\n") == 0, "abort: old file lost (%s)", got ? got : "(nothing)");
    CHECK(!exists(tmp), "abort: .tmp left behind");

    /* 3: a full disk: the write fails and the old file stays */
    CHECK(symlink("/dev/full", tmp) == 0, "full: can't fake a full disk");
    f = portFileWriteBegin(file, "w");
    CHECK(f != NULL, "full: begin");
    if (f != NULL)
    {
        int i;

        for (i = 0; i < 2000; i++)
        {
            fputs("0123456789abcdef\n", f);
        }
        CHECK(portFileWriteEnd(f) == 0, "full: a write to a full disk said it worked");
    }
    got = slurp(file);
    CHECK(got != NULL && strcmp(got, "new=2\n") == 0, "full: old file lost (%s)", got ? got : "(nothing)");
    CHECK(!exists(tmp), "full: .tmp left behind");
    unlink(tmp);

    /* 4: versioned saves keep their header and read back */
    f = portSaveOpenWrite("coop_campaign.txt", "coop-campaign", 2);
    CHECK(f != NULL, "save: open");
    fputs("dam=1\n", f);
    CHECK(portSaveCommit(f) == 1, "save: commit");
    f = portSaveOpenRead("coop_campaign.txt", NULL, &version);
    CHECK(f != NULL && version == 2, "save: read back version %d", version);
    if (f != NULL)
    {
        char line[64] = "";

        CHECK(fgets(line, sizeof(line), f) != NULL, "save: no body");
        CHECK(strcmp(line, "dam=1\n") == 0, "save: body %s", line);
        fclose(f);
    }

    /* 5: a new build backs the saves up and stamps build.txt */
    put(portSavePath("build.txt"), "aaaaaaaaa\n");
    put(portSavePath("eeprom.bin"), "folders");
    portSaveBootCheck();
    got = slurp(portSavePath("build.txt"));
    CHECK(got != NULL && strncmp(got, PORT_BUILD_COMMIT, strlen(PORT_BUILD_COMMIT)) == 0, "backup: build.txt not stamped (%s)", got);
    CHECK(countDirs(portRootPath("Data/Saves/Backups")) == 1, "backup: %d backups, want 1",
          countDirs(portRootPath("Data/Saves/Backups")));

    /* 6: a save that can't be read whole: no half backup, and build.txt is
     * left so the next start tries again */
    put(portSavePath("build.txt"), "bbbbbbbbb\n");
    CHECK(symlink("/proc/self/mem", portSavePath("unreadable.bin")) == 0, "unreadable: can't fake it");
    portSaveBootCheck();
    got = slurp(portSavePath("build.txt"));
    CHECK(got != NULL && strcmp(got, "bbbbbbbbb\n") == 0, "unreadable: build.txt stamped after a failed backup (%s)", got);
    CHECK(countDirs(portRootPath("Data/Saves/Backups")) == 1, "unreadable: %d backups, want the 1 good one",
          countDirs(portRootPath("Data/Saves/Backups")));
    unlink(portSavePath("unreadable.bin"));
    portSaveBootCheck();
    got = slurp(portSavePath("build.txt"));
    CHECK(got != NULL && strncmp(got, PORT_BUILD_COMMIT, strlen(PORT_BUILD_COMMIT)) == 0, "retry: build.txt not stamped (%s)", got);
    CHECK(countDirs(portRootPath("Data/Saves/Backups")) == 2, "retry: %d backups, want 2",
          countDirs(portRootPath("Data/Saves/Backups")));

    printf("%s\n", sFails ? "save_test: FAILED" : "save_test: all passed");
    return sFails ? 1 : 0;
}
