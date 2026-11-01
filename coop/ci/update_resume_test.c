/**
 * An update cut off half way through swapping files (the game killed, a crash,
 * the power) is finished on the next start (port_update.c resumeSwap):
 *
 *   gcc -O1 -Wall -Iport/include -Ibuild_pc/generated -o update_resume_test \
 *       coop/ci/update_resume_test.c port/src/port_update.c port/src/port_update_util.c -lpthread
 *   ./update_resume_test <an empty scratch folder>
 *
 * (build_pc/generated holds port_buildinfo.h after any configure of the game.)
 * Each case lays out a game folder as the swap left it and runs the
 * updater's boot step on it in a child process (the updater keeps its state
 * in statics, once a run, like the game). Linux only. Exit 1 on any failure.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "port_update.h"

static char sRoot[1024];
static int sFails;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); sFails++; } } while (0)

/* what the game gives the updater */
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
    printf("    log: ");
    vprintf(fmt, ap);
    va_end(ap);
}

int portVideoAutoUpdate(void) { return 0; }
void portRequestQuit(void) { }

static const char *at(const char *rel)
{
    return portRootPath(rel);
}

static void put(const char *rel, const char *text)
{
    FILE *f = fopen(at(rel), "w");

    if (f != NULL)
    {
        fputs(text, f);
        fclose(f);
    }
}

static const char *get(const char *rel)
{
    static char buf[2][256];
    static int next;
    char *b = buf[next++ & 1];
    FILE *f = fopen(at(rel), "r");
    size_t n;

    if (f == NULL)
    {
        return "(none)";
    }
    n = fread(b, 1, 255, f);
    fclose(f);
    b[n] = '\0';
    return b;
}

static int has(const char *rel)
{
    struct stat st;

    return stat(at(rel), &st) == 0;
}

/* three files, as a swap cut off after the first and half way through the
 * second leaves them: a.dll is in, b.txt is moved aside but the new one not
 * yet in, Data/c.txt not started */
static void layout(const char *case_name, int list)
{
    char cmd[1200];

    snprintf(sRoot, sizeof(sRoot), "%s/%s", getenv("RESUME_BASE"), case_name);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s/Data/Update/new/Data'", sRoot, sRoot);
    if (system(cmd) != 0)
    {
        printf("FAIL: can't make %s\n", sRoot);
        exit(1);
    }
    put("a.dll", "new a");
    put("a.dll.old", "old a");
    put("b.txt.old", "old b");
    put("Data/Update/new/b.txt", "new b");
    put("Data/c.txt", "old c");
    put("Data/Update/new/Data/c.txt", "new c");
    if (list == 1)
    {
        put("Data/Update/pending.txt", "v0.6.14 v0.6.15 GoldenEye-Coop-v0.6.15-linux.zip\na.dll\nb.txt\nData/c.txt\n");
    }
    else if (list == 2)
    {   /* cut off while the list itself was being written: nothing moved yet */
        put("Data/Update/pending.txt", "");
    }
}

/* the updater's boot step, as a build claiming to be `ver` */
static void boot(const char *ver, int *state, char *latest)
{
    int fd[2];
    pid_t pid;
    char buf[64] = "";

    fflush(stdout);
    if (pipe(fd) != 0)
    {
        exit(1);
    }
    pid = fork();
    if (pid == 0)
    {
        char *argv[] = { "ProjectMidas", NULL };
        char out[64];

        close(fd[0]);
        setenv("GE_UPDATE_FAKEVER", ver, 1);
        portUpdateBoot(1, argv);
        portUpdateTick();   /* the first frame: must not drop a finished install's Restart */
        snprintf(out, sizeof(out), "%d %s", portUpdateState(), portUpdateLatest()[0] ? portUpdateLatest() : "-");
        if (write(fd[1], out, strlen(out)) < 0)
        {
            _exit(1);
        }
        fflush(stdout);
        _exit(0);
    }
    close(fd[1]);
    if (read(fd[0], buf, sizeof(buf) - 1) < 0)
    {
        buf[0] = '\0';
    }
    close(fd[0]);
    waitpid(pid, NULL, 0);
    *state = -1;
    latest[0] = '\0';
    sscanf(buf, "%d %47s", state, latest);
}

int main(int argc, char **argv)
{
    char latest[48];
    int state;

    if (argc < 2)
    {
        printf("usage: %s <empty scratch folder>\n", argv[0]);
        return 2;
    }
    setenv("RESUME_BASE", argv[1], 1);
    mkdir(argv[1], 0755);

    printf("1: the old build starts again after the cut\n");
    layout("old", 1);
    boot("v0.6.14", &state, latest);
    CHECK(strcmp(get("a.dll"), "new a") == 0, "old: a.dll is %s", get("a.dll"));
    CHECK(strcmp(get("b.txt"), "new b") == 0, "old: b.txt is %s", get("b.txt"));
    CHECK(strcmp(get("Data/c.txt"), "new c") == 0, "old: Data/c.txt is %s", get("Data/c.txt"));
    CHECK(strcmp(get("b.txt.old"), "old b") == 0 && strcmp(get("Data/c.txt.old"), "old c") == 0,
          "old: the old files weren't kept as .old");
    CHECK(!has("Data/Update/pending.txt") && has("Data/Update/installed.txt"), "old: the list wasn't marked done");
    CHECK(state == PORTUPD_READY && strcmp(latest, "v0.6.15") == 0, "old: state %d latest %s, want Restart into v0.6.15", state, latest);

    printf("2: the new build is the one started (its exe was in before the cut)\n");
    layout("new", 1);
    boot("v0.6.15", &state, latest);
    CHECK(strcmp(get("b.txt"), "new b") == 0 && strcmp(get("Data/c.txt"), "new c") == 0, "new: files not finished");
    CHECK(!has("a.dll.old") && !has("b.txt.old") && !has("Data/c.txt.old"), "new: .old files not cleared");
    CHECK(!has("Data/Update/pending.txt") && !has("Data/Update/installed.txt"), "new: the lists weren't cleared");
    CHECK(state != PORTUPD_READY, "new: offers a restart it doesn't need");

    printf("3: a newer build was unzipped over the folder since\n");
    layout("newer", 1);
    boot("v0.6.16", &state, latest);
    CHECK(!has("b.txt") && strcmp(get("Data/c.txt"), "old c") == 0, "newer: the stale download was moved in");
    CHECK(!has("Data/Update/pending.txt"), "newer: the stale list was kept");
    CHECK(state != PORTUPD_READY, "newer: offers a restart");

    printf("4: cut off while the list was being written\n");
    layout("nolist", 2);
    boot("v0.6.14", &state, latest);
    CHECK(strcmp(get("Data/c.txt"), "old c") == 0, "nolist: moved files with no list");
    CHECK(!has("Data/Update/pending.txt"), "nolist: the empty list was kept");

    printf("5: no update under way\n");
    layout("none", 0);
    boot("v0.6.14", &state, latest);
    CHECK(strcmp(get("Data/c.txt"), "old c") == 0 && strcmp(get("a.dll.old"), "old a") == 0, "none: files moved");

    printf("%s\n", sFails ? "update_resume_test: FAILED" : "update_resume_test: all passed");
    return sFails ? 1 : 0;
}
