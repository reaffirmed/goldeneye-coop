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

/* ---- a stored (uncompressed) zip, for the catch-up cases ---- */

static unsigned long crc32of(const char *s, size_t n)
{
    unsigned long c = 0xFFFFFFFFUL;
    size_t i;
    int k;

    for (i = 0; i < n; i++)
    {
        c ^= (unsigned char) s[i];
        for (k = 0; k < 8; k++)
        {
            c = (c & 1) ? (c >> 1) ^ 0xEDB88320UL : c >> 1;
        }
    }
    return c ^ 0xFFFFFFFFUL;
}

static void le(FILE *f, unsigned long v, int bytes)
{
    while (bytes-- > 0)
    {
        fputc((int) (v & 0xFF), f);
        v >>= 8;
    }
}

/* names[i] holds texts[i]; every name is under the zip's one top folder */
static void putZip(const char *rel, const char *const *names, const char *const *texts, int n)
{
    FILE *f = fopen(at(rel), "wb");
    long offs[16], cd;
    int i;

    for (i = 0; i < n; i++)
    {
        const size_t nl = strlen(names[i]), tl = strlen(texts[i]);

        offs[i] = ftell(f);
        le(f, 0x04034b50UL, 4); le(f, 20, 2); le(f, 0, 2); le(f, 0, 2); le(f, 0, 2); le(f, 0, 2);
        le(f, crc32of(texts[i], tl), 4); le(f, tl, 4); le(f, tl, 4); le(f, nl, 2); le(f, 0, 2);
        fwrite(names[i], 1, nl, f);
        fwrite(texts[i], 1, tl, f);
    }
    cd = ftell(f);
    for (i = 0; i < n; i++)
    {
        const size_t nl = strlen(names[i]), tl = strlen(texts[i]);

        le(f, 0x02014b50UL, 4); le(f, 20, 2); le(f, 20, 2); le(f, 0, 2); le(f, 0, 2); le(f, 0, 2); le(f, 0, 2);
        le(f, crc32of(texts[i], tl), 4); le(f, tl, 4); le(f, tl, 4); le(f, nl, 2); le(f, 0, 2); le(f, 0, 2);
        le(f, 0, 2); le(f, 0, 2); le(f, 0, 4); le(f, (unsigned long) offs[i], 4);
        fwrite(names[i], 1, nl, f);
    }
    le(f, 0x06054b50UL, 4); le(f, 0, 2); le(f, 0, 2); le(f, n, 2); le(f, n, 2);
    le(f, (unsigned long) (ftell(f) - cd - 12), 4);
    le(f, (unsigned long) cd, 4); le(f, 0, 2);
    fclose(f);
}

/* an update from an older build that left the new lang/ folder out: the
 * swap is done, installed.txt lists only what that build took, and its
 * download is still in Data/Update for the new build */
static void layoutCatchUp(const char *case_name)
{
    static char exe[260], names[6][300];
    const char *const texts[6] = { "new exe", "msgid \"Yes\"\nmsgstr \"S\xed\"\n", "name=Espanol\n", "zip readme", "zip ini", "nested" };
    const char *namep[6];
    char self[1024], cmd[1200], list[600];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    const char *base = self;
    int i;

    self[n > 0 ? n : 0] = '\0';
    for (i = 0; self[i] != '\0'; i++)
    {
        if (self[i] == '/')
        {
            base = self + i + 1;
        }
    }
    snprintf(exe, sizeof(exe), "%s", base);   /* the updater finds the zip's top folder by our own exe's name */
    snprintf(sRoot, sizeof(sRoot), "%s/%s", getenv("RESUME_BASE"), case_name);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s/Data/Update'", sRoot, sRoot);
    if (system(cmd) != 0)
    {
        printf("FAIL: can't make %s\n", sRoot);
        exit(1);
    }
    snprintf(names[0], sizeof(names[0]), "GoldenEye-Coop/%s", exe);
    snprintf(names[1], sizeof(names[1]), "GoldenEye-Coop/lang/es/port.po");
    snprintf(names[2], sizeof(names[2]), "GoldenEye-Coop/lang/es/language.ini");
    snprintf(names[3], sizeof(names[3]), "GoldenEye-Coop/Data/README.txt");
    snprintf(names[4], sizeof(names[4]), "GoldenEye-Coop/midas.ini");
    snprintf(names[5], sizeof(names[5]), "GoldenEye-Coop/lang/es/deeper/x.po");
    for (i = 0; i < 6; i++)
    {
        namep[i] = names[i];
    }
    putZip("Data/Update/GoldenEye-Coop-v0.8.0-linux.zip", namep, texts, 6);
    put(exe, "new exe");
    put("Data/README.txt", "the install's readme");
    snprintf(list, sizeof(list), "v0.7.1 v0.8.0 GoldenEye-Coop-v0.8.0-linux.zip\n%s\nData/README.txt\n", exe);
    put("Data/Update/installed.txt", list);
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

    printf("6: the new build puts in the lang folder an older updater left out\n");
    layoutCatchUp("catchup");
    boot("v0.8.0", &state, latest);
    CHECK(strcmp(get("lang/es/port.po"), "msgid \"Yes\"\nmsgstr \"S\xed\"\n") == 0, "catchup: lang/es/port.po is %s", get("lang/es/port.po"));
    CHECK(strcmp(get("lang/es/language.ini"), "name=Espanol\n") == 0, "catchup: lang/es/language.ini is %s", get("lang/es/language.ini"));
    CHECK(strcmp(get("Data/README.txt"), "the install's readme") == 0, "catchup: overwrote a file that was there");
    CHECK(!has("midas.ini"), "catchup: wrote a settings file");
    CHECK(!has("lang/es/deeper/x.po"), "catchup: wrote below a language's own folder");
    CHECK(!has("lang/es/port.po.tmp") && !has("lang/es/language.ini.tmp"), "catchup: left a .tmp");
    CHECK(!has("Data/Update/installed.txt") && !has("Data/Update/GoldenEye-Coop-v0.8.0-linux.zip"), "catchup: the list or the download wasn't cleared");

    printf("7: a newer build was unzipped over that install before it ran\n");
    layoutCatchUp("catchup-newer");
    boot("v0.8.1", &state, latest);
    CHECK(!has("lang/es/port.po"), "catchup-newer: put in files from the older download");

    printf("%s\n", sFails ? "update_resume_test: FAILED" : "update_resume_test: all passed");
    return sFails ? 1 : 0;
}
