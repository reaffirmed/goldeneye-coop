/**
 * The updater's pieces that need no game or network (port_update_util.c):
 *
 *   gcc -O1 -Iport/include -o update_test coop/ci/update_test.c port/src/port_update_util.c
 *   ./update_test [<zip> <dir> ...]
 *
 * Always: SHA-256 against the FIPS 180-2 vectors, a GitHub releases/latest
 * answer (with escapes, nesting and fields we skip), version order, and the
 * rule for which paths an update may write (the ROM, saves and settings
 * never). Each <zip> <dir> pair: every file in the zip is read back (inflate
 * and CRC) and must equal <dir>/<its name> byte for byte.
 * Exit 1 on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_update.h"

static int sFails;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); sFails++; } } while (0)

static unsigned char *readAll(const char *path, size_t *len)
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
    buf = malloc((size_t) n + 1);
    if (buf == NULL || fread(buf, 1, (size_t) n, f) != (size_t) n)
    {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *len = (size_t) n;
    return buf;
}

static void testSha256(void)
{
    static const struct { const char *in; const char *hex; } v[] = {
        { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
    };
    char hex[65];
    char *million;
    size_t i;

    for (i = 0; i < sizeof(v) / sizeof(v[0]); i++)
    {
        portUpdSha256(v[i].in, strlen(v[i].in), hex);
        CHECK(strcmp(hex, v[i].hex) == 0, "sha256(\"%s\") = %s", v[i].in, hex);
    }
    million = malloc(1000000);
    memset(million, 'a', 1000000);
    portUpdSha256(million, 1000000, hex);
    CHECK(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0, "sha256(a x 1e6) = %s", hex);
    free(million);
    CHECK(portUpdCrc32("123456789", 9) == 0xCBF43926UL, "crc32 check value");
}

static void testRelease(void)
{
    static const char json[] =
        "{\n  \"url\": \"https://api.github.com/repos/reaffirmed/goldeneye-coop/releases/1\",\n"
        "  \"tag_name\": \"v0.3.0\", \"target_commitish\": \"abc\", \"name\": \"GoldenEye \\\"Co-op\\\" v0.3.0\",\n"
        "  \"draft\": false, \"prerelease\": false, \"id\": 425874066,\n"
        "  \"author\": { \"login\": \"github-actions[bot]\", \"id\": 41898282, \"nested\": [1, 2.5e3, {\"a\": null}] },\n"
        "  \"html_url\": \"https://github.com/reaffirmed/goldeneye-coop/releases/tag/v0.3.0\",\n"
        "  \"assets\": [\n"
        "    { \"name\": \"GoldenEye-Coop-v0.3.0-win32.zip\", \"size\": 8583479, \"label\": \"\",\n"
        "      \"uploader\": { \"login\": \"x\" },\n"
        "      \"browser_download_url\": \"https://github.com/reaffirmed/goldeneye-coop/releases/download/v0.3.0/GoldenEye-Coop-v0.3.0-win32.zip\",\n"
        "      \"digest\": \"sha256:67EB7B8419A06B1A0E300A8053208B5D6901F0E11C0B1E0859D19BF9AF817366\" },\n"
        "    { \"name\": \"notes\\u00e9.txt\", \"size\": 12, \"digest\": null, \"browser_download_url\": \"https://x/y\" }\n"
        "  ],\n"
        "  \"body\": \"Line one\\nLine \\u0022two\\u0022 \\\\ end\"\n}\n";
    struct PortUpdRelease r;

    CHECK(portUpdParseRelease(json, strlen(json), &r) == 0, "release parse");
    CHECK(strcmp(r.tag, "v0.3.0") == 0, "tag %s", r.tag);
    CHECK(strcmp(r.page, "https://github.com/reaffirmed/goldeneye-coop/releases/tag/v0.3.0") == 0, "page %s", r.page);
    CHECK(r.nassets == 2, "assets %d", r.nassets);
    CHECK(strcmp(r.assets[0].name, "GoldenEye-Coop-v0.3.0-win32.zip") == 0, "asset name %s", r.assets[0].name);
    CHECK(r.assets[0].size == 8583479, "asset size %ld", r.assets[0].size);
    CHECK(strcmp(r.assets[0].sha256, "67eb7b8419a06b1a0e300a8053208b5d6901f0e11c0b1e0859d19bf9af817366") == 0, "digest %s", r.assets[0].sha256);
    CHECK(strstr(r.assets[0].url, "/download/v0.3.0/") != NULL, "url %s", r.assets[0].url);
    CHECK(r.assets[1].sha256[0] == '\0' && strcmp(r.assets[1].name, "notes?.txt") == 0, "second asset %s", r.assets[1].name);
    CHECK(portUpdParseRelease("{\"message\":\"Not Found\"}", 23, &r) != 0, "an error answer is not a release");
    CHECK(portUpdParseRelease("{\"tag_name\": \"v1\"", 17, &r) != 0, "a cut answer is not a release");
    CHECK(portUpdParseRelease("", 0, &r) != 0, "an empty answer is not a release");
}

static void testVersions(void)
{
    CHECK(portUpdCompareVersions("v0.2.1", "v0.2.2") == -1, "0.2.1 < 0.2.2");
    CHECK(portUpdCompareVersions("v0.2.10", "v0.2.9") == 1, "0.2.10 > 0.2.9");
    CHECK(portUpdCompareVersions("v0.3", "v0.3.0") == 0, "0.3 = 0.3.0");
    CHECK(portUpdCompareVersions("0.2.1", "v0.2.1") == 0, "without the v");
    CHECK(portUpdCompareVersions("v1.0.0", "v0.9.9") == 1, "1.0.0 > 0.9.9");
    CHECK(portUpdCompareVersions("v0.2.1-rc1", "v0.2.1") == 0, "a suffix is ignored");
    CHECK(portUpdCompareVersions("coop-latest", "v0.2.1") == -2, "not a version");
    CHECK(portUpdCompareVersions("", "v0.2.1") == -2, "empty");
}

static void testPaths(void)
{
    static const char *const yes[] = {
        "ProjectMidas.exe", "SDL2.dll", "steam_api.dll", "steam_appid.txt", "Host co-op.bat",
        "README-COOP.txt", "gamecontrollerdb.txt", "Data/README.txt", "Data/shaders_base.txt", "ProjectMidas",
    };
    static const char *const no[] = {
        "", "/etc/passwd", "../ProjectMidas.exe", "Data/../../x", "C:/Windows/x.dll", "a\\b.dll",
        "midas.ini", "MIDAS.INI", "Data/baserom.u.z64", "Data/GoldenEye.Z64", "Data/ge.n64", "ge.v64",
        "Data/Saves/eeprom.bin", "Data/Saves/coop_campaign.txt", "Data/Profiles/a.txt", "Data/Logs/ge_direct.log",
        "Data/Crashes/pending.txt", "Data/foo.dll", "coop_campaign.txt", "coop_character.txt", "eeprom.bin",
        "Mods/x.dll", "Data/", ".hidden", "ProjectMidas.exe.old", "x\001y",
    };
    size_t i;

    for (i = 0; i < sizeof(yes) / sizeof(yes[0]); i++)
    {
        CHECK(portUpdPathAllowed(yes[i]), "may write %s", yes[i]);
    }
    for (i = 0; i < sizeof(no) / sizeof(no[0]); i++)
    {
        CHECK(!portUpdPathAllowed(no[i]), "must not write %s", no[i]);
    }
}

static void testZip(const char *zippath, const char *dir)
{
    size_t len, flen;
    unsigned char *zip = readAll(zippath, &len);
    int n, i, files = 0;

    CHECK(zip != NULL, "read %s", zippath);
    if (zip == NULL)
    {
        return;
    }
    n = portUpdZipCount(zip, len);
    CHECK(n > 0, "%s: %d entries", zippath, n);
    for (i = 0; i < n; i++)
    {
        struct PortUpdZipEntry e;
        unsigned char *out, *want;
        char path[600];

        if (portUpdZipEntry(zip, len, i, &e) != 0)
        {
            CHECK(0, "%s: entry %d", zippath, i);
            continue;
        }
        if (e.dir)
        {
            continue;
        }
        out = malloc(e.size + 1);
        CHECK(portUpdZipExtract(zip, len, i, out) == 0, "%s: extract %s", zippath, e.name);
        snprintf(path, sizeof(path), "%s/%s", dir, e.name);
        want = readAll(path, &flen);
        CHECK(want != NULL && flen == e.size && memcmp(want, out, flen) == 0, "%s: %s differs from %s", zippath, e.name, path);
        free(out);
        free(want);
        files++;
    }
    /* a damaged copy must not extract cleanly */
    if (len > 200)
    {
        int bad = 0;

        zip[len / 3] ^= 0x55;
        for (i = 0; i < n; i++)
        {
            struct PortUpdZipEntry e;
            unsigned char *out;

            if (portUpdZipEntry(zip, len, i, &e) != 0 || e.dir)
            {
                continue;
            }
            out = malloc(e.size + 1);
            bad += portUpdZipExtract(zip, len, i, out) != 0;
            free(out);
        }
        CHECK(bad > 0, "%s: a flipped byte went unnoticed", zippath);
    }
    printf("%s: %d files match\n", zippath, files);
    free(zip);
}

int main(int argc, char **argv)
{
    int i;

    testSha256();
    testRelease();
    testVersions();
    testPaths();
    for (i = 1; i + 1 < argc; i += 2)
    {
        testZip(argv[i], argv[i + 1]);
    }
    printf("%s (%d failures)\n", sFails ? "FAILED" : "ok", sFails);
    return sFails ? 1 : 0;
}
