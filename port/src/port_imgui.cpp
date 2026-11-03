/**
 * port_imgui.cpp - Dear ImGui (port/imgui, MIT) over the game, for the co-op
 * colour picker: ImGui's own ColorPicker4, drawn and driven as ImGui draws
 * and drives it (the saturation/value square, the hue bar, the RGB, HSV and
 * hex inputs, the Current and Original swatches, the right-click options).
 *
 * Threads: the frame is built on the sim thread (portImguiFrame, called where
 * a game frame is handed to the renderer, video.c), which also owns the
 * window's events. ImGui's draw lists are copied there and handed over; the
 * thread that holds the GL context (the render thread when it runs) draws the
 * newest copy over the finished game frame (portImguiRender, gfx_pc.cpp
 * gfx_run). The GL backend is set up on that thread before the first frame
 * is built, so the font texture id never changes under a frame.
 *
 * Input: while the picker is up the game gets none (video.c and
 * libultra_shim.c blank the keyboard, mouse and pads as they do for the
 * console). Keys, text, the wheel and pad buttons reach ImGui through an SDL
 * event watch; the pointer is read once a frame.
 */
#include <SDL.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

extern "C" {
SDL_Window *gfx_sdl_get_window(void);
const char *portTrUtf8(const char *en);
void portConLogf(const char *fmt, ...);
int32_t portConIsOpen(void);
int32_t portTextInputActive(void);
int32_t portAccessColorblind(void);
void portInputMouseWindowPos(int32_t *x, int32_t *y, int32_t *moved);

int32_t portColourPickerOpen(int32_t rgb, int32_t defrgb, const char *name);
int32_t portColourPickerPoll(int32_t *rgb);
int32_t portColourPickerActive(void);
void portImguiFrame(void);
void portImguiRender(void);
}

enum { PICK_NONE = 0, PICK_OK = 1, PICK_CANCEL = 2 };

#define PICK_FONTS      4       /* ProggyClean at 13, 26, 39 and 52 px: whole multiples stay sharp */
#define PICK_POLL_MS    500     /* the lobby stopped asking (a mission started): close */
#define PICK_EVENTS     256

/* ---- sim thread ---- */
static int s_Open;              /* the picker is up */
static int s_Result;            /* PICK_*, until polled */
static int32_t s_ResultRgb;
static float s_Col[3];          /* the colour being picked */
static float s_Ref[3];          /* what it was when the picker opened (ImGui's Original) */
static float s_Def[3];          /* the player number's colour */
static int s_UseDefault;        /* Default Colour pressed, nothing changed since */
static char s_Name[48];
static Uint32 s_PollMs;
static int s_PopupOpened;       /* OpenPopup done for this opening */
static bool s_Busy;             /* last frame: something in the picker was being edited */
static int s_MouseHeld;         /* the click that opened it is still down: not ImGui's */
static int s_StartedText;       /* we started SDL text input */
static ImFont *s_Fonts[PICK_FONTS];
static int s_Scale;             /* the style's scale (1..PICK_FONTS) */
static ImGuiStyle s_BaseStyle;
static Uint64 s_LastCounter;

/* ---- between threads ---- */
static SDL_atomic_t s_GlWanted;  /* sim -> GL: set the backend up */
static SDL_atomic_t s_GlReady;   /* GL -> sim: it is */
static SDL_atomic_t s_GlFailed;  /* GL -> sim: it could not be: no picker this run */
static SDL_atomic_t s_Active;    /* the picker is up (read by the input blanks) */
static SDL_SpinLock s_SnapLock;

struct ImguiSnap
{
    std::vector<ImDrawList *> lists;
    ImVec2 pos, size, scale;
};
static ImguiSnap *s_SnapPending;  /* newest frame, not yet taken by the GL thread */
static ImguiSnap *s_SnapShown;    /* GL thread: what it draws each frame */

/* events from the watch, applied at the next frame (the watch can run on
 * another thread than the frame's) */
struct ImguiEvent
{
    int kind;       /* 0 key, 1 text, 2 wheel, 3 pad button, 4 pad axis */
    int key;        /* ImGuiKey */
    int down;
    float value;
    char text[32];
};
static SDL_mutex *s_EvMtx;
static ImguiEvent s_Ev[PICK_EVENTS];
static int s_EvCount;

static void snapFree(ImguiSnap *s)
{
    if (s == NULL)
    {
        return;
    }
    for (ImDrawList *l : s->lists)
    {
        IM_DELETE(l);
    }
    delete s;
}

static ImGuiKey keyFromSdl(SDL_Keycode k)
{
    switch (k)
    {
        case SDLK_TAB: return ImGuiKey_Tab;
        case SDLK_LEFT: return ImGuiKey_LeftArrow;
        case SDLK_RIGHT: return ImGuiKey_RightArrow;
        case SDLK_UP: return ImGuiKey_UpArrow;
        case SDLK_DOWN: return ImGuiKey_DownArrow;
        case SDLK_PAGEUP: return ImGuiKey_PageUp;
        case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
        case SDLK_HOME: return ImGuiKey_Home;
        case SDLK_END: return ImGuiKey_End;
        case SDLK_INSERT: return ImGuiKey_Insert;
        case SDLK_DELETE: return ImGuiKey_Delete;
        case SDLK_BACKSPACE: return ImGuiKey_Backspace;
        case SDLK_SPACE: return ImGuiKey_Space;
        case SDLK_RETURN: return ImGuiKey_Enter;
        case SDLK_ESCAPE: return ImGuiKey_Escape;
        case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
        case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
        case SDLK_LSHIFT: return ImGuiKey_LeftShift;
        case SDLK_LALT: return ImGuiKey_LeftAlt;
        case SDLK_LGUI: return ImGuiKey_LeftSuper;
        case SDLK_RCTRL: return ImGuiKey_RightCtrl;
        case SDLK_RSHIFT: return ImGuiKey_RightShift;
        case SDLK_RALT: return ImGuiKey_RightAlt;
        case SDLK_RGUI: return ImGuiKey_RightSuper;
        default: break;
    }
    if (k >= SDLK_a && k <= SDLK_z)
    {
        return (ImGuiKey) (ImGuiKey_A + (k - SDLK_a));
    }
    if (k >= SDLK_0 && k <= SDLK_9)
    {
        return (ImGuiKey) (ImGuiKey_0 + (k - SDLK_0));
    }
    return ImGuiKey_None;
}

static ImGuiKey keyFromPad(int b)
{
    switch (b)
    {
        case SDL_CONTROLLER_BUTTON_A: return ImGuiKey_GamepadFaceDown;
        case SDL_CONTROLLER_BUTTON_B: return ImGuiKey_GamepadFaceRight;
        case SDL_CONTROLLER_BUTTON_X: return ImGuiKey_GamepadFaceLeft;
        case SDL_CONTROLLER_BUTTON_Y: return ImGuiKey_GamepadFaceUp;
        case SDL_CONTROLLER_BUTTON_START: return ImGuiKey_GamepadStart;
        case SDL_CONTROLLER_BUTTON_BACK: return ImGuiKey_GamepadBack;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return ImGuiKey_GamepadL1;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return ImGuiKey_GamepadR1;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
        default: return ImGuiKey_None;
    }
}

static void evPush(const ImguiEvent &e)
{
    SDL_LockMutex(s_EvMtx);
    if (s_EvCount < PICK_EVENTS)
    {
        s_Ev[s_EvCount++] = e;
    }
    SDL_UnlockMutex(s_EvMtx);
}

static int SDLCALL eventWatch(void *userdata, SDL_Event *ev)
{
    ImguiEvent e;

    (void) userdata;
    if (!SDL_AtomicGet(&s_Active))
    {
        return 0;
    }
    memset(&e, 0, sizeof(e));
    switch (ev->type)
    {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            e.kind = 0;
            e.key = keyFromSdl(ev->key.keysym.sym);
            e.down = (ev->type == SDL_KEYDOWN);
            if (e.key != ImGuiKey_None)
            {
                evPush(e);
            }
            break;
        case SDL_TEXTINPUT:
            e.kind = 1;
            SDL_strlcpy(e.text, ev->text.text, sizeof(e.text));
            evPush(e);
            break;
        case SDL_MOUSEWHEEL:
            e.kind = 2;
            e.value = (float) ev->wheel.y;
            evPush(e);
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            e.kind = 3;
            e.key = keyFromPad(ev->cbutton.button);
            e.down = (ev->type == SDL_CONTROLLERBUTTONDOWN);
            if (e.key != ImGuiKey_None)
            {
                evPush(e);
            }
            break;
        case SDL_CONTROLLERAXISMOTION:
        {   /* the left stick moves ImGui's nav, as its SDL backend maps it */
            const float v = ev->caxis.value / 32767.0f;
            const float dead = 8000.0f / 32767.0f;
            ImGuiKey neg = ImGuiKey_None, pos = ImGuiKey_None;

            if (ev->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX)
            {
                neg = ImGuiKey_GamepadLStickLeft;
                pos = ImGuiKey_GamepadLStickRight;
            }
            else if (ev->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)
            {
                neg = ImGuiKey_GamepadLStickUp;
                pos = ImGuiKey_GamepadLStickDown;
            }
            if (neg == ImGuiKey_None)
            {
                break;
            }
            e.kind = 4;
            e.key = neg;
            e.value = (v < -dead) ? (-v - dead) / (1.0f - dead) : 0.0f;
            evPush(e);
            e.key = pos;
            e.value = (v > dead) ? (v - dead) / (1.0f - dead) : 0.0f;
            evPush(e);
            break;
        }
        default:
            break;
    }
    return 0;
}

/* sim thread, once: the context, the fonts, the event watch */
static int imguiInit(void)
{
    static int done;

    if (done)
    {
        return done > 0;
    }
    done = -1;
    if (gfx_sdl_get_window() == NULL)
    {
        return 0;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "ge_port";
    ImGui::StyleColorsDark();
    s_BaseStyle = ImGui::GetStyle();
    for (int i = 0; i < PICK_FONTS; i++)
    {
        ImFontConfig cfg;

        cfg.SizePixels = 13.0f * (float) (i + 1);
        s_Fonts[i] = io.Fonts->AddFontDefault(&cfg);
    }
    {   /* built here, on this thread; the GL thread only uploads it */
        unsigned char *px;
        int w, h;

        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    }
    s_EvMtx = SDL_CreateMutex();
    SDL_AddEventWatch(eventWatch, NULL);
    SDL_AtomicSet(&s_GlWanted, 1);
    done = 1;
    portConLogf("ge: imgui: Dear ImGui %s ready (colour picker)\n", IMGUI_VERSION);
    return 1;
}

static void pickClose(int result)
{
    s_Open = 0;
    s_Result = result;
    SDL_AtomicSet(&s_Active, 0);
    if (s_StartedText && !portConIsOpen() && !portTextInputActive())
    {
        SDL_StopTextInput();
    }
    s_StartedText = 0;
}

static void rgbToFloat(int32_t rgb, float out[3])
{
    out[0] = ((rgb >> 16) & 0xFF) / 255.0f;
    out[1] = ((rgb >> 8) & 0xFF) / 255.0f;
    out[2] = (rgb & 0xFF) / 255.0f;
}

static int32_t floatToRgb(const float c[3])
{
    int32_t out = 0;

    for (int i = 0; i < 3; i++)
    {   /* as ImGui's own inputs round (IM_F32_TO_INT8_SAT) */
        float v = c[i] < 0.0f ? 0.0f : (c[i] > 1.0f ? 1.0f : c[i]);

        out = (out << 8) | (int32_t) (v * 255.0f + 0.5f);
    }
    return out;
}

/* Open the picker on rgb (0xRRGGBB, -1: the default), defrgb being the player
 * number's colour and name how chat shows ours. 1: it is up. */
int32_t portColourPickerOpen(int32_t rgb, int32_t defrgb, const char *name)
{
    if (s_Open || SDL_AtomicGet(&s_GlFailed) || !imguiInit())
    {
        return s_Open;
    }
    rgbToFloat((rgb >= 0) ? rgb : defrgb, s_Col);
    memcpy(s_Ref, s_Col, sizeof(s_Ref));
    rgbToFloat(defrgb, s_Def);
    s_UseDefault = (rgb < 0);
    SDL_strlcpy(s_Name, (name != NULL && name[0] != '\0') ? name : "Bond", sizeof(s_Name));
    s_Result = PICK_NONE;
    s_PollMs = SDL_GetTicks();
    s_PopupOpened = 0;
    s_Busy = false;
    s_MouseHeld = 1;
    s_Open = 1;
    SDL_AtomicSet(&s_Active, 1);
    if (!SDL_IsTextInputActive())
    {   /* the hex and number fields take typed text */
        SDL_StartTextInput();
        s_StartedText = 1;
    }
    SDL_LockMutex(s_EvMtx);
    s_EvCount = 0;
    SDL_UnlockMutex(s_EvMtx);
    portConLogf("ge: imgui: colour picker open (%06X)\n", (unsigned) floatToRgb(s_Col));
    return 1;
}

/* The lobby asks every tick: 0 still up (or never opened), PICK_OK with *rgb
 * (-1: back to the default), PICK_CANCEL. */
int32_t portColourPickerPoll(int32_t *rgb)
{
    int r = s_Result;

    s_PollMs = SDL_GetTicks();
    s_Result = PICK_NONE;
    if (r == PICK_OK && rgb != NULL)
    {
        *rgb = s_ResultRgb;
    }
    return r;
}

int32_t portColourPickerActive(void)
{
    return SDL_AtomicGet(&s_Active);
}

static void imguiInput(ImGuiIO &io)
{
    SDL_Window *w = gfx_sdl_get_window();
    int ww = 1, wh = 1, dw = 1, dh = 1, mx = 0, my = 0;
    Uint32 mb;

    SDL_GetWindowSize(w, &ww, &wh);
    SDL_GL_GetDrawableSize(w, &dw, &dh);
    ww = (ww > 0) ? ww : 1;
    wh = (wh > 0) ? wh : 1;
    io.DisplaySize = ImVec2((float) dw, (float) dh);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    {
        const Uint64 now = SDL_GetPerformanceCounter();
        const double dt = (s_LastCounter != 0) ? (double) (now - s_LastCounter) / (double) SDL_GetPerformanceFrequency() : 1.0 / 60.0;

        io.DeltaTime = (dt > 0.0 && dt < 0.25) ? (float) dt : 1.0f / 60.0f;
        s_LastCounter = now;
    }

    /* the pointer: the OS's while the menus have it free, else the game's own
     * virtual one (input_pc.c) */
    if (SDL_GetRelativeMouseMode())
    {
        int32_t vx = 0, vy = 0;

        portInputMouseWindowPos(&vx, &vy, NULL);
        mx = vx;
        my = vy;
        mb = SDL_GetMouseState(NULL, NULL);
    }
    else
    {
        mb = SDL_GetMouseState(&mx, &my);
    }
    if (s_MouseHeld)
    {   /* the lobby's click on the Colour row must not land in the picker */
        s_MouseHeld = (mb & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK | SDL_BUTTON_MMASK)) != 0;
        mb = 0;
    }
    if (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS)
    {
        io.AddMousePosEvent(mx * (float) dw / (float) ww, my * (float) dh / (float) wh);
    }
    io.AddMouseButtonEvent(0, (mb & SDL_BUTTON_LMASK) != 0);
    io.AddMouseButtonEvent(1, (mb & SDL_BUTTON_RMASK) != 0);
    io.AddMouseButtonEvent(2, (mb & SDL_BUTTON_MMASK) != 0);
    /* the game hides the OS arrow while its mouse option is on: draw ImGui's */
    io.MouseDrawCursor = (SDL_ShowCursor(SDL_QUERY) == SDL_DISABLE);

    {
        const SDL_Keymod mod = SDL_GetModState();

        io.AddKeyEvent(ImGuiMod_Ctrl, (mod & KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (mod & KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (mod & KMOD_ALT) != 0);
        io.AddKeyEvent(ImGuiMod_Super, (mod & KMOD_GUI) != 0);
    }
    SDL_LockMutex(s_EvMtx);
    for (int i = 0; i < s_EvCount; i++)
    {
        const ImguiEvent &e = s_Ev[i];

        switch (e.kind)
        {
            case 0:
            case 3:
                io.AddKeyEvent((ImGuiKey) e.key, e.down != 0);
                break;
            case 1:
                io.AddInputCharactersUTF8(e.text);
                break;
            case 2:
                io.AddMouseWheelEvent(0.0f, e.value);
                break;
            case 4:
                io.AddKeyAnalogEvent((ImGuiKey) e.key, e.value > 0.1f, e.value);
                break;
        }
    }
    s_EvCount = 0;
    SDL_UnlockMutex(s_EvMtx);
}

/* the picker's window: ImGui's ColorPicker4 as it comes, with OK, Cancel and
 * Default Colour under it */
static void pickerUi(void)
{
    const float k = (float) s_Scale;
    const char *title = portTrUtf8("Player Colour");
    char id[96];
    bool open = true;
    int done = PICK_NONE;

    snprintf(id, sizeof(id), "%s###coopcolour", title);
    if (!s_PopupOpened)
    {
        ImGui::OpenPopup("###coopcolour");
        s_PopupOpened = 1;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(id, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        pickClose(PICK_CANCEL);
        return;
    }
    {
        /* last frame's: Escape that ends an edit or shuts a right-click menu
         * has already done so by now, and must not cancel the picker too */
        const bool busy = s_Busy;
        float before[3];

        memcpy(before, s_Col, sizeof(before));
        ImGui::ColorPicker4("##picker", s_Col, ImGuiColorEditFlags_NoAlpha, s_Ref);
        if (memcmp(before, s_Col, sizeof(before)) != 0)
        {
            s_UseDefault = 0;
        }

        ImGui::Spacing();
        ImGui::TextUnformatted(portTrUtf8("In chat:"));
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(s_Col[0], s_Col[1], s_Col[2], 1.0f), "%s", s_Name);
        if (portAccessColorblind() != 0)
        {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 16.0f * ImGui::GetFontSize());
            ImGui::TextDisabled("%s", portTrUtf8("Colour Blind is on, so you see its colours instead."));
            ImGui::PopTextWrapPos();
        }
        ImGui::Separator();
        if (ImGui::Button(portTrUtf8("OK"), ImVec2(80.0f * k, 0.0f)))
        {
            done = PICK_OK;
        }
        ImGui::SameLine();
        if (ImGui::Button(portTrUtf8("Cancel"), ImVec2(80.0f * k, 0.0f)))
        {
            done = PICK_CANCEL;
        }
        ImGui::SameLine();
        if (ImGui::Button(portTrUtf8("Default Colour")))
        {
            memcpy(s_Col, s_Def, sizeof(s_Col));
            s_UseDefault = 1;
        }
        if (!busy && done == PICK_NONE
            && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
        {   /* Escape, or B, with nothing being edited: Cancel */
            done = PICK_CANCEL;
        }
        if (!open)
        {
            done = PICK_CANCEL;
        }
        if (done != PICK_NONE)
        {
            ImGui::CloseCurrentPopup();
        }
        /* an edit going on, or a menu of ours open over the picker */
        s_Busy = ImGui::IsAnyItemActive() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        ImGui::EndPopup();
    }
    if (done == PICK_OK)
    {
        s_ResultRgb = s_UseDefault ? -1 : floatToRgb(s_Col);
        portConLogf("ge: imgui: colour picked: %s%06X\n", s_UseDefault ? "default, " : "", (unsigned) floatToRgb(s_Col));
    }
    else if (done == PICK_CANCEL)
    {
        portConLogf("ge: imgui: colour picker cancelled\n");
    }
    if (done != PICK_NONE)
    {
        pickClose(done);
    }
}

static void publish(ImguiSnap *snap)
{
    ImguiSnap *old;

    SDL_AtomicLock(&s_SnapLock);
    old = s_SnapPending;
    s_SnapPending = snap;
    SDL_AtomicUnlock(&s_SnapLock);
    snapFree(old);
}

/* sim thread, once per frame handed to the renderer */
void portImguiFrame(void)
{
    static int shown;   /* the last frame published had the picker */

    if (!s_Open)
    {
        if (shown)
        {   /* an empty frame: the GL thread stops drawing it */
            publish(new ImguiSnap());
            shown = 0;
        }
        return;
    }
    if (SDL_GetTicks() - s_PollMs > PICK_POLL_MS)
    {   /* the lobby went away under it (a mission started): drop the pick */
        portConLogf("ge: imgui: colour picker closed, the lobby is gone\n");
        pickClose(PICK_CANCEL);
        return;
    }
    if (SDL_AtomicGet(&s_GlFailed))
    {   /* never drawn, so it must not hold the game's input */
        pickClose(PICK_CANCEL);
        return;
    }
    if (!SDL_AtomicGet(&s_GlReady))
    {
        return;     /* the GL thread sets the backend up first */
    }

    ImGuiIO &io = ImGui::GetIO();

    imguiInput(io);
    {   /* ImGui's look at a whole multiple of its 13 px font, about 720 lines a step */
        int scale = (int) floorf(io.DisplaySize.y / 720.0f + 0.5f);

        scale = (scale < 1) ? 1 : (scale > PICK_FONTS) ? PICK_FONTS : scale;
        if (scale != s_Scale)
        {
            ImGui::GetStyle() = s_BaseStyle;
            ImGui::GetStyle().ScaleAllSizes((float) scale);
            s_Scale = scale;
        }
        io.FontDefault = s_Fonts[scale - 1];
    }
    ImGui::NewFrame();
    pickerUi();
    ImGui::Render();

    ImDrawData *dd = ImGui::GetDrawData();
    ImguiSnap *snap = new ImguiSnap();

    snap->pos = dd->DisplayPos;
    snap->size = dd->DisplaySize;
    snap->scale = dd->FramebufferScale;
    for (int i = 0; s_Open && i < dd->CmdListsCount; i++)
    {
        snap->lists.push_back(dd->CmdLists[i]->CloneOutput());
    }
    publish(snap);
    shown = 1;
}

/* the GL thread, after the game's frame and before the swap (gfx_pc.cpp) */
void portImguiRender(void)
{
    static int glstate;     /* 0 not yet, 1 up, -1 failed */
    ImguiSnap *snap;

    if (glstate == 0 && SDL_AtomicGet(&s_GlWanted))
    {
        if (ImGui_ImplOpenGL3_Init(NULL))
        {
            ImGui_ImplOpenGL3_NewFrame();   /* the shaders and the font texture */
            glstate = 1;
            SDL_AtomicSet(&s_GlReady, 1);
        }
        else
        {
            glstate = -1;
            SDL_AtomicSet(&s_GlFailed, 1);
            portConLogf("ge: imgui: the GL backend failed to start; no colour picker\n");
        }
    }
    SDL_AtomicLock(&s_SnapLock);
    snap = s_SnapPending;
    s_SnapPending = NULL;
    SDL_AtomicUnlock(&s_SnapLock);
    if (snap != NULL)
    {
        snapFree(s_SnapShown);
        s_SnapShown = snap;
    }
    if (glstate != 1 || s_SnapShown == NULL || s_SnapShown->lists.empty())
    {
        return;
    }

    ImDrawData dd;

    dd.Valid = true;
    dd.DisplayPos = s_SnapShown->pos;
    dd.DisplaySize = s_SnapShown->size;
    dd.FramebufferScale = s_SnapShown->scale;
    for (ImDrawList *l : s_SnapShown->lists)
    {
        dd.CmdLists.push_back(l);
        dd.TotalVtxCount += l->VtxBuffer.Size;
        dd.TotalIdxCount += l->IdxBuffer.Size;
    }
    dd.CmdListsCount = dd.CmdLists.Size;
    ImGui_ImplOpenGL3_RenderDrawData(&dd);
}
