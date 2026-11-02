/**
 * port_update_launch.c - the update offer, asked when the game is opened.
 *
 * videoInit calls portUpdateLaunch after midas.ini is read and before the game
 * window opens. It starts the release check (port_update.c) and waits for it a
 * few seconds at most. If a newer release with a download for this system is
 * out, and the player hasn't skipped it, a system message box asks:
 *
 *   Update Now          downloads it with a small progress window, installs
 *                       it and starts the new build;
 *   Not Now             starts the game; asks again next launch;
 *   Skip This Version   starts the game; quiet until a newer release.
 *
 * Closing the progress window starts the game while the download finishes in
 * the background (the CO-OP screen shows it and offers Restart Now). A failed
 * download or install offers Try Again or Play Now.
 *
 * If the last install was cut off and finished at this start by the old build
 * (port_update.c resumeSwap), the box offers Restart Now instead.
 *
 * Nothing is asked with [display] auto_update=0, when the check doesn't answer
 * in time (offline), or in automated runs (GE_FRAMES, GE_NOMSGBOX), where a box
 * would hang the harness. GE_UPDATE_AUTO=install answers Update Now without a
 * box, for tests; GE_UPDATE_AUTO=restart also takes Restart Now.
 * GE_UPDATE_LAUNCHWAIT=<ms> changes how long the check is waited for.
 */
#include <SDL.h>    /* first: the game's own include/string.h clashes with mingw's intrin.h after it */
#include <stdio.h>
#include <stdlib.h>

#include "port_update.h"

extern void portConLogf(const char *fmt, ...);

#define LAUNCH_WAIT_MS 3000   /* GitHub usually answers in well under a second */

static int launchQuiet(void)
{
    return getenv("GE_FRAMES") != NULL || getenv("GE_NOMSGBOX") != NULL;
}

/* a box with up to three buttons, left to right; the id of the one picked,
 * or esc (the window closed, Escape) */
static int launchAsk(const char *title, const char *text, const char *const *labels, int n, int esc)
{
    SDL_MessageBoxButtonData buttons[3];
    SDL_MessageBoxData box;
    int i, hit = esc;

    SDL_memset(buttons, 0, sizeof(buttons));
    for (i = 0; i < n && i < 3; i++)
    {
        buttons[i].buttonid = i;
        buttons[i].text = labels[i];
        buttons[i].flags = (i == 0) ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT
                         : (i == esc) ? SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT : 0;
    }
    SDL_memset(&box, 0, sizeof(box));
    box.flags = SDL_MESSAGEBOX_INFORMATION;
#if SDL_VERSION_ATLEAST(2, 0, 12)
    box.flags |= SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
#endif
    box.title = title;
    box.message = text;
    box.numbuttons = (n < 3) ? n : 3;
    box.buttons = buttons;
    if (SDL_ShowMessageBox(&box, &hit) != 0 || hit < 0)
    {
        portConLogf("ge: update: launch box: %s\n", hit < 0 ? "closed" : SDL_GetError());
        hit = esc;
    }
    return hit;
}

/* start the new build and leave; returns only if that failed */
static void launchRestart(void)
{
    portUpdateRestart();
    if (portUpdateState() != PORTUPD_FAILED)
    {
        portConLogf("ge: update: started the new build, closing this one\n");
        exit(0);
    }
}

/* the download and install, with a small window showing how far it is.
 * 1: done (READY or FAILED); 0: the player closed the window to play now */
static int launchProgress(void)
{
    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    char title[96], shown[96] = "";
    int closed = 0, pulse = 0;

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) == 0)
    {
        win = SDL_CreateWindow("Updating GoldenEye Coop", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 480, 96, 0);
        if (win != NULL)
        {
            ren = SDL_CreateRenderer(win, -1, 0);
            if (ren == NULL)
            {
                ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
            }
        }
    }
    if (win == NULL)
    {
        portConLogf("ge: update: no progress window (%s); waiting without one\n", SDL_GetError());
    }
    for (;;)
    {
        const int st = portUpdateState();
        const int pct = portUpdateProgress();
        SDL_Event ev;

        if (st != PORTUPD_DOWNLOADING && st != PORTUPD_INSTALLING)
        {
            break;
        }
        while (win != NULL && SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_QUIT || (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE))
            {
                closed = 1;
            }
        }
        if (closed)
        {
            portConLogf("ge: update: progress window closed; the download goes on while the game runs\n");
            break;
        }
        if (st == PORTUPD_DOWNLOADING)
        {
            snprintf(title, sizeof(title), "Downloading GoldenEye Coop %s: %d%%", portUpdateLatest(), pct);
        }
        else
        {
            snprintf(title, sizeof(title), "Installing GoldenEye Coop %s...", portUpdateLatest());
        }
        if (win != NULL && SDL_strcmp(title, shown) != 0)
        {
            SDL_SetWindowTitle(win, title);
            snprintf(shown, sizeof(shown), "%s", title);
        }
        if (ren != NULL)
        {   /* a bar in the game's menu colours: dark field, lime fill */
            SDL_Rect frame = { 24, 36, 432, 24 };
            SDL_Rect fill = { 27, 39, 0, 18 };

            SDL_SetRenderDrawColor(ren, 0x16, 0x18, 0x14, 0xFF);
            SDL_RenderClear(ren);
            SDL_SetRenderDrawColor(ren, 0x48, 0x4E, 0x42, 0xFF);
            SDL_RenderDrawRect(ren, &frame);
            if (st == PORTUPD_DOWNLOADING)
            {
                fill.w = 426 * pct / 100;
            }
            else
            {   /* installing takes a moment and has no percent: a block moves along */
                pulse = (pulse + 6) % 426;
                fill.x += pulse;
                fill.w = (pulse + 80 > 426) ? 426 - pulse : 80;
            }
            SDL_SetRenderDrawColor(ren, 0xB6, 0xD8, 0x3C, 0xFF);
            SDL_RenderFillRect(ren, &fill);
            SDL_RenderPresent(ren);
        }
        SDL_Delay(33);
    }
    if (ren != NULL)
    {
        SDL_DestroyRenderer(ren);
    }
    if (win != NULL)
    {
        SDL_DestroyWindow(win);
    }
    if (SDL_WasInit(SDL_INIT_VIDEO))
    {   /* the game's own SDL_Init comes next; leave nothing queued for it */
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    return !closed;
}

/* they said Update Now */
static void launchInstall(void)
{
    static const char *const again[2] = { "Try Again", "Play Now" };
    char text[256];

    for (;;)
    {
        if (!launchProgress())
        {
            return;
        }
        if (portUpdateState() == PORTUPD_READY)
        {
            const char *autoans = getenv("GE_UPDATE_AUTO");

            if (autoans == NULL || SDL_strcmp(autoans, "restart") == 0)
            {
                launchRestart();
            }
            return;   /* GE_UPDATE_AUTO=install: tests restart it themselves */
        }
        if (portUpdateState() != PORTUPD_FAILED)
        {
            return;
        }
        portConLogf("ge: update: launch install failed: %s\n", portUpdateError());
        snprintf(text, sizeof(text), "The update to %s didn't install:\n%s\n\n"
                 "Nothing was changed. Play Now starts the game you have.",
                 portUpdateLatest(), portUpdateError());
        if (launchQuiet() || launchAsk("GoldenEye Coop update", text, again, 2, 1) != 0)
        {
            return;
        }
        portUpdateInstall();
    }
}

void portUpdateLaunch(void)
{
    static const char *const offer[3] = { "Update Now", "Not Now", "Skip This Version" };
    static const char *const restart[2] = { "Restart Now", "Later" };
    const char *autoans = getenv("GE_UPDATE_AUTO");
    const char *waitenv = getenv("GE_UPDATE_LAUNCHWAIT");
    const Uint32 wait = (waitenv != NULL) ? (Uint32) strtoul(waitenv, NULL, 10) : LAUNCH_WAIT_MS;
    const char *own = portUpdateOwn();
    Uint32 start;
    char text[512];
    int answer;

    portUpdateLaunchCheck();
    if (portUpdateState() == PORTUPD_READY)
    {   /* the last install was cut off and has just been finished: the new
         * build is in the folder, this is the old one */
        snprintf(text, sizeof(text), "The update to %s was cut off last time and has now been finished.\n\n"
                 "Restart Now to play %s.", portUpdateLatest(), portUpdateLatest());
        if ((autoans != NULL && SDL_strcmp(autoans, "restart") == 0)
            || (!launchQuiet() && launchAsk("GoldenEye Coop update", text, restart, 2, 1) == 0))
        {
            launchRestart();
        }
        return;
    }
    start = SDL_GetTicks();
    while (portUpdateState() == PORTUPD_CHECKING && SDL_GetTicks() - start < wait)
    {
        SDL_Delay(20);
    }
    if (portUpdateState() == PORTUPD_CHECKING)
    {
        portConLogf("ge: update: no answer from GitHub in %u ms; starting the game (the CO-OP screen shows an update if one is out)\n",
                    (unsigned) wait);
        return;
    }
    if (!portUpdatePromptWanted())
    {
        return;
    }
    if (autoans != NULL)
    {
        answer = PORTUPD_ANSWER_UPDATE;
    }
    else if (launchQuiet())
    {
        return;   /* not answered: the next launch asks */
    }
    else
    {
        const long bytes = portUpdateDownloadBytes();
        char size[48] = "";

        if (bytes > 0)
        {
            snprintf(size, sizeof(size), " (%.1f MB)", bytes / (1024.0 * 1024.0));
        }
        snprintf(text, sizeof(text), "GoldenEye Coop %s is out. You have %s.\n\n"
                 "Players need the same version to play co-op together.\n\n"
                 "Update Now downloads it%s, installs it and restarts the game. "
                 "Your saves, settings and cart image are not touched.",
                 portUpdateLatest(), own[0] != '\0' ? own : "a build from source", size);
        answer = launchAsk("GoldenEye Coop update", text, offer, 3, PORTUPD_ANSWER_LATER);
    }
    portUpdatePromptAnswer(answer);
    if (answer == PORTUPD_ANSWER_UPDATE)
    {
        launchInstall();
    }
}
