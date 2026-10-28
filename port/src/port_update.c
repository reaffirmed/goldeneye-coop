/**
 * port_update.c - the in-game updater.
 *
 * Asks GitHub for the newest release of the co-op build (releases/latest,
 * which skips the rolling coop-latest prerelease), once at startup unless
 * [display] auto_update=0, again when a host turns us away for another co-op
 * version, and when the player picks Check Now. A newer release shows on the
 * CO-OP screen (front.c coopLobbyAddUpdate); picking it:
 *
 *   1. downloads the release's zip for this system to Data/Update/<name>.part
 *      (WinHTTP on Windows, curl elsewhere), checks its size and the SHA-256
 *      GitHub lists for it ("digest"), and keeps it as Data/Update/<name>;
 *   2. reads the zip (port_update_util.c: inflate and CRC for every file) and
 *      unpacks the files under its top folder that portUpdPathAllowed lets
 *      through to Data/Update/new/. Never the ROM, midas.ini, Data/Saves or
 *      anything else under Data/ but its plain text files;
 *   3. swaps each one in beside the exe: the old file is renamed <name>.old
 *      (Windows lets a running exe and its loaded DLLs be renamed, not
 *      overwritten), the new one moved into its place. Any failure puts back
 *      every file moved so far. Data/Update/installed.txt lists them;
 *   4. Restart starts the new exe with "-updatewait <our pid>" and quits the
 *      normal way (the ini saver runs). The new game waits for us to be gone,
 *      then deletes the .old files (portUpdateBoot).
 *
 * Everything slow runs on one worker thread; the menus only read the state.
 *
 * Harness: GE_UPDATE_API=<url> asks there instead of GitHub (a local mock),
 * GE_UPDATE_FAKEVER=<tag> claims to be that release, GE_UPDATE_AUTO=install
 * installs what a check finds, =restart also restarts into it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <wchar.h>   /* before windows.h, which uses wcscpy */
#include <windows.h>
#include <winhttp.h>
#include <direct.h>
#include <process.h>
#define rmdir _rmdir
#else
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#include "port_buildinfo.h"
#include "port_update.h"

/* the game's include/ comes first and its string.h is the N64 one, which
 * lacks these: declared here, or they would be ints under -w */
extern int strcmp(const char *a, const char *b);
extern int strncmp(const char *a, const char *b, size_t n);
extern char *strstr(const char *s, const char *find);
extern size_t strcspn(const char *s, const char *reject);
extern void *memchr(const void *s, int c, size_t n);

extern void portConLogf(const char *fmt, ...);
extern const char *portRootPath(const char *name);
extern int portVideoAutoUpdate(void);   /* video.c: [display] auto_update */
extern void portRequestQuit(void);      /* video.c: the close box's quit */

#ifndef PORT_BUILD_RELEASE
#define PORT_BUILD_RELEASE ""
#endif

#define UPD_API_DEFAULT "https://api.github.com/repos/reaffirmed/goldeneye-coop/releases/latest"
#define UPD_AGENT       "GoldenEye-Coop-Updater"
#define UPD_DIR         "Data/Update"
#define UPD_MAX_ZIP     (64L * 1024 * 1024)
#define UPD_MAX_FILES   64

#ifdef _WIN32
#define UPD_ASSET_TAIL  "-win32.zip"
#else
#define UPD_ASSET_TAIL  "-linux.zip"
#endif

static volatile int sState = PORTUPD_IDLE;
static volatile int sProgress;
static volatile int sBusy;              /* the worker is running */
static char sUpdatedTo[48];             /* this run started from an update's Restart, into this release */
static int sStarted;                    /* portUpdateTick has run the startup check */
static int sRestarted;                  /* started by an update's Restart ("-updatewait") */
static unsigned sLastForced;            /* portUpdateTick count of the last forced check */
static unsigned sTicks;
static char sOwn[48];
static char sLatest[48];
static char sError[96];
static char sExe[260];                  /* the exe's own name beside the root, taken at boot */
static struct PortUpdRelease sRelease;
static int sAsset = -1;                 /* sRelease.assets index for this system */

static int sArgc;
static char **sArgv;

#ifdef _WIN32
static CRITICAL_SECTION sLock;
static void lockInit(void) { InitializeCriticalSection(&sLock); }
static void lock(void) { EnterCriticalSection(&sLock); }
static void unlock(void) { LeaveCriticalSection(&sLock); }
#else
static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static void lockInit(void) { }
static void lock(void) { pthread_mutex_lock(&sLock); }
static void unlock(void) { pthread_mutex_unlock(&sLock); }
#endif

static void setFailed(const char *why)
{
    lock();
    snprintf(sError, sizeof(sError), "%s", why);
    unlock();
    portConLogf("ge: update: %s\n", why);
    sState = PORTUPD_FAILED;
}

/* ---- files ---------------------------------------------------------------------- */

static void makeDir(const char *path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

/* every folder above a file */
static void makeParents(const char *path)
{
    char buf[1200];
    size_t i;

    snprintf(buf, sizeof(buf), "%s", path);
    for (i = 1; buf[i] != '\0'; i++)
    {
        if (buf[i] == '/' || buf[i] == '\\')
        {
            const char c = buf[i];

            buf[i] = '\0';
            makeDir(buf);
            buf[i] = c;
        }
    }
}

static int fileExists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

/* rename, replacing what is there (rename() won't on Windows) */
static int moveFile(const char *from, const char *to)
{
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

static char *rootPathCopy(const char *name)
{
    static char paths[4][1200];
    static int next;
    char *p = paths[next++ & 3];

    snprintf(p, 1200, "%s", portRootPath(name));
    return p;
}

/* a read-only install (Program Files, a locked folder): the download has
 * nowhere to go, which curl and WinHTTP would only report as a write error */
#define UPD_READONLY "Can't write to the game's folder; move it somewhere you own"

static int canWrite(const char *path)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL)
    {
        portConLogf("ge: update: can't write %s\n", path);
        return 0;
    }
    fclose(f);
    return 1;
}

static unsigned char *readFile(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (f == NULL)
    {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > UPD_MAX_ZIP)
    {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t) n + 1);
    if (buf == NULL || fread(buf, 1, (size_t) n, f) != (size_t) n)
    {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    *len = n;
    return buf;
}

#ifndef _WIN32
static long fileSize(const char *path)
{
    struct stat st;

    return (stat(path, &st) == 0) ? (long) st.st_size : -1;
}
#endif

/* ---- HTTP GET to a file ----------------------------------------------------------- */

#ifdef _WIN32
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

static int httpGet(const char *url, const char *outpath, long expect, char *err, size_t errcap)
{
    wchar_t wurl[1024], host[256], path[2048], extra[1024];
    URL_COMPONENTS uc;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD status = 0, statuslen = sizeof(status);
    FILE *f = NULL;
    long got = 0;
    int rc = -1;

    if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024) == 0)
    {
        snprintf(err, errcap, "bad address");
        return -1;
    }
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc))
    {
        snprintf(err, errcap, "bad address");
        return -1;
    }
    if (uc.dwExtraInfoLength > 0 && wcslen(path) + wcslen(extra) < 2048)
    {
        wcscat(path, extra);
    }
    /* Windows 8.1+ finds the system's proxy itself; older ones take the default */
    ses = WinHttpOpen(L"" UPD_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses == NULL)
    {
        ses = WinHttpOpen(L"" UPD_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (ses == NULL)
    {
        snprintf(err, errcap, "no internet access (%lu)", GetLastError());
        return -1;
    }
    WinHttpSetTimeouts(ses, 15000, 15000, 20000, 30000);
    con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (con != NULL)
    {
        req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
    }
    if (req == NULL
        || !WinHttpSendRequest(req, L"Accept: application/vnd.github+json, application/octet-stream\r\n", (DWORD) -1,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(req, NULL))
    {
        snprintf(err, errcap, "couldn't reach %s (%lu)", strstr(url, "api.github.com") ? "GitHub" : "the server", GetLastError());
        goto done;
    }
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &statuslen, WINHTTP_NO_HEADER_INDEX);
    if (status != 200)
    {
        snprintf(err, errcap, (status == 403 || status == 429) ? "GitHub is busy (%lu), try later" : "server said %lu", status);
        goto done;
    }
    f = fopen(outpath, "wb");
    if (f == NULL)
    {
        snprintf(err, errcap, "can't write %s", outpath);
        goto done;
    }
    for (;;)
    {
        static char buf[65536];
        DWORD n = 0;

        if (!WinHttpReadData(req, buf, sizeof(buf), &n))
        {
            snprintf(err, errcap, "download cut off (%lu)", GetLastError());
            goto done;
        }
        if (n == 0)
        {
            break;
        }
        if (fwrite(buf, 1, n, f) != n)
        {
            snprintf(err, errcap, "disk full?");
            goto done;
        }
        got += (long) n;
        if (got > UPD_MAX_ZIP)
        {
            snprintf(err, errcap, "download too big");
            goto done;
        }
        if (expect > 0)
        {
            sProgress = (int) ((double) got * 100.0 / (double) expect);
        }
    }
    rc = 0;
done:
    if (f != NULL && fclose(f) != 0 && rc == 0)
    {
        snprintf(err, errcap, "disk full?");
        rc = -1;
    }
    if (req != NULL) WinHttpCloseHandle(req);
    if (con != NULL) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return rc;
}
#else
/* a child with none of our sockets or files: the restarted game must be able
 * to open what we had open (Steam's, the mock's port). Returns its pid, or -1 */
static pid_t spawnClean(const char *file, char *const args[], int search)
{
    const pid_t pid = fork();

    if (pid == 0)
    {
        long fd, max = sysconf(_SC_OPEN_MAX);

        if (max < 0 || max > 65536)
        {
            max = 65536;
        }
        for (fd = 3; fd < max; fd++)
        {
            close((int) fd);
        }
        if (search)
        {
            execvp(file, args);
        }
        else
        {
            execv(file, args);
        }
        _exit(127);
    }
    return pid;
}

/* curl as a child (no shell: the address comes from GitHub's answer) */
static int httpGet(const char *url, const char *outpath, long expect, char *err, size_t errcap)
{
    char *args[] = {
        "curl", "-fsSL", "--proto", "=https,http", "--connect-timeout", "15", "--max-time", "900",
        "-A", UPD_AGENT, "-H", "Accept: application/vnd.github+json, application/octet-stream",
        "-o", (char *) outpath, (char *) url, NULL
    };
    pid_t pid;
    int status = 0;

    pid = spawnClean("curl", args, 1);
    if (pid < 0)
    {
        snprintf(err, errcap, "needs curl to download");
        return -1;
    }
    for (;;)
    {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        struct timespec ts = { 0, 100 * 1000000L };

        if (r == pid)
        {
            break;
        }
        if (r < 0)
        {
            snprintf(err, errcap, "lost the download");
            return -1;
        }
        if (expect > 0)
        {
            const long got = fileSize(outpath);

            if (got > 0)
            {
                sProgress = (int) ((double) got * 100.0 / (double) expect);
            }
        }
        nanosleep(&ts, NULL);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

        snprintf(err, errcap, code == 127 ? "needs curl to download" : code == 22 ? "the server refused (curl 22)"
                             : (code == 6 || code == 7) ? "couldn't reach the server (curl %d)" : "download failed (curl %d)", code);
        return -1;
    }
    return 0;
}
#endif

/* ---- check -------------------------------------------------------------------------- */

static const char *apiUrl(void)
{
    const char *e = getenv("GE_UPDATE_API");

    return (e != NULL && e[0] != '\0') ? e : UPD_API_DEFAULT;
}

/* quiet: the startup check, which fails without a word (offline is normal) */
static void checkFailed(const char *why, int quiet)
{
    if (!quiet)
    {
        setFailed(why);
        return;
    }
    portConLogf("ge: update: %s\n", why);
    sState = PORTUPD_IDLE;
}

static void doCheck(int quiet)
{
    char err[96] = "";
    char *path = rootPathCopy(UPD_DIR "/release.json");
    unsigned char *json;
    long len = 0;
    struct PortUpdRelease r;
    int i, asset = -1;

    makeDir(rootPathCopy("Data"));
    makeDir(rootPathCopy(UPD_DIR));
    portConLogf("ge: update: asking %s (we are %s)\n", apiUrl(), sOwn[0] != '\0' ? sOwn : "an unknown release");
    if (!canWrite(path))
    {
        checkFailed(UPD_READONLY, quiet);
        return;
    }
    if (httpGet(apiUrl(), path, 0, err, sizeof(err)) != 0)
    {
        char why[96];

        snprintf(why, sizeof(why), "Couldn't check: %s", err);
        checkFailed(why, quiet);
        return;
    }
    json = readFile(path, &len);
    remove(path);
    if (json == NULL || portUpdParseRelease((const char *) json, (size_t) len, &r) != 0)
    {
        free(json);
        checkFailed("Couldn't check: GitHub's answer made no sense", quiet);
        return;
    }
    free(json);
    for (i = 0; i < r.nassets; i++)
    {
        const size_t n = strlen(r.assets[i].name), t = strlen(UPD_ASSET_TAIL);

        if (n > t && strcmp(r.assets[i].name + n - t, UPD_ASSET_TAIL) == 0)
        {
            asset = i;
            break;
        }
    }
    lock();
    sRelease = r;
    sAsset = asset;
    snprintf(sLatest, sizeof(sLatest), "%s", r.tag);
    unlock();
    if (portUpdateNewer(r.tag))
    {
        portConLogf("ge: update: %s is out (%s), we are %s\n", r.tag, asset >= 0 ? r.assets[asset].name : "no zip for this system",
                    sOwn[0] != '\0' ? sOwn : "unknown");
        sState = PORTUPD_AVAILABLE;
    }
    else
    {
        portConLogf("ge: update: %s is the newest, we are %s\n", r.tag, sOwn);
        sState = PORTUPD_CURRENT;
    }
}

/* ---- install --------------------------------------------------------------------------- */

struct Staged {
    char rel[260];
};

/* the zip's top folder: the one holding the exe */
static int zipTopFolder(const unsigned char *zip, size_t len, char *top, size_t cap)
{
    const int n = portUpdZipCount(zip, len);
    int i;

    for (i = 0; i < n; i++)
    {
        struct PortUpdZipEntry e;
        const char *slash;

        if (portUpdZipEntry(zip, len, i, &e) != 0 || e.dir)
        {
            continue;
        }
        slash = strchr(e.name, '/');
        if (slash != NULL && strcmp(slash + 1, sExe) == 0 && (size_t) (slash - e.name) + 2 <= cap)
        {
            memcpy(top, e.name, (size_t) (slash - e.name) + 1);
            top[slash - e.name + 1] = '\0';
            return 0;
        }
    }
    return -1;
}

/* unpack what may be written to Data/Update/new/; the count, or -1 */
static int unpack(const unsigned char *zip, size_t len, struct Staged *staged, int maxstaged)
{
    const int n = portUpdZipCount(zip, len);
    char top[64];
    int i, count = 0;

    if (n <= 0)
    {
        setFailed("The download isn't a zip");
        return -1;
    }
    if (zipTopFolder(zip, len, top, sizeof(top)) != 0)
    {
        char why[96];

        snprintf(why, sizeof(why), "The zip has no %s", sExe);
        setFailed(why);
        return -1;
    }
    for (i = 0; i < n; i++)
    {
        struct PortUpdZipEntry e;
        const char *rel;
        unsigned char *data;
        char dest[400];
        FILE *f;

        if (portUpdZipEntry(zip, len, i, &e) != 0)
        {
            setFailed("The zip is damaged");
            return -1;
        }
        if (e.dir || strncmp(e.name, top, strlen(top)) != 0)
        {
            continue;
        }
        rel = e.name + strlen(top);
        if (!portUpdPathAllowed(rel))
        {
            portConLogf("ge: update: leaving %s alone\n", e.name);
            continue;
        }
        if (count >= maxstaged || e.size > (unsigned long) UPD_MAX_ZIP)
        {
            setFailed("The zip has too much in it");
            return -1;
        }
        data = malloc(e.size + 1);
        if (data == NULL || portUpdZipExtract(zip, len, i, data) != 0)
        {
            free(data);
            setFailed("The zip is damaged");
            return -1;
        }
        snprintf(dest, sizeof(dest), UPD_DIR "/new/%s", rel);
        makeParents(rootPathCopy(dest));
        f = fopen(rootPathCopy(dest), "wb");
        if (f == NULL || fwrite(data, 1, e.size, f) != e.size || fclose(f) != 0)
        {
            free(data);
            setFailed("Couldn't unpack the update (disk full?)");
            return -1;
        }
        free(data);
        snprintf(staged[count].rel, sizeof(staged[count].rel), "%s", rel);
        count++;
    }
    return count;
}

/* put back the first `done` files swapped in */
static void rollBack(const struct Staged *staged, int done, const int *hadold)
{
    while (done-- > 0)
    {
        char target[1200], old[1200];

        snprintf(target, sizeof(target), "%s", portRootPath(staged[done].rel));
        snprintf(old, sizeof(old), "%s.old", target);
        if (hadold[done])
        {
            moveFile(old, target);
        }
        else
        {
            remove(target);
        }
    }
}

static void doInstall(void)
{
    static struct Staged staged[UPD_MAX_FILES];
    int hadold[UPD_MAX_FILES];
    struct PortUpdAsset a;
    char tag[48];
    char err[96] = "";
    char name[200], part[220], hex[65];
    unsigned char *zip;
    long len = 0;
    int count, i;
    FILE *f;

    lock();
    if (sAsset < 0)
    {
        unlock();
        setFailed("No download for this system yet");
        return;
    }
    a = sRelease.assets[sAsset];
    snprintf(tag, sizeof(tag), "%s", sRelease.tag);
    unlock();
    if (strchr(a.name, '/') != NULL || strchr(a.name, '\\') != NULL || strstr(a.name, "..") != NULL)
    {
        setFailed("The release's file name is odd");
        return;
    }
    if (a.sha256[0] == '\0')
    {   /* nothing to check it against: don't install it */
        setFailed("GitHub gave no checksum for the zip");
        return;
    }

    /* 1: download and check */
    sProgress = 0;
    sState = PORTUPD_DOWNLOADING;
    makeDir(rootPathCopy("Data"));
    makeDir(rootPathCopy(UPD_DIR));
    snprintf(name, sizeof(name), UPD_DIR "/%s", a.name);
    snprintf(part, sizeof(part), "%s.part", name);
    if (!canWrite(rootPathCopy(part)))
    {
        setFailed(UPD_READONLY);
        return;
    }
    portConLogf("ge: update: downloading %s (%ld bytes)\n", a.url, a.size);
    if (httpGet(a.url, rootPathCopy(part), a.size, err, sizeof(err)) != 0)
    {
        char why[96];

        remove(rootPathCopy(part));
        snprintf(why, sizeof(why), "Download failed: %s", err);
        setFailed(why);
        return;
    }
    sProgress = 100;
    sState = PORTUPD_INSTALLING;
    zip = readFile(rootPathCopy(part), &len);
    if (zip == NULL || (a.size > 0 && len != a.size))
    {
        free(zip);
        remove(rootPathCopy(part));
        setFailed("The download came out the wrong size");
        return;
    }
    portUpdSha256(zip, (size_t) len, hex);
    if (strcmp(hex, a.sha256) != 0)
    {
        portConLogf("ge: update: %s has sha256 %s, GitHub lists %s\n", a.name, hex, a.sha256);
        free(zip);
        remove(rootPathCopy(part));
        setFailed("The download doesn't match its checksum");
        return;
    }
    moveFile(rootPathCopy(part), rootPathCopy(name));
    portConLogf("ge: update: %s checked (sha256 %.16s...)\n", a.name, hex);

    /* 2: unpack beside us */
    count = unpack(zip, (size_t) len, staged, UPD_MAX_FILES);
    free(zip);
    if (count <= 0)
    {
        if (count == 0)
        {
            setFailed("Nothing in the zip to install");
        }
        return;
    }

    /* 3: swap: the old file aside as <name>.old, the new one in */
    for (i = 0; i < count; i++)
    {
        char target[1200], old[1200], src[1200];

        snprintf(target, sizeof(target), "%s", portRootPath(staged[i].rel));
        snprintf(old, sizeof(old), "%s.old", target);
        snprintf(src, sizeof(src), "%s", portRootPath(UPD_DIR "/new/"));
        snprintf(src + strlen(src), sizeof(src) - strlen(src), "%s", staged[i].rel);
        remove(old);
        makeParents(target);
        hadold[i] = fileExists(target);
        if (hadold[i] && moveFile(target, old) != 0)
        {
            portConLogf("ge: update: can't move %s aside\n", staged[i].rel);
            rollBack(staged, i, hadold);
            setFailed("A game file is in use; close other copies and retry");
            return;
        }
        if (moveFile(src, target) != 0)
        {
            portConLogf("ge: update: can't put %s in place\n", staged[i].rel);
            if (hadold[i])
            {
                moveFile(old, target);
            }
            rollBack(staged, i, hadold);
            setFailed("Couldn't replace the game files");
            return;
        }
#ifndef _WIN32
        if (strcmp(staged[i].rel, sExe) == 0)
        {
            chmod(target, 0755);
        }
#endif
    }
    f = fopen(rootPathCopy(UPD_DIR "/installed.txt"), "w");
    if (f != NULL)
    {
        fprintf(f, "%s %s %s\n", sOwn[0] != '\0' ? sOwn : "unknown", tag, a.name);
        for (i = 0; i < count; i++)
        {
            fprintf(f, "%s\n", staged[i].rel);
        }
        fclose(f);
    }
    portConLogf("ge: update: installed %s (%d files); restart to play it\n", tag, count);
    sState = PORTUPD_READY;
}

/* ---- worker ------------------------------------------------------------------------------- */

enum { JOB_CHECK, JOB_CHECK_QUIET, JOB_INSTALL };

#ifdef _WIN32
static unsigned __stdcall worker(void *arg)
#else
static void *worker(void *arg)
#endif
{
    if ((int) (size_t) arg == JOB_INSTALL)
    {
        doInstall();
    }
    else
    {
        doCheck((int) (size_t) arg == JOB_CHECK_QUIET);
    }
    sBusy = 0;
    return 0;
}

static void startJob(int job)
{
    if (sBusy)
    {
        return;
    }
    sBusy = 1;
    sState = (job == JOB_INSTALL) ? PORTUPD_DOWNLOADING : PORTUPD_CHECKING;
#ifdef _WIN32
    {
        const uintptr_t h = _beginthreadex(NULL, 0, worker, (void *) (size_t) job, 0, NULL);

        if (h == 0)
        {
            sBusy = 0;
            setFailed("Couldn't start the updater");
            return;
        }
        CloseHandle((HANDLE) h);
    }
#else
    {
        pthread_t t;

        if (pthread_create(&t, NULL, worker, (void *) (size_t) job) != 0)
        {
            sBusy = 0;
            setFailed("Couldn't start the updater");
            return;
        }
        pthread_detach(t);
    }
#endif
}

/* ---- boot and the frame tick ---------------------------------------------------------------- */

/* "-updatewait <pid>": the game that installed us is quitting; let it go first */
static void waitForOld(unsigned long pid)
{
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD) pid);

    if (h != NULL)
    {
        WaitForSingleObject(h, 15000);
        CloseHandle(h);
    }
#else
    int i;

    for (i = 0; i < 150 && kill((pid_t) pid, 0) == 0; i++)
    {
        struct timespec ts = { 0, 100 * 1000000L };

        nanosleep(&ts, NULL);
    }
#endif
}

/* after an update: the .old files go, and the log says what changed;
 * `to` gets the release installed ("" when there was no update) */
static void tidyUp(char *to, size_t tocap)
{
    char *list = rootPathCopy(UPD_DIR "/installed.txt");
    FILE *f = fopen(list, "r");
    char line[300], from[48] = "", zip[160] = "";
    int left = 0;

    to[0] = '\0';
    if (f == NULL)
    {
        return;
    }
    if (fgets(line, sizeof(line), f) != NULL && tocap >= 48)
    {
        sscanf(line, "%47s %47s %159s", from, to, zip);
    }
    while (fgets(line, sizeof(line), f) != NULL)
    {
        char old[1200];

        line[strcspn(line, "\r\n")] = '\0';
        if (!portUpdPathAllowed(line))
        {
            continue;
        }
        snprintf(old, sizeof(old), "%s.old", portRootPath(line));
        if (fileExists(old) && remove(old) != 0)
        {
            left++;   /* still in use: next time */
        }
    }
    fclose(f);
    if (left == 0)
    {
        if (zip[0] != '\0' && strchr(zip, '/') == NULL && strchr(zip, '\\') == NULL && strstr(zip, "..") == NULL)
        {   /* the download, kept until the new build ran */
            char path[200];

            snprintf(path, sizeof(path), UPD_DIR "/%s", zip);
            remove(rootPathCopy(path));
        }
        remove(list);
        rmdir(rootPathCopy(UPD_DIR "/new/Data"));
        rmdir(rootPathCopy(UPD_DIR "/new"));
    }
    portConLogf("ge: update: now on %s (was %s)%s\n", to, from, left ? ", some old files are still in use" : "");
}

void portUpdateBoot(int argc, char **argv)
{
    const char *fake = getenv("GE_UPDATE_FAKEVER");
    int i;

    lockInit();
    sArgc = argc;
    sArgv = argv;
    snprintf(sOwn, sizeof(sOwn), "%s", (fake != NULL) ? fake : PORT_BUILD_RELEASE);
    {   /* our own name, before an update renames the file under us */
#ifdef _WIN32
        char self[MAX_PATH];
        const DWORD n = GetModuleFileNameA(NULL, self, sizeof(self));
#else
        char self[1024];
        const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
#endif
        const char *base = self;

        if (n > 0 && (size_t) n < sizeof(self))
        {
            self[n] = '\0';
            for (i = 0; self[i] != '\0'; i++)
            {
                if (self[i] == '/' || self[i] == '\\')
                {
                    base = self + i + 1;
                }
            }
            snprintf(sExe, sizeof(sExe), "%s", base);
        }
        if (sExe[0] == '\0' || strstr(sExe, ".old") != NULL)
        {
#ifdef _WIN32
            snprintf(sExe, sizeof(sExe), "ProjectMidas.exe");
#else
            snprintf(sExe, sizeof(sExe), "ProjectMidas");
#endif
        }
    }
    for (i = 1; i + 1 < argc; i++)
    {
        if (strcmp(argv[i], "-updatewait") == 0)
        {
            waitForOld(strtoul(argv[i + 1], NULL, 10));
        }
    }
    {
        char to[48];
        int updated = 0;

        for (i = 1; i < argc; i++)
        {
            updated |= strcmp(argv[i], "-updatewait") == 0;
        }
        tidyUp(to, sizeof(to));
        if (fake != NULL && updated && to[0] != '\0')
        {   /* harness: the "new" build is this one; it is the release it was installed as */
            snprintf(sOwn, sizeof(sOwn), "%s", to);
        }
        sRestarted = updated;
        if (updated)
        {
            snprintf(sUpdatedTo, sizeof(sUpdatedTo), "%s", to);
        }
    }
    portConLogf("ge: update: this is %s (%s), checks %s\n", sOwn[0] != '\0' ? sOwn : "an unknown release", sExe, apiUrl());
}

/* boss.c, once a frame (the ini is read by now) */
void portUpdateTick(void)
{
    const char *autoinstall = getenv("GE_UPDATE_AUTO");

    sTicks++;
    if (!sStarted)
    {
        sStarted = 1;
        if (portUpdateEnabled())
        {
            startJob(JOB_CHECK_QUIET);
        }
        else
        {
            sState = PORTUPD_OFF;
        }
    }
    if (autoinstall != NULL && !sBusy && !sRestarted)
    {   /* harness: take what a check found (not again after its restart) */
        if (sState == PORTUPD_AVAILABLE)
        {
            portUpdateInstall();
        }
        else if (sState == PORTUPD_READY && strcmp(autoinstall, "restart") == 0)
        {
            portUpdateRestart();
        }
    }
}

/* ---- the menus' side -------------------------------------------------------------------------- */

int portUpdateEnabled(void)
{
    return portVideoAutoUpdate() != 0;
}

void portUpdateCheck(int force)
{
    if (sBusy || sState == PORTUPD_READY || sState == PORTUPD_DOWNLOADING || sState == PORTUPD_INSTALLING)
    {
        return;
    }
    if (!force && !portUpdateEnabled())
    {
        return;
    }
    if (sState == PORTUPD_AVAILABLE)
    {   /* already know */
        return;
    }
    if (force && sLastForced != 0 && sTicks - sLastForced < 60 * 10)
    {   /* GitHub allows 60 unauthenticated asks an hour: one in ten seconds at most */
        return;
    }
    sLastForced = sTicks | 1;
    startJob(JOB_CHECK);
}

int portUpdateState(void)
{
    return sState;
}

int portUpdateProgress(void)
{
    const int p = sProgress;

    return (p < 0) ? 0 : (p > 100) ? 100 : p;
}

const char *portUpdateOwn(void)
{
    return sOwn;
}

const char *portUpdateLatest(void)
{
    static char copy[48];

    lock();
    snprintf(copy, sizeof(copy), "%s", sLatest);
    unlock();
    return copy;
}

const char *portUpdateError(void)
{
    static char copy[96];

    lock();
    snprintf(copy, sizeof(copy), "%s", sError);
    unlock();
    return copy;
}

int portUpdateNewer(const char *tag)
{
    if (tag == NULL || tag[0] == '\0')
    {
        return 0;
    }
    if (sOwn[0] == '\0')
    {   /* built from source without tags: anything released counts as newer */
        return portUpdCompareVersions(tag, "v0.0.0") > 0;
    }
    return portUpdCompareVersions(tag, sOwn) == 1;
}

int portUpdateHasDownload(void)
{
    int has;

    lock();
    has = sAsset >= 0;
    unlock();
    return has;
}

void portUpdateInstall(void)
{
    if (sState != PORTUPD_AVAILABLE && !(sState == PORTUPD_FAILED && sLatest[0] != '\0' && portUpdateNewer(sLatest)))
    {
        return;
    }
    if (!portUpdateHasDownload())
    {
        setFailed("No download for this system yet");
        return;
    }
    startJob(JOB_INSTALL);
}

const char *portUpdateJustUpdated(void)
{
    return sUpdatedTo;
}

void portUpdateRestart(void)
{
    char *exe = rootPathCopy(sExe);

    if (sState != PORTUPD_READY)
    {
        return;
    }
    portConLogf("ge: update: restarting into %s\n", exe);
#ifdef _WIN32
    {
        static wchar_t cmd[4096];
        wchar_t wexe[1200];
        const wchar_t *line = GetCommandLineW();
        const wchar_t *was = wcsstr(line, L" -updatewait ");
        size_t keep = (was != NULL) ? (size_t) (was - line) : wcslen(line);
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;

        if (keep > 3500)
        {
            keep = 3500;
        }
        wcsncpy(cmd, line, keep);
        cmd[keep] = L'\0';
        _snwprintf(cmd + keep, 4096 - keep, L" -updatewait %lu", (unsigned long) GetCurrentProcessId());
        MultiByteToWideChar(CP_ACP, 0, exe, -1, wexe, 1200);   /* GetModuleFileNameA spelling */
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        if (!CreateProcessW(wexe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        {
            setFailed("Installed, but couldn't start it: start the game again");
            return;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#else
    {
        char *args[64];
        char pid[24];
        pid_t child;
        int i, n = 0;

        args[n++] = exe;
        for (i = 1; i < sArgc && n < 60; i++)
        {
            if (strcmp(sArgv[i], "-updatewait") == 0)
            {
                i++;
                continue;
            }
            args[n++] = sArgv[i];
        }
        snprintf(pid, sizeof(pid), "%lu", (unsigned long) getpid());
        args[n++] = "-updatewait";
        args[n++] = pid;
        args[n] = NULL;
        child = spawnClean(exe, args, 0);
        if (child < 0)
        {
            setFailed("Installed, but couldn't start it: start the game again");
            return;
        }
    }
#endif
    portRequestQuit();
}
