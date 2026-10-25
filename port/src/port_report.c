/**
 * Crash reports and bug reports: a folder a player can send in one piece.
 *
 * Crash (main.c's handlers): Data/Crashes/crash-<date>-<time>/ gets info.txt
 * (build, OS, where the game was), log.txt (the last 64 KB of the port's log
 * lines), coop.txt (coopDebugDump), crash.log and, on Windows, minidump.dmp
 * and the tail of ge_direct.log. Windows then asks at once whether to put a
 * zip of it on the Desktop; the zip is also put on the clipboard as a file,
 * so Ctrl+V in Discord attaches it. Anything not answered then (Linux, a
 * stall, GE_NOMSGBOX) is asked at the next start, from Data/Crashes/pending.txt.
 *
 * Bug report (F8, or `bugreport` in the console): the same files minus the
 * dump, plus a screenshot, under Data/Reports/bug-<date>-<time>/, zipped to
 * the Desktop straight away, with a line on screen saying where.
 *
 * The zip is "stored" (no compression): this runs inside a crashed process,
 * so it allocates nothing and only uses fixed buffers. Screenshots are halved
 * until 1280 wide or less to keep the zip under Discord's 10 MB.
 * Each folder keeps its newest REPORT_KEEP reports.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <shlobj.h>
#include <shellapi.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/utsname.h>
#include <dirent.h>
#include <unistd.h>
#endif
#if defined(GE_VIDEO) && !defined(_WIN32)
#include <SDL.h>    /* Windows uses its own boxes and clipboard; SDL.h fights windows.h there */
#endif
#include "port_buildinfo.h"

extern const char *portRootPath(const char *name);
extern const char *portLogPath(const char *name);
extern void portConPrintf(int showmsg, const char *fmt, ...);
extern void portConLogf(const char *fmt, ...);
extern int coopDebugDump(char *buf, int cap);   /* src/game/coop.c */
extern void coopCrashBye(void);                 /* src/game/coop.c */
extern int ge_shot_force;                       /* gfx_sdl2.cpp */
extern char ge_shot_path[512];

#define REPORT_KEEP     10
#define REPORT_TAPSIZE  (64 * 1024)
#define REPORT_SHOT_MAXW 1280

/* ---- log tap -------------------------------------------------------------- */

/* The newest REPORT_TAPSIZE bytes of portConLogf's lines. The console's own
 * ring is 128 rows cut at 120 columns, too little to see how a crash began. */
static char sTap[REPORT_TAPSIZE];
static unsigned int sTapHead;   /* total bytes ever written */
static volatile int sTapLock;

void portReportTap(const char *text)
{
    int spin;

    for (spin = 0; spin < 0x10000; spin++)
    {
        if (sTapLock == 0 && __sync_lock_test_and_set(&sTapLock, 1) == 0)
        {
            for (; *text != '\0'; text++)
            {
                sTap[sTapHead++ % REPORT_TAPSIZE] = *text;
            }
            __sync_lock_release(&sTapLock);
            return;
        }
    }
}

static void tapWrite(FILE *f)
{
    unsigned int n = sTapHead < REPORT_TAPSIZE ? sTapHead : REPORT_TAPSIZE;
    unsigned int start = sTapHead - n;
    unsigned int i = 0;

    if (sTapHead > REPORT_TAPSIZE)
    {   /* start on a whole line */
        while (i < n && sTap[(start + i) % REPORT_TAPSIZE] != '\n')
        {
            i++;
        }
        i++;
    }
    for (; i < n; i++)
    {
        fputc(sTap[(start + i) % REPORT_TAPSIZE], f);
    }
}

/* ---- files and folders ---------------------------------------------------- */

static void reportMkdir(const char *path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static int reportExists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL)
    {
        return 0;
    }
    fclose(f);
    return 1;
}

static void reportStamp(char *out, int cap)
{
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);

    if (lt == NULL)
    {
        snprintf(out, cap, "%lu", (unsigned long) t);
        return;
    }
    strftime(out, cap, "%Y%m%d-%H%M%S", lt);
}

static void reportCopy(const char *from, const char *to, long tail)
{
    static char buf[16384];
    FILE *in = fopen(from, "rb");
    FILE *out;
    size_t n;

    if (in == NULL)
    {
        return;
    }
    if (tail > 0 && fseek(in, 0, SEEK_END) == 0 && ftell(in) > tail)
    {
        fseek(in, -tail, SEEK_END);
    }
    else
    {
        fseek(in, 0, SEEK_SET);
    }
    out = fopen(to, "wb");
    if (out != NULL)
    {
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        {
            fwrite(buf, 1, n, out);
        }
        fclose(out);
    }
    fclose(in);
}

static void reportPath(char *out, int cap, const char *dir, const char *name)
{
    snprintf(out, cap, "%s/%s", dir, name);
}

/* folder names under base, oldest first (they sort by their date stamp) */
static int reportList(const char *base, const char *prefix, char names[][48], int max)
{
    int n = 0;
    int i, j;
#ifdef _WIN32
    char pat[1300];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(pat, sizeof(pat), "%s/%s*", base, prefix);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n < max && strlen(fd.cFileName) < 48)
        {
            snprintf(names[n++], 48, "%s", fd.cFileName);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(base);
    struct dirent *e;

    if (d == NULL)
    {
        return 0;
    }
    while ((e = readdir(d)) != NULL)
    {
        if (strncmp(e->d_name, prefix, strlen(prefix)) == 0 && n < max && strlen(e->d_name) < 48)
        {
            snprintf(names[n++], 48, "%s", e->d_name);
        }
    }
    closedir(d);
#endif
    for (i = 1; i < n; i++)
    {
        for (j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--)
        {
            char t[48];

            memcpy(t, names[j], 48);
            memcpy(names[j], names[j - 1], 48);
            memcpy(names[j - 1], t, 48);
        }
    }
    return n;
}

static void reportRemoveDir(const char *dir)
{
#ifdef _WIN32
    char pat[1300];
    char path[1300];
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
    char path[1300];

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

/* keep the newest REPORT_KEEP folders, so reports never pile up */
static void reportPrune(const char *base, const char *prefix)
{
    static char names[64][48];
    char path[1300];
    int n = reportList(base, prefix, names, 64);
    int i;

    for (i = 0; i + REPORT_KEEP < n; i++)
    {
        snprintf(path, sizeof(path), "%s/%s", base, names[i]);
        reportRemoveDir(path);
    }
}

/* ---- zip (stored) --------------------------------------------------------- */

static unsigned int sCrcTable[256];

static unsigned int reportCrc(unsigned int crc, const unsigned char *p, size_t n)
{
    size_t i;

    if (sCrcTable[1] == 0)
    {
        unsigned int c, k, b;

        for (k = 0; k < 256; k++)
        {
            c = k;
            for (b = 0; b < 8; b++)
            {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            sCrcTable[k] = c;
        }
    }
    crc = ~crc;
    for (i = 0; i < n; i++)
    {
        crc = sCrcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

static void zipPut16(FILE *f, unsigned int v)
{
    fputc(v & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);
}

static void zipPut32(FILE *f, unsigned int v)
{
    zipPut16(f, v & 0xFFFF);
    zipPut16(f, v >> 16);
}

#define ZIP_MAX 12

/* zip dir's files (names) into zippath, each as "<folder>/<name>" */
static int reportZip(const char *zippath, const char *dir, const char *folder, const char *const *names, int count)
{
    static unsigned char buf[32768];
    unsigned int crc[ZIP_MAX], size[ZIP_MAX], ofs[ZIP_MAX];
    int have[ZIP_MAX];
    unsigned int dostime, dosdate, cdofs, cdsize;
    char entry[160];
    char path[1300];
    int i, files = 0;
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    FILE *z;

    dostime = lt ? (unsigned) ((lt->tm_hour << 11) | (lt->tm_min << 5) | (lt->tm_sec / 2)) : 0;
    dosdate = lt ? (unsigned) (((lt->tm_year - 80) << 9) | ((lt->tm_mon + 1) << 5) | lt->tm_mday) : 0x21;
    z = fopen(zippath, "wb");
    if (z == NULL)
    {
        return 0;
    }
    for (i = 0; i < count && i < ZIP_MAX; i++)
    {
        FILE *in;
        size_t n;
        long start;

        reportPath(path, sizeof(path), dir, names[i]);
        have[i] = 0;
        in = fopen(path, "rb");
        if (in == NULL)
        {
            continue;
        }
        crc[i] = 0;
        size[i] = 0;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        {
            crc[i] = reportCrc(crc[i], buf, n);
            size[i] += (unsigned) n;
        }
        snprintf(entry, sizeof(entry), "%s/%s", folder, names[i]);
        start = ftell(z);
        ofs[i] = (unsigned) start;
        zipPut32(z, 0x04034b50);
        zipPut16(z, 10);            /* version needed */
        zipPut16(z, 0);             /* flags */
        zipPut16(z, 0);             /* stored */
        zipPut16(z, dostime);
        zipPut16(z, dosdate);
        zipPut32(z, crc[i]);
        zipPut32(z, size[i]);
        zipPut32(z, size[i]);
        zipPut16(z, (unsigned) strlen(entry));
        zipPut16(z, 0);
        fwrite(entry, 1, strlen(entry), z);
        fseek(in, 0, SEEK_SET);
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        {
            fwrite(buf, 1, n, z);
        }
        fclose(in);
        have[i] = 1;
        files++;
    }
    cdofs = (unsigned) ftell(z);
    for (i = 0; i < count && i < ZIP_MAX; i++)
    {
        if (!have[i])
        {
            continue;
        }
        snprintf(entry, sizeof(entry), "%s/%s", folder, names[i]);
        zipPut32(z, 0x02014b50);
        zipPut16(z, 20);            /* made by */
        zipPut16(z, 10);
        zipPut16(z, 0);
        zipPut16(z, 0);
        zipPut16(z, dostime);
        zipPut16(z, dosdate);
        zipPut32(z, crc[i]);
        zipPut32(z, size[i]);
        zipPut32(z, size[i]);
        zipPut16(z, (unsigned) strlen(entry));
        zipPut16(z, 0);             /* extra */
        zipPut16(z, 0);             /* comment */
        zipPut16(z, 0);             /* disk */
        zipPut16(z, 0);             /* internal attributes */
        zipPut32(z, 0);             /* external attributes */
        zipPut32(z, ofs[i]);
        fwrite(entry, 1, strlen(entry), z);
    }
    cdsize = (unsigned) ftell(z) - cdofs;
    zipPut32(z, 0x06054b50);
    zipPut16(z, 0);
    zipPut16(z, 0);
    zipPut16(z, (unsigned) files);
    zipPut16(z, (unsigned) files);
    zipPut32(z, cdsize);
    zipPut32(z, cdofs);
    zipPut16(z, 0);
    fclose(z);
    return files;
}

/* ---- where the zip goes --------------------------------------------------- */

/* the Desktop, else the folder the reports live in */
static void reportDesktop(char *out, int cap, const char *fallback)
{
#ifdef _WIN32
    char path[MAX_PATH];

    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, path)))
    {
        snprintf(out, cap, "%s", path);
        return;
    }
#else
    const char *home = getenv("HOME");
    struct stat st;

    if (getenv("GE_REPORT_DESKTOP") != NULL)
    {   /* harness: where the "Desktop" is */
        snprintf(out, cap, "%s", getenv("GE_REPORT_DESKTOP"));
        return;
    }
    if (home != NULL)
    {
        snprintf(out, cap, "%s/Desktop", home);
        if (stat(out, &st) == 0 && S_ISDIR(st.st_mode))
        {
            return;
        }
    }
#endif
    snprintf(out, cap, "%s", fallback);
}

static void reportNativePath(char *p)
{
#ifdef _WIN32
    for (; *p != '\0'; p++)
    {
        if (*p == '/')
        {
            *p = '\\';
        }
    }
#else
    (void) p;
#endif
}

/* the zip on the clipboard: as a file on Windows (paste attaches it), as its
 * path elsewhere */
static void reportClipboard(const char *zippath)
{
#ifdef _WIN32
    size_t len = strlen(zippath);
    HGLOBAL h = GlobalAlloc(GHND, sizeof(DROPFILES) + len + 2);
    DROPFILES *df;

    if (h == NULL)
    {
        return;
    }
    df = (DROPFILES *) GlobalLock(h);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = FALSE;
    memcpy((char *) (df + 1), zippath, len);    /* GHND zeroed the double NUL */
    GlobalUnlock(h);
    if (OpenClipboard(NULL))
    {
        EmptyClipboard();
        if (SetClipboardData(CF_HDROP, h) == NULL)
        {
            GlobalFree(h);
        }
        CloseClipboard();
    }
    else
    {
        GlobalFree(h);
    }
#elif defined(GE_VIDEO)
    if (SDL_WasInit(SDL_INIT_VIDEO))
    {
        SDL_SetClipboardText(zippath);
    }
#else
    (void) zippath;
#endif
}

static void reportShowInFolder(const char *zippath)
{
#ifdef _WIN32
    char args[1400];

    snprintf(args, sizeof(args), "/select,\"%s\"", zippath);
    ShellExecuteA(NULL, "open", "explorer.exe", args, NULL, SW_SHOWNORMAL);
#else
    (void) zippath;
#endif
}

/* ---- the report's files --------------------------------------------------- */

static const char *const sCrashFiles[] = {
    "info.txt", "log.txt", "coop.txt", "crash.log", "backtrace.txt", "ge_direct.log", "midas.ini", "minidump.dmp",
};
static const char *const sBugFiles[] = {
    "info.txt", "log.txt", "coop.txt", "ge_direct.log", "midas.ini", "screenshot.bmp",
};
#define COUNT(a) ((int) (sizeof(a) / sizeof((a)[0])))

static void reportWriteInfo(const char *dir, const char *what)
{
    extern int current_menu;
    extern int bossGetStageNum(void);
    char path[1300];
    char stamp[32];
    FILE *f;

    reportPath(path, sizeof(path), dir, "info.txt");
    f = fopen(path, "w");
    if (f == NULL)
    {
        return;
    }
    reportStamp(stamp, sizeof(stamp));
    fprintf(f, "%s\n", what);
    fprintf(f, "time: %s\n", stamp);
    fprintf(f, "build: %s (commit %s, branch %s)\n", PORT_BUILD_STRING, PORT_BUILD_COMMIT, PORT_BUILD_BRANCH);
#ifdef _WIN32
    {
        OSVERSIONINFOA v;

        memset(&v, 0, sizeof(v));
        v.dwOSVersionInfoSize = sizeof(v);
        GetVersionExA(&v);  /* manifest-less: says 6.2 on Windows 10/11, still tells 7 from 10 */
        fprintf(f, "os: Windows %lu.%lu build %lu\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    }
#else
    {
        struct utsname u;

        if (uname(&u) == 0)
        {
            fprintf(f, "os: %s %s %s\n", u.sysname, u.release, u.machine);
        }
    }
#endif
    fprintf(f, "menu %d, stage %d\n", current_menu, bossGetStageNum());
    fclose(f);
}

static void reportWriteCommon(const char *dir)
{
    static char coop[8192];
    char path[1300];
    FILE *f;

    reportPath(path, sizeof(path), dir, "log.txt");
    f = fopen(path, "w");
    if (f != NULL)
    {
        tapWrite(f);
        fclose(f);
    }
    reportPath(path, sizeof(path), dir, "coop.txt");
    f = fopen(path, "w");
    if (f != NULL)
    {
        coop[0] = '\0';
        coopDebugDump(coop, sizeof(coop));
        fputs(coop, f);
        fclose(f);
    }
    reportPath(path, sizeof(path), dir, "midas.ini");
    reportCopy(portRootPath("midas.ini"), path, 0);
#ifdef _WIN32
    reportPath(path, sizeof(path), dir, "ge_direct.log");
    reportCopy(portLogPath("ge_direct.log"), path, 512 * 1024);
#endif
}

/* ---- crash ---------------------------------------------------------------- */

static char sCrashDir[1200];
static char sCrashName[48];
static volatile int sCrashing;

static int reportPackToDesktop(const char *dir, const char *folder, const char *const *names, int count,
                               char *zippath, int cap)
{
    char desk[1100];

    reportDesktop(desk, sizeof(desk), dir);
    snprintf(zippath, cap, "%s/GoldenEye-%s.zip", desk, folder);
    reportNativePath(zippath);
    if (reportZip(zippath, dir, folder, names, count) == 0)
    {
        return 0;
    }
    reportClipboard(zippath);
    return 1;
}

static void reportCrashBase(char *out, int cap)
{
    snprintf(out, cap, "%s", portRootPath("Data/Crashes"));
}

/* write the crash folder; ctx is the EXCEPTION_POINTERS on Windows (NULL from
 * the stall watchdog, which dumps the whole process from its own thread) */
void portReportCrash(void *ctx, const char *what)
{
    char base[1200];
    char stamp[32];
    char path[1300];
    FILE *f;

    if (__sync_lock_test_and_set(&sCrashing, 1) != 0)
    {
        return;     /* once: a fault inside the report must not loop */
    }
    reportCrashBase(base, sizeof(base));
    reportMkdir(portRootPath("Data"));
    reportMkdir(base);
    reportStamp(stamp, sizeof(stamp));
    snprintf(sCrashName, sizeof(sCrashName), "crash-%s", stamp);
    snprintf(sCrashDir, sizeof(sCrashDir), "%s/%s", base, sCrashName);
    reportMkdir(sCrashDir);

    reportWriteInfo(sCrashDir, what);
    reportWriteCommon(sCrashDir);
    /* the partners hear now, not after the silence: a crashed host's
     * partners pick a new one at once (after coop.txt, which is the state
     * before it) */
    coopCrashBye();
    reportPath(path, sizeof(path), sCrashDir, "crash.log");
    reportCopy(portLogPath("crash.log"), path, 0);
#ifdef _WIN32
    {
        HANDLE h;

        reportPath(path, sizeof(path), sCrashDir, "minidump.dmp");
        h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION mei;

            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = (EXCEPTION_POINTERS *) ctx;
            mei.ClientPointers = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                              (MINIDUMP_TYPE) (MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory),
                              ctx != NULL ? &mei : NULL, NULL, NULL);
            CloseHandle(h);
        }
    }
#else
    (void) ctx;
#endif
    /* asked about at the next start unless answered now */
    reportPath(path, sizeof(path), base, "pending.txt");
    f = fopen(path, "w");
    if (f != NULL)
    {
        fprintf(f, "%s\n", sCrashName);
        fclose(f);
    }
    reportPrune(base, "crash-");
    fprintf(stderr, "ge: crash report: %s\n", sCrashDir);
}

/* Linux: the backtrace beside the rest (main.c writes it with
 * backtrace_symbols_fd, which needs a descriptor) */
FILE *portReportCrashFile(const char *name)
{
    char path[1300];

    if (sCrashDir[0] == '\0')
    {
        return NULL;
    }
    reportPath(path, sizeof(path), sCrashDir, name);
    return fopen(path, "w");
}

static void reportForgetPending(void)
{
    char base[1200];
    char path[1300];

    reportCrashBase(base, sizeof(base));
    reportPath(path, sizeof(path), base, "pending.txt");
    remove(path);
}

static int reportAutoAnswer(void)
{
    /* harness: GE_REPORT_AUTOSAVE=1 answers "save it" without a box */
    return getenv("GE_REPORT_AUTOSAVE") != NULL;
}

#ifdef _WIN32
static BOOL CALLBACK reportHideWindow(HWND w, LPARAM lp)
{
    (void) lp;
    ShowWindow(w, SW_MINIMIZE);     /* a fullscreen game window would cover the box */
    return TRUE;
}
#endif

/* Windows, in the crash handler: ask now. Linux asks at the next start. */
void portReportCrashAsk(void)
{
#ifdef _WIN32
    char zip[1300];
    char text[1600];
    int yes;

    if (sCrashDir[0] == '\0')
    {
        return;
    }
    if (!reportAutoAnswer())
    {
        if (getenv("GE_NOMSGBOX") != NULL || getenv("GE_FRAMES") != NULL)
        {
            return;     /* the next start asks */
        }
        EnumThreadWindows(GetCurrentThreadId(), reportHideWindow, 0);
        yes = MessageBoxA(NULL,
            "GoldenEye Co-op crashed, sorry about that.\n\n"
            "A crash report was saved. Put it on your Desktop as a zip so you can send it?\n"
            "It is copied too: paste it straight into Discord.\n\n"
            "It has the game's log, its settings and a memory dump, nothing else.",
            "GoldenEye Co-op crashed", MB_YESNO | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND) == IDYES;
        if (!yes)
        {
            return;     /* asked again next start */
        }
    }
    if (reportPackToDesktop(sCrashDir, sCrashName, sCrashFiles, COUNT(sCrashFiles), zip, sizeof(zip)))
    {
        reportForgetPending();
        if (!reportAutoAnswer())
        {
            reportShowInFolder(zip);
            snprintf(text, sizeof(text), "Saved %s\n\nIt is on the clipboard: paste it into Discord to send it.", zip);
            MessageBoxA(NULL, text, "Crash report saved", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        }
    }
#endif
}

/* At start, before the window: a crash report nobody answered for. */
void portReportBootCheck(void)
{
    char base[1200];
    char path[1300];
    char dir[1300];
    char name[48];
    char zip[1300];
    FILE *f;
    int choice = 0;     /* 0 not now, 1 save, 2 forget it */

    reportCrashBase(base, sizeof(base));
    reportPath(path, sizeof(path), base, "pending.txt");
    f = fopen(path, "r");
    if (f == NULL)
    {
        return;
    }
    name[0] = '\0';
    if (fgets(name, sizeof(name), f) == NULL)
    {
        name[0] = '\0';
    }
    fclose(f);
    name[strcspn(name, "\r\n")] = '\0';
    snprintf(dir, sizeof(dir), "%s/%s", base, name);
    reportPath(path, sizeof(path), dir, "info.txt");
    if (name[0] == '\0' || !reportExists(path))
    {
        reportForgetPending();
        return;
    }
    if (reportAutoAnswer())
    {
        choice = 1;
    }
#ifdef _WIN32
    else if (getenv("GE_NOMSGBOX") == NULL && getenv("GE_FRAMES") == NULL)
    {
        switch (MessageBoxA(NULL,
            "The game crashed last time and saved a crash report.\n\n"
            "Save it to your Desktop as a zip so you can send it? It is copied too, "
            "so you can paste it straight into Discord.\n\n"
            "It has the game's log, its settings and a memory dump, nothing else.\n\n"
            "Yes: save it.   No: don't send it.   Cancel: ask me next time.",
            "GoldenEye Co-op crashed last time", MB_YESNOCANCEL | MB_ICONWARNING | MB_TOPMOST))
        {
        case IDYES: choice = 1; break;
        case IDNO:  choice = 2; break;
        default:    choice = 0; break;
        }
    }
#elif defined(GE_VIDEO)
    else if (getenv("GE_NOMSGBOX") == NULL && getenv("GE_FRAMES") == NULL)
    {
        static const SDL_MessageBoxButtonData buttons[] = {
            { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Save to Desktop" },
            { 0, 2, "Don't send" },
            { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Ask me later" },
        };
        SDL_MessageBoxData box;
        int hit = 0;

        memset(&box, 0, sizeof(box));
        box.flags = SDL_MESSAGEBOX_WARNING;
        box.title = "GoldenEye Co-op crashed last time";
        box.message = "The game crashed last time and saved a crash report.\n\n"
                      "Save it to your Desktop as a zip so you can send it? It is copied too, "
                      "so you can paste it straight into Discord.\n\n"
                      "It has the game's log, its settings and a memory dump, nothing else.";
        box.numbuttons = 3;
        box.buttons = buttons;
        if (SDL_ShowMessageBox(&box, &hit) == 0)
        {
            choice = hit;
        }
    }
#endif
    if (choice == 0)
    {
        return;
    }
    reportForgetPending();
    if (choice == 1 && reportPackToDesktop(dir, name, sCrashFiles, COUNT(sCrashFiles), zip, sizeof(zip)))
    {
        portConLogf("ge: crash report saved: %s\n", zip);
        reportShowInFolder(zip);
    }
}

/* ---- bug report ----------------------------------------------------------- */

enum { BUG_IDLE, BUG_ASKED, BUG_SHOT };
static int sBugState;
static int sBugFrames;
static char sBugDir[1200];
static char sBugName[48];

void portReportBugRequest(void)
{
    if (sBugState == BUG_IDLE)
    {
        sBugState = BUG_ASKED;
    }
}

/* halve a 24-bit BMP until it is REPORT_SHOT_MAXW wide or less (in place) */
static void reportShrinkShot(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char hdr[54];
    unsigned char *px, *out;
    int w, h, stride, nw, nh, nstride, x, y, c;
    long size;

    if (f == NULL)
    {
        return;
    }
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M' || hdr[28] != 24)
    {
        fclose(f);
        return;
    }
    memcpy(&w, hdr + 18, 4);
    memcpy(&h, hdr + 22, 4);
    if (w <= REPORT_SHOT_MAXW || h <= 0)
    {
        fclose(f);
        return;
    }
    stride = (w * 3 + 3) & ~3;
    size = (long) stride * h;
    px = (unsigned char *) malloc(size);
    if (px == NULL || (long) fread(px, 1, size, f) != size)
    {
        free(px);
        fclose(f);
        return;
    }
    fclose(f);
    while (w > REPORT_SHOT_MAXW && h >= 2)
    {
        nw = w / 2;
        nh = h / 2;
        nstride = (nw * 3 + 3) & ~3;
        out = px;   /* the halved image is smaller: write it over the start */
        for (y = 0; y < nh; y++)
        {
            for (x = 0; x < nw; x++)
            {
                for (c = 0; c < 3; c++)
                {
                    const unsigned char *a = px + (2 * y) * stride + (2 * x) * 3 + c;
                    const unsigned char *b = a + stride;

                    out[y * nstride + x * 3 + c] = (unsigned char) ((a[0] + a[3] + b[0] + b[3] + 2) / 4);
                }
            }
        }
        w = nw;
        h = nh;
        stride = nstride;
    }
    size = (long) stride * h;
    {
        unsigned int fsize = 54 + (unsigned int) size;
        unsigned int isize = (unsigned int) size;

        memcpy(hdr + 2, &fsize, 4);
        memcpy(hdr + 18, &w, 4);
        memcpy(hdr + 22, &h, 4);
        memcpy(hdr + 34, &isize, 4);
    }
    f = fopen(path, "wb");
    if (f != NULL)
    {
        fwrite(hdr, 1, 54, f);
        fwrite(px, 1, size, f);
        fclose(f);
    }
    free(px);
}

static void reportBugFinish(void)
{
    char zip[1300];
    char path[1300];
    const char *leaf;

    reportPath(path, sizeof(path), sBugDir, "screenshot.bmp");
    reportShrinkShot(path);
    if (!reportPackToDesktop(sBugDir, sBugName, sBugFiles, COUNT(sBugFiles), zip, sizeof(zip)))
    {
        portConPrintf(1, "Couldn't save the bug report zip. The files are in %s\n", sBugDir);
        return;
    }
    leaf = strrchr(zip, '/');
#ifdef _WIN32
    if (strrchr(zip, '\\') > leaf)
    {
        leaf = strrchr(zip, '\\');
    }
#endif
    leaf = leaf != NULL ? leaf + 1 : zip;
    portConLogf("ge: bug report saved: %s\n", zip);
    portConPrintf(1, "Bug report saved to your Desktop: %s\n", leaf);
    portConPrintf(1, "It is copied: paste it into Discord to send it.\n");
}

/* harness: GE_BUGTEST=<frame> presses F8, GE_CRASHTEST=<frame> crashes */
static void reportHarness(void)
{
    static int frame;
    static int bugAt = -2, crashAt = -2;

    if (bugAt == -2)
    {
        bugAt = getenv("GE_BUGTEST") != NULL ? atoi(getenv("GE_BUGTEST")) : -1;
        crashAt = getenv("GE_CRASHTEST") != NULL ? atoi(getenv("GE_CRASHTEST")) : -1;
    }
    frame++;
    if (frame == bugAt)
    {
        portReportBugRequest();
    }
    if (frame == crashAt)
    {
        portConLogf("ge: GE_CRASHTEST: crashing on purpose\n");
        *(volatile int *) (size_t) 16 = 1;
    }
}

/* once a frame (boss.c): F8 waits here for the frame to be drawn */
void portReportTick(void)
{
    char base[1200];
    char stamp[32];
    char path[1300];

    reportHarness();

    switch (sBugState)
    {
    case BUG_ASKED:
        snprintf(base, sizeof(base), "%s", portRootPath("Data/Reports"));
        reportMkdir(portRootPath("Data"));
        reportMkdir(base);
        reportPrune(base, "bug-");
        reportStamp(stamp, sizeof(stamp));
        snprintf(sBugName, sizeof(sBugName), "bug-%s", stamp);
        snprintf(sBugDir, sizeof(sBugDir), "%s/%s", base, sBugName);
        reportMkdir(sBugDir);
        reportWriteInfo(sBugDir, "bug report (F8)");
        reportWriteCommon(sBugDir);
        reportPath(path, sizeof(path), sBugDir, "screenshot.bmp");
        snprintf(ge_shot_path, sizeof(ge_shot_path), "%s", path);
        ge_shot_force = 1;
        sBugFrames = 0;
        sBugState = BUG_SHOT;
        break;
    case BUG_SHOT:
        reportPath(path, sizeof(path), sBugDir, "screenshot.bmp");
        /* the shot is written as a frame is presented, which clears
         * ge_shot_path; without one after 2 s, send the rest */
        sBugFrames++;
        if ((sBugFrames >= 3 && ge_shot_path[0] == '\0' && reportExists(path)) || sBugFrames > 120)
        {
            reportBugFinish();
            sBugState = BUG_IDLE;
        }
        break;
    default:
        break;
    }
}
