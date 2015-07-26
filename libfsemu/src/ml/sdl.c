#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef USE_SDL_VIDEO

// FIXME: make libfsml independent of libfsemu
#include "../emu/video.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#ifdef USE_SDL2
#define USE_SDL
#endif

#ifdef USE_SDL
#include <SDL.h>
#endif

//#ifdef USE_GLIB
//#include <glib.h>
//#endif

#include <fs/conf.h>
#include <GLee.h>
#include <fs/glib.h>
#include <fs/ml.h>
#include <fs/thread.h>

#ifdef USE_OPENGL
#include <fs/ml/opengl.h>
#endif

//#include "fs/emu.h"
#define FS_EMU_INTERNAL
#include <fs/emu/input.h>
#include <fs/emu/monitor.h>

#include "ml_internal.h"

static GQueue *g_video_event_queue = NULL;
static fs_mutex *g_video_event_mutex = NULL;

static fs_condition *g_video_cond = NULL;
static fs_mutex *g_video_mutex = NULL;

#ifdef USE_SDL2
SDL_Window *g_fs_ml_window = NULL;
SDL_GLContext g_fs_ml_context = 0;
static int g_display;
#else
static SDL_Surface *g_sdl_screen = NULL;
#endif

// current size of window or fullscreen view
//static int g_video_width = 0;
//static int g_video_height = 0;
static int g_has_input_grab = 0;
static int g_initial_input_grab = 0;
static int g_fs_ml_automatic_input_grab = 1;
static int g_fs_ml_keyboard_input_grab = 1;
static int g_fsaa = 0;

static int g_debug_input = 0;
static int g_f12_state, g_f11_state;

static char *g_window_title;
static int g_window_width, g_window_height;
#ifdef USE_SDL2
static int g_window_x, g_window_y;
#endif
static int g_window_resizable;
static int g_fullscreen_width, g_fullscreen_height;

static GLint g_max_texture_size;

int g_fs_ml_had_input_grab = 0;
int g_fs_ml_was_fullscreen = 0;

#define FS_ML_VIDEO_EVENT_GRAB_INPUT 1
#define FS_ML_VIDEO_EVENT_UNGRAB_INPUT 2
#define FS_ML_VIDEO_EVENT_SHOW_CURSOR 3
#define FS_ML_VIDEO_EVENT_HIDE_CURSOR 4

#define FULLSCREEN_FULLSCREEN 0
#define FULLSCREEN_WINDOW 1
#define FULLSCREEN_DESKTOP 2

int fs_ml_get_max_texture_size()
{
    return g_max_texture_size;
}

int fs_ml_get_fullscreen_width()
{
    return g_fullscreen_width;
}

int fs_ml_get_fullscreen_height()
{
    return g_fullscreen_height;
}

int fs_ml_get_windowed_width()
{
    return g_window_width;
}

int fs_ml_get_windowed_height()
{
    return g_window_height;
}

static void post_video_event(int event)
{
#ifdef FS_EMU_DRIVERS
    // printf("FS_EMU_DRIVERS: ignoring post_video_event\n");
#else
    fs_mutex_lock(g_video_event_mutex);
    g_queue_push_head(g_video_event_queue, FS_INT_TO_POINTER(event));
    fs_mutex_unlock(g_video_event_mutex);
#endif
}

static void process_video_events()
{
    fs_mutex_lock(g_video_event_mutex);
    int count = g_queue_get_length(g_video_event_queue);
    for (int i = 0; i < count; i++) {
        int event = FS_POINTER_TO_INT(g_queue_pop_tail(g_video_event_queue));
        if (event == FS_ML_VIDEO_EVENT_GRAB_INPUT) {
            fs_ml_grab_input(1, 1);
        }
        else if (event == FS_ML_VIDEO_EVENT_UNGRAB_INPUT) {
            fs_ml_grab_input(0, 1);
        }
        else if (event == FS_ML_VIDEO_EVENT_SHOW_CURSOR) {
            fs_ml_show_cursor(1, 1);
        }
        else if (event == FS_ML_VIDEO_EVENT_HIDE_CURSOR) {
            fs_ml_show_cursor(0, 1);
        }
    }
    fs_mutex_unlock(g_video_event_mutex);
}

int fs_ml_has_input_grab()
{
    //printf("has input grab? %d\n", g_grab_input);
    return g_has_input_grab;
}

int fs_ml_has_automatic_input_grab()
{
    return g_fs_ml_automatic_input_grab;
}

void fs_ml_grab_input(int grab, int immediate)
{
    //printf("fs_ml_grab_input %d %d\n", grab, immediate);
    if (immediate) {
#ifdef USE_SDL2
        SDL_SetWindowGrab(g_fs_ml_window, grab ? SDL_TRUE : SDL_FALSE);
        SDL_SetRelativeMouseMode(grab ? SDL_TRUE : SDL_FALSE);
#else
        SDL_WM_GrabInput(grab ? SDL_GRAB_ON : SDL_GRAB_OFF);
#endif
        if (fs_ml_cursor_allowed()) {
            fs_ml_show_cursor(!grab, 1);
        }
    }
    else {
        post_video_event(grab ? FS_ML_VIDEO_EVENT_GRAB_INPUT :
                FS_ML_VIDEO_EVENT_UNGRAB_INPUT);
    }
    g_has_input_grab = grab ? 1 : 0;
}

void fs_ml_set_video_fsaa(int fsaa)
{
    g_fsaa = fsaa;
}

void fs_ml_show_cursor(int show, int immediate)
{
    if (immediate) {
        SDL_ShowCursor(show);
    }
    else {
        post_video_event(show ? FS_ML_VIDEO_EVENT_SHOW_CURSOR :
                FS_ML_VIDEO_EVENT_HIDE_CURSOR);
    }
}

static void log_opengl_information()
{
    static int written = 0;
    if (written) {
        return;
    }
    written = 1;
    char *software_renderer = NULL;
    const char *s;
    s = (const char*) glGetString(GL_VENDOR);
    if (s) {
        fs_log("opengl vendor: %s\n", s);
    }
    s = (const char*) glGetString(GL_RENDERER);
    if (s) {
        fs_log("opengl renderer: %s\n", s);
        if (strstr(s, "GDI Generic") != NULL) {
            software_renderer = g_strdup(s);
        }
    }
    s = (const char*) glGetString(GL_VERSION);
    if (s) {
        fs_log("opengl version: %s\n", s);
    }
    s = (const char*) glGetString(GL_SHADING_LANGUAGE_VERSION);
    if (s) {
        fs_log("opengl shading language version: %s\n", s);
    }
    s = (const char*) glGetString(GL_EXTENSIONS);
    if (s) {
        fs_log("opengl extensions: %s\n", s);
    }
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g_max_texture_size);
    fs_log("opengl max texture size (estimate): %dx%d\n", g_max_texture_size,
            g_max_texture_size);

    if (software_renderer) {
        fs_emu_warning("No HW OpenGL driver (\"%s\")",
                software_renderer);
        g_free(software_renderer);
    }
}

static void set_video_mode()
{
#ifdef USE_SDL2
    int flags = SDL_WINDOW_OPENGL;
    if (g_fs_emu_video_fullscreen_mode != FULLSCREEN_WINDOW &&
            g_window_resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    int x = g_window_x, y = g_window_y;
    int w = -1, h = -1;

//    if (g_initial_input_grab) {
//        flags |= SDL_WINDOW_INPUT_GRABBED;
//        g_has_input_grab = 1;
//    }

    if (g_fs_emu_video_fullscreen == 1) {
        w = g_fullscreen_width;
        h = g_fullscreen_height;
        //w = g_window_width;
        //h = g_window_height;

        if (g_fs_emu_video_fullscreen_mode == FULLSCREEN_WINDOW) {
            fs_log("using fullscreen window mode\n");
            //x = 0;
            //y = 0;
            //w = g_fullscreen_width;
            //h = g_fullscreen_height;
            flags |= SDL_WINDOW_BORDERLESS;

            FSEmuMonitor monitor;
            fs_emu_monitor_get_by_index(g_display, &monitor);
            x = monitor.rect.x;
            y = monitor.rect.y;
            w = monitor.rect.w;
            h = monitor.rect.h;
        }
        else if (g_fs_emu_video_fullscreen_mode == FULLSCREEN_DESKTOP) {
            fs_log("using fullscreen desktop mode\n");
            // the width and height will not be used for the fullscreen
            // desktop mode, only for the window when toggling fullscreen
            // state
#if 0
            w = g_window_width;
            h = g_window_height;
#else
            FSEmuMonitor monitor;
            fs_emu_monitor_get_by_index(g_display, &monitor);
            x = monitor.rect.x;
            y = monitor.rect.y;
            w = monitor.rect.w;
            h = monitor.rect.h;
#endif
            flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        }
        else {
            fs_log("using SDL_FULLSCREEN mode\n");
            flags |= SDL_WINDOW_FULLSCREEN;
        }
        fs_log("setting (fullscreen) video mode %d %d\n", w, h);
    }
    else {
        w = g_window_width;
        h = g_window_height;

        fs_log("using windowed mode\n");
        //SDL_putenv("SDL_VIDEO_WINDOW_POS=");
        fs_log("setting (windowed) video mode %d %d\n", w, h);
    }

    if (fs_config_get_boolean("window_border") == 0) {
        fs_log("borderless window requested\n");
        flags |= SDL_WINDOW_BORDERLESS;
    }

#if 0
    Uint8 data[] = "\0";
    SDL_Cursor *cursor = SDL_CreateCursor(data, data, 8, 1, 0, 0);
    SDL_SetCursor(cursor);
#endif

    g_fs_ml_video_width = w;
    g_fs_ml_video_height = h;
    fs_log("SDL_CreateWindow(x=%d, y=%d, w=%d, h=%d, flags=%d)\n",
           x, y, w, h, flags);
    g_fs_ml_window = SDL_CreateWindow(g_window_title, x, y, w, h, flags);
    g_fs_ml_context = SDL_GL_CreateContext(g_fs_ml_window);

#else
    int flags = SDL_DOUBLEBUF | SDL_OPENGL;
    if (g_fs_emu_video_fullscreen == 1) {
        g_fs_ml_video_width = g_fullscreen_width;
        g_fs_ml_video_height = g_fullscreen_height;
        if (g_fs_emu_video_fullscreen_mode == FULLSCREEN_WINDOW) {
            fs_log("using fullscreen window mode\n");
            SDL_putenv("SDL_VIDEO_WINDOW_POS=0,0");
            flags |= SDL_NOFRAME;
            //fs_ml_set_fullscreen_extra();
        }
        else {
            fs_log("using SDL_FULLSCREEN mode\n");
            flags |= SDL_FULLSCREEN;
        }
        fs_log("setting (fullscreen) video mode %d %d\n", g_fs_ml_video_width,
                g_fs_ml_video_height);
        g_sdl_screen = SDL_SetVideoMode(g_fs_ml_video_width,
                g_fs_ml_video_height, 0, flags);
        //update_viewport();
    }
    else {
        fs_log("using windowed mode\n");
        //SDL_putenv("SDL_VIDEO_WINDOW_POS=");
        g_fs_ml_video_width = g_window_width;
        g_fs_ml_video_height = g_window_height;
        if (g_window_resizable) {
            flags |= SDL_RESIZABLE;
        }
        fs_log("setting (windowed) video mode %d %d\n", g_fs_ml_video_width,
                g_fs_ml_video_height);
        g_sdl_screen = SDL_SetVideoMode(g_fs_ml_video_width,
                g_fs_ml_video_height, 0, flags);
        //update_viewport();
    }

#endif

    fs_ml_configure_window();

    // FIXME: this can be removed
    g_fs_ml_opengl_context_stamp++;

    log_opengl_information();
}

#ifdef USE_SDL2
// When using SDL 2, the OpenGL context does not need to be recreated when
// resizing the window and/or toggling fullscreen.
#else

static void destroy_opengl_state()
{
    fs_log("destroy_opengl_state\n");
    fs_gl_send_context_notification(FS_GL_CONTEXT_DESTROY);
}

static void recreate_opengl_state()
{
    fs_log("recreate_opengl_state\n");
    fs_gl_reset_client_state();
    fs_gl_send_context_notification(FS_GL_CONTEXT_CREATE);
}

#endif

void fs_ml_toggle_fullscreen()
{
    fs_log("fs_ml_toggle_fullscreen\n");
#ifdef USE_SDL2
    if (g_fs_emu_video_fullscreen_mode == FULLSCREEN_WINDOW) {
        fs_emu_warning("Cannot toggle fullscreen with fullscreen-mode=window");
        return;
    }

    int display_index = 0;
    SDL_DisplayMode mode;
    memset(&mode, 0, sizeof(SDL_DisplayMode));
    if (SDL_GetDesktopDisplayMode(display_index, &mode) == 0) {
        SDL_SetWindowDisplayMode(g_fs_ml_window, &mode);
    }

    g_fs_emu_video_fullscreen = !g_fs_emu_video_fullscreen;
    int flags = 0;
    if (g_fs_emu_video_fullscreen) {
        if (g_fs_emu_video_fullscreen_mode == FULLSCREEN_DESKTOP) {
            flags = SDL_WINDOW_FULLSCREEN_DESKTOP;
        }
        else {
            flags = SDL_WINDOW_FULLSCREEN;
        }
    }
    SDL_SetWindowFullscreen(g_fs_ml_window, flags);

#else
    g_fs_emu_video_fullscreen = !g_fs_emu_video_fullscreen;
    destroy_opengl_state();
    set_video_mode();
    recreate_opengl_state();
#endif
}

static int g_fs_emu_monitor_count;
// static FSEmuMonitor g_fs_emu_monitors[FS_EMU_MONITOR_MAX_COUNT];
static GArray *g_fs_emu_monitors;

static gint fs_emu_monitor_compare(gconstpointer a, gconstpointer b)
{
    FSEmuMonitor *am = (FSEmuMonitor *) a;
    FSEmuMonitor *bm = (FSEmuMonitor *) b;

    return am->rect.x - bm->rect.x;
}

static void fs_emu_monitor_init()
{
    static bool initialized = false;
    if (initialized) {
        return;
    }
    initialized = true;

    g_fs_emu_monitors = g_array_new(false, true, sizeof(FSEmuMonitor));

    int display_index = 0;
    SDL_DisplayMode mode;
    int error = SDL_GetCurrentDisplayMode(display_index, &mode);
    if (error) {
        fs_log("SDL_GetCurrentDisplayMode failed\n");
        SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_ERROR, "Display Error",
            "SDL_GetCurrentDisplayMode failed.", NULL);
        exit(1);
    }
    g_fs_emu_monitor_count = SDL_GetNumVideoDisplays();

    if (g_fs_emu_monitor_count < 1) {
        fs_log("Error %d retrieving number of displays/monitors\n",
               g_fs_emu_monitor_count);
        g_fs_emu_monitor_count = 1;
    }
    if (g_fs_emu_monitor_count >  FS_EMU_MONITOR_MAX_COUNT) {
        fs_log("Limiting number of displays to %d\n",
                FS_EMU_MONITOR_MAX_COUNT);
        g_fs_emu_monitor_count =  FS_EMU_MONITOR_MAX_COUNT;
    }

    for (int i = 0; i < g_fs_emu_monitor_count; i++) {
        SDL_Rect rect;
        FSEmuMonitor monitor;
        int error = SDL_GetDisplayBounds(i, &rect);
        if (error) {
            fs_log("Error retrieving display bounds for display %d: %s\n",
                   i, SDL_GetError());
            /* Setting dummy values on error*/
            rect.x = 0;
            rect.y = 0;
            rect.w = 1024;
            rect.h = 768;
        }

        monitor.rect.x = rect.x;
        monitor.rect.y = rect.y;
        monitor.rect.w = rect.w;
        monitor.rect.h = rect.h;
        monitor.index = i;

        g_array_append_val(g_fs_emu_monitors, monitor);
    }

    g_array_sort(g_fs_emu_monitors, fs_emu_monitor_compare);
    for (int i = 0; i < g_fs_emu_monitor_count; i++) {
        g_array_index(g_fs_emu_monitors, FSEmuMonitor, i).index = i;
        /* Set physical position flags (left, m-left, m-right, right) */
        int flags = 0;
        for (int j = 0; j < 4; j++) {
            int pos = (g_fs_emu_monitor_count - 1.0) * j / 3.0 + 0.5;
            fs_log("Monitor - j %d pos %d\n", j, pos);
            if (pos == i) {
                flags |= (1 << j);
            }
        }
        fs_log("Monitor index %d flags %d\n", i, flags);
        g_array_index(g_fs_emu_monitors, FSEmuMonitor, i).flags = flags;
    }
}

int fs_emu_monitor_count()
{
    return g_fs_emu_monitor_count;
}

bool fs_emu_monitor_get_by_index(int index, FSEmuMonitor* monitor)
{
    if (index < 0 || index >= g_fs_emu_monitor_count) {
        monitor->index = -1;
        monitor->flags = 0;
        monitor->rect.x = 0;
        monitor->rect.y = 0;
        monitor->rect.w = 1024;
        monitor->rect.h = 768;
        return false;
    }
    SDL_assert(monitor != NULL);
    memcpy(monitor, &g_array_index(g_fs_emu_monitors, FSEmuMonitor, index),
           sizeof(FSEmuMonitor));
    return true;
}

bool fs_emu_monitor_get_by_flag(int flag, FSEmuMonitor* monitor)
{
    for (int i = 0; i < g_fs_emu_monitor_count; i++) {
        if ((g_array_index(g_fs_emu_monitors,
                          FSEmuMonitor, i).flags & flag) == flag) {
            fs_log("Monitor: found index %d for flag %d\n", i, flag);
            return fs_emu_monitor_get_by_index(i, monitor);
        }
    }
    fs_emu_monitor_get_by_index(0, monitor);
    return false;
}

int fs_ml_video_create_window(const char *title)
{
    fs_log("fs_ml_video_create_window\n");
    g_window_title = g_strdup(title);

    g_fs_ml_keyboard_input_grab = fs_config_get_boolean(
            "keyboard_input_grab");
    if (g_fs_ml_automatic_input_grab == FS_CONFIG_NONE) {
        g_fs_ml_keyboard_input_grab = 1;
    }
    fs_log("keyboard input grab: %d\n", g_fs_ml_keyboard_input_grab);

    static int initialized = 0;
#ifdef USE_SDL2
   SDL_SetHint(SDL_HINT_GRAB_KEYBOARD,
               g_fs_ml_keyboard_input_grab ? "1" : "0");

   SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

#endif
    SDL_Init(SDL_INIT_VIDEO);

    SDL_version version;
    SDL_VERSION(&version);
    fs_log("FS-UAE was compiled for SDL %d.%d.%d\n",
           version.major, version.minor, version.patch);

    if (!initialized) {
#ifdef USE_SDL2

        int display_index = 0;
        SDL_DisplayMode mode;
        int error = SDL_GetCurrentDisplayMode(display_index, &mode);
        if (error) {
            fs_log("SDL_GetCurrentDisplayMode failed\n");
            SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_ERROR, "Display Error",
                "SDL_GetCurrentDisplayMode failed.", NULL);
            exit(1);
        }

        fs_emu_monitor_init();

        const char *mon = fs_config_get_const_string("monitor");
        int mon_flag = -1;
        if (mon == NULL) {
            mon = "middle-left";
        }
        if (strcmp(mon, "left") == 0) {
            mon_flag = FS_EMU_MONITOR_FLAG_LEFT;
        } else if (strcmp(mon, "middle-left") == 0) {
            mon_flag = FS_EMU_MONITOR_FLAG_MIDDLE_LEFT;
        } else if (strcmp(mon, "middle-right") == 0) {
            mon_flag = FS_EMU_MONITOR_FLAG_MIDDLE_RIGHT;
        } else if (strcmp(mon, "right") == 0) {
            mon_flag = FS_EMU_MONITOR_FLAG_RIGHT;
        }
        else {
            mon_flag = FS_EMU_MONITOR_FLAG_MIDDLE_LEFT;
        }
        FSEmuMonitor monitor;
        fs_emu_monitor_get_by_flag(mon_flag, &monitor);
        fs_log("Monitor \"%s\" (flag %d) => index %d\n",
               mon, mon_flag, monitor.index);
        g_display = monitor.index;

#else
        const SDL_VideoInfo* info = SDL_GetVideoInfo();

#endif
        g_fullscreen_width = fs_config_get_int("fullscreen_width");
        if (g_fullscreen_width == FS_CONFIG_NONE) {
#ifdef USE_SDL2
            g_fullscreen_width = mode.w;
#else
            g_fullscreen_width = info->current_w;
#endif
        }
        g_fullscreen_height = fs_config_get_int("fullscreen_height");
        if (g_fullscreen_height == FS_CONFIG_NONE) {
#ifdef USE_SDL2
            g_fullscreen_height = mode.h;
#else
            g_fullscreen_height = info->current_h;
#endif
        }

        if (g_fs_emu_video_fullscreen_mode_string == NULL) {
            g_fs_emu_video_fullscreen_mode = -1;
        }
        else if (g_ascii_strcasecmp(g_fs_emu_video_fullscreen_mode_string,
                "window") == 0) {
            g_fs_emu_video_fullscreen_mode = FULLSCREEN_WINDOW;
        }
        else if (g_ascii_strcasecmp(g_fs_emu_video_fullscreen_mode_string,
                "fullscreen") == 0) {
            g_fs_emu_video_fullscreen_mode = FULLSCREEN_FULLSCREEN;
        }
#ifdef USE_SDL2
        else if (g_ascii_strcasecmp(g_fs_emu_video_fullscreen_mode_string,
                "desktop") == 0) {
            g_fs_emu_video_fullscreen_mode = FULLSCREEN_DESKTOP;
        }
#endif
        if (g_fs_emu_video_fullscreen_mode == -1) {
#ifdef MACOSX
            g_fs_emu_video_fullscreen_mode = FULLSCREEN_FULLSCREEN;
#else
            g_fs_emu_video_fullscreen_mode = FULLSCREEN_FULLSCREEN;
#endif
#ifdef USE_SDL2
            fs_log("defaulting to fullscreen_mode = desktop for SDL2\n");
            g_fs_emu_video_fullscreen_mode = FULLSCREEN_DESKTOP;
#endif
        }
        initialized = 1;
    }

    if (g_fs_ml_video_sync) {
        g_fs_ml_vblank_sync = 1;
    }

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
#ifdef USE_SDL2
    // setting swap interval after creating OpenGL context
#else
    if (g_fs_ml_vblank_sync) {
        fs_emu_log("*** Setting swap interval to 1 ***\n");
        SDL_GL_SetAttribute(SDL_GL_SWAP_CONTROL, 1);
    }
    else {
        fs_emu_log("*** Setting swap interval to 0 ***\n");
        SDL_GL_SetAttribute(SDL_GL_SWAP_CONTROL, 0);
    }
#endif

    if (g_fsaa) {
        fs_log("setting FSAA samples to %d\n", g_fsaa);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, g_fsaa);
    }

    g_window_width = fs_config_get_int("window_width");
    if (g_window_width == FS_CONFIG_NONE) {
        g_window_width = 1920 / 2;
    }
    g_window_height = fs_config_get_int("window_height");
    if (g_window_height == FS_CONFIG_NONE) {
        g_window_height = 1080/ 2;
    }
#ifdef USE_SDL2
    g_window_x = fs_config_get_int("window_x");
    if (g_window_x == FS_CONFIG_NONE) {
        g_window_x = SDL_WINDOWPOS_CENTERED;
    }
    g_window_y = fs_config_get_int("window_y");
    if (g_window_y == FS_CONFIG_NONE) {
        g_window_y = SDL_WINDOWPOS_CENTERED;
    }
#endif
    g_window_resizable = fs_config_get_boolean("window_resizable");
    if (g_window_resizable == FS_CONFIG_NONE) {
        g_window_resizable = 1;
    }

    g_fs_ml_automatic_input_grab = fs_config_get_boolean(
            "automatic_input_grab");
    if (g_fs_ml_automatic_input_grab == FS_CONFIG_NONE) {
        if (fs_ml_mouse_integration()) {
            g_fs_ml_automatic_input_grab = 0;
        } else {
            g_fs_ml_automatic_input_grab = 1;
        }
    }
    fs_log("automatic input grab: %d\n", g_fs_ml_automatic_input_grab);

    g_initial_input_grab = g_fs_ml_automatic_input_grab;
    if (fs_config_get_boolean("initial_input_grab") == 1) {
        g_initial_input_grab = 1;
    }
    else if (fs_config_get_boolean("initial_input_grab") == 0 ||
            // deprecated names:
            fs_config_get_boolean("input_grab") == 0 ||
            fs_config_get_boolean("grab_input") == 0) {
        g_initial_input_grab = 0;
    }

    set_video_mode();

#ifdef USE_SDL2
    if (g_fs_ml_vblank_sync) {
        fs_emu_log("*** Setting swap interval to 1 ***\n");
        if (SDL_GL_SetSwapInterval(1) != 0) {
            fs_emu_warning("SDL_GL_SetSwapInterval(1) failed");
        }
    }
    else {
        fs_emu_log("*** Setting swap interval to 0 ***\n");
        SDL_GL_SetSwapInterval(0);
    }
#endif

    // we display a black frame as soon as possible (to reduce flickering on
    // startup)
    glClear(GL_COLOR_BUFFER_BIT);
#ifdef USE_SDL2
    SDL_GL_SwapWindow(g_fs_ml_window);
#else
    SDL_GL_SwapBuffers();
#endif
    fs_gl_finish();

#ifdef USE_SDL2
    // set in SDL_CreateWindow instead
#else
    SDL_WM_SetCaption(g_window_title, fs_get_application_name());
#endif

    fs_log("initial input grab: %d\n", g_initial_input_grab);
    if (g_initial_input_grab && !g_has_input_grab) {
        fs_ml_grab_input(1, 1);
    }
    fs_ml_show_cursor(0, 1);

    // this function must be called from the video thread
    fs_log("init_opengl\n");
    fs_emu_video_init_opengl();

#ifdef WINDOWS
#ifdef USE_SDL2
    // we use only SDL functions with SDL2
#else
    fs_ml_init_raw_input();
#endif
#else
#ifdef USE_SDL2
    SDL_StartTextInput();
#else
    // enable keysym to unicode char translation
    SDL_EnableUNICODE(1);
#endif
#endif
    fs_log("create windows is done\n");
    return 1;
}

int g_fs_ml_running = 1;

#ifndef WINDOWS
// using separate implementation on Windows with raw input
void fs_ml_clear_keyboard_modifier_state()
{

}
#endif

#ifdef USE_SDL2
#include "sdl2_keys.h"
// modifiers have values in SDL and SDL2, except META is renamed to GUI
#define KMOD_LMETA KMOD_LGUI
#define KMOD_RMETA KMOD_RGUI
#define KMOD_META (KMOD_LMETA|KMOD_RMETA)
#endif

static void on_resize(int width, int height)
{
    if (width == g_fs_ml_video_width && height == g_fs_ml_video_height) {
        fs_log("got resize event, but size was unchanged\n");
        return;
    }
    if (g_fs_emu_video_fullscreen) {
        fs_log("not updating window size in fullscreen\n");
    }
    else if (width == g_fullscreen_width &&
        height == g_fullscreen_height) {
        fs_log("not setting window size to fullscreen size\n");
    }
    else {
        g_window_width = width;
        g_window_height = height;
        fs_log("resize event %d %d\n", width, height);
    }
#ifdef USE_SDL2
    g_fs_ml_video_width = width;
    g_fs_ml_video_height = height;
#else
#ifdef MACOSX
        destroy_opengl_state();
#endif
        set_video_mode();
#ifdef MACOSX
        recreate_opengl_state();
#endif
#endif
}

int fs_ml_event_loop(void)
{
    // printf("fs_ml_event_loop\n");
    int result = 0;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch(event.type) {
        case SDL_QUIT:
            fs_log("intercepted SDL_QUIT\n");
            fs_ml_quit();
#ifdef FS_EMU_DRIVERS
            printf("returning 1 from fs_ml_event_loop\n");
            result = 1;
#endif
            continue;
#ifdef USE_SDL2
        case SDL_WINDOWEVENT:
            // printf("SDL_WINDOWEVENT...\n");
            if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                on_resize(event.window.data1, event.window.data2);
            }
            else if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                event.type = SDL_QUIT;
                SDL_PushEvent(&event);
            }
            continue;
#else
        case SDL_VIDEORESIZE:
            on_resize(event.resize.w, event.resize.h);
            continue;
        case SDL_ACTIVEEVENT:
            //fs_log("got active event %d %d %d %d\n", event.active.state,
            //      SDL_APPMOUSEFOCUS, SDL_APPINPUTFOCUS, SDL_APPACTIVE);
            if ((event.active.state & SDL_APPINPUTFOCUS)) {
                if (event.active.gain) {
                    fs_log("got keyboard focus\n");
                    // just got keyboard focus -- clearing modifier states
                    fs_ml_clear_keyboard_modifier_state();
                    if (g_fs_ml_had_input_grab) {
                        fs_log("- had input grab, re-acquiring\n");
                        fs_ml_grab_input(1, 1);
                        g_fs_ml_had_input_grab = 0;
                    }
                    if (g_fs_ml_was_fullscreen) {
                        if (!g_fs_emu_video_fullscreen) {
                            fs_log("- was in fullsreen mode before (switching)\n");
                            fs_ml_toggle_fullscreen();
                        }
                        g_fs_ml_was_fullscreen = 0;
                    }
                }
                else {
                    fs_log("lost keyboard focus\n");
                    if (fs_ml_has_input_grab()) {
                        fs_log("- releasing input grab\n");
                        fs_ml_grab_input(0, 1);
                        g_fs_ml_had_input_grab = 1;
                    }
                    else {
                        fs_log("- did not have input grab\n");
                        //g_fs_ml_had_input_grab = 0;
                    }
                }
            }
            continue;
#endif
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (g_debug_input) {
                fs_log("SDL key sym %d mod %d scancode %d state %d\n",
                        event.key.keysym.sym, event.key.keysym.mod,
                        event.key.keysym.scancode, event.key.state);
            }
            if (event.key.keysym.sym == 0 && event.key.keysym.scancode == 0) {
                // ignore "ghost key" seen on OS X which without this
                // specific check will cause the A key to be mysteriously
                // pressed.
                if (g_debug_input) {
                    fs_log("- ignored key with keysym 0 and scancode 0\n");
                }
                continue;
            }
            /*
            if (event.key.keysym.sym == SDLK_F12) {
                g_f12_state = event.key.state ? FS_ML_KEY_MOD_F12 : 0;
                printf("-- g_f12_state is %d\n", g_f12_state);
            }
            else if (event.key.keysym.sym == SDLK_F11) {
                g_f11_state = event.key.state ? FS_ML_KEY_MOD_F11 : 0;
            }
            */

            const Uint8* key_state;
            int num_keys;
#ifdef USE_SDL2
            key_state = SDL_GetKeyboardState(&num_keys);
            g_f11_state = key_state[SDL_SCANCODE_F11] ? FS_ML_KEY_MOD_F11 : 0;
            g_f12_state = key_state[SDL_SCANCODE_F12] ? FS_ML_KEY_MOD_F12 : 0;
            // printf("%d %d\n", g_f11_state, g_f12_state);
#else
            key_state = SDL_GetKeyState(&num_keys);
            g_f11_state = key_state[SDLK_F11] ? FS_ML_KEY_MOD_F11 : 0;
            g_f12_state = key_state[SDLK_F12] ? FS_ML_KEY_MOD_F12 : 0;
#endif

            int key = -1;
#ifdef USE_SDL2
            if (event.key.keysym.scancode <= LAST_SDL2_SCANCODE) {
                key = g_sdl2_keys[event.key.keysym.scancode];
            }
#else
            if (0) {
            }
#endif
#if defined(MACOSX)
#ifdef USE_SDL2

#else
            else if (event.key.keysym.sym == SDLK_LSHIFT) {
                key = SDLK_LSHIFT;
            }
            else if (event.key.keysym.sym == SDLK_LCTRL) {
                key = SDLK_LCTRL;
            }
            else if (event.key.keysym.sym == SDLK_LALT) {
                key = SDLK_LALT;
            }
            else if (event.key.keysym.sym == SDLK_LMETA) {
                key = SDLK_LSUPER;
            }
            else if (event.key.keysym.sym == SDLK_RMETA) {
                key = SDLK_RSUPER;
            }
            else if (event.key.keysym.sym == SDLK_RALT) {
                key = SDLK_RALT;
            }
            else if (event.key.keysym.sym == SDLK_RCTRL) {
                key = SDLK_RCTRL;
            }
            else if (event.key.keysym.sym == SDLK_RSHIFT) {
                key = SDLK_RSHIFT;
            }
            else if (event.key.keysym.sym == SDLK_CAPSLOCK) {
                key = SDLK_CAPSLOCK;
            }
#endif
#elif defined(WINDOWS)

#else
            else if (event.key.keysym.sym == SDLK_MODE) {
                key = SDLK_RALT;
            }
#endif
            else {
                key = fs_ml_scancode_to_key(event.key.keysym.scancode);
            }

#ifdef USE_SDL2
            if (0) {
                // the below trick does not currently work for SDL2, as
                // there is no mapping yet for translated keys
            }
#else
            if (g_f12_state || g_f11_state) {
                // leave translated key code in keysym
            }
#endif
            else if (key >= 0) {
                if (g_debug_input) {
                    fs_log("- key code set to %d (was %d) based on "
                           "scancode %d\n", key, event.key.keysym.sym,
                           event.key.keysym.scancode);
                }
                event.key.keysym.sym = key;
            }

            int mod = event.key.keysym.mod;
            if (mod & KMOD_LSHIFT || mod & KMOD_RSHIFT) {
                event.key.keysym.mod |= KMOD_SHIFT;
            }
            if (mod & KMOD_LALT || mod & KMOD_RALT) {
                //mod & ~(KMOD_LALT | KMOD_RALT);
                event.key.keysym.mod |= KMOD_ALT;
            }
            if (mod & KMOD_LCTRL || mod & KMOD_RCTRL) {
                event.key.keysym.mod |= KMOD_CTRL;
            }
            if (mod & KMOD_LMETA || mod & KMOD_RMETA) {
                event.key.keysym.mod |= KMOD_META;
            }
            // filter out other modidifers
            event.key.keysym.mod &= (KMOD_SHIFT | KMOD_ALT | KMOD_CTRL |
                    KMOD_META);
            // add F11/F12 state
            event.key.keysym.mod |= g_f11_state | g_f12_state;

            //printf("%d %d %d %d\n", event.key.keysym.mod,
            //        KMOD_ALT, KMOD_LALT, KMOD_RALT);
            break;
        //case SDL_MOUSEBUTTONDOWN:
        //    printf("--- mousebutton down ---\n");
        }
        fs_ml_event *new_event = NULL;
#if !defined(USE_SDL2)
        fs_ml_event *new_event_2 = NULL;
#endif
        if (event.type == SDL_KEYDOWN) {
            new_event = fs_ml_alloc_event();
            new_event->type = FS_ML_KEYDOWN;
            new_event->key.keysym.sym = event.key.keysym.sym;
            new_event->key.keysym.mod = event.key.keysym.mod;
#ifdef USE_SDL2
            // SDL2 sends its own text input events
#else
            if (event.key.keysym.unicode && event.key.keysym.unicode < 128) {
                // FIXME: only supporting ASCII for now..
                new_event_2 = fs_ml_alloc_event();
                new_event_2->type = FS_ML_TEXTINPUT;
                new_event_2->text.text[0] = event.key.keysym.unicode;
                new_event_2->text.text[1] = '\0';
            }
#endif
            new_event->key.state = event.key.state;
        }
        else if (event.type == SDL_KEYUP) {
            new_event = fs_ml_alloc_event();
            new_event->type = FS_ML_KEYUP;
            new_event->key.keysym.sym = event.key.keysym.sym;
            new_event->key.keysym.mod = event.key.keysym.mod;
            new_event->key.state = event.key.state;
        }
        else if (event.type == SDL_JOYBUTTONDOWN) {
            new_event = fs_ml_alloc_event();
            new_event->type = FS_ML_JOYBUTTONDOWN;
            new_event->jbutton.which = g_fs_ml_first_joystick_index + \
                    event.jbutton.which;
            new_event->jbutton.button = event.jbutton.button;
            new_event->jbutton.state = event.jbutton.state;
        }
        else if (event.type == SDL_JOYBUTTONUP) {
            new_event = fs_ml_alloc_event();
            new_event->type = FS_ML_JOYBUTTONUP;
            new_event->jbutton.which = g_fs_ml_first_joystick_index + \
                    event.jbutton.which;
            new_event->jbutton.button = event.jbutton.button;
            new_event->jbutton.state = event.jbutton.state;
        }
        else if (event.type == SDL_JOYAXISMOTION) {
            new_event = fs_ml_alloc_event();
            new_event->type = FS_ML_JOYAXISMOTION;
            new_event->jaxis.which = g_fs_ml_first_joystick_index + \
                    event.jaxis.which;
            new_event->jaxis.axis = event.jaxis.axis;
            new_event->jaxis.value = event.jaxis.value;
        }
        else if (event.type == SDL_JOYHATMOTION) {
            new_event = fs_ml_alloc_event();
            new_event->type = FS_ML_JOYHATMOTION;
            new_event->jhat.which = g_fs_ml_first_joystick_index + \
                    event.jhat.which;
            new_event->jhat.hat = event.jhat.hat;
            new_event->jhat.value = event.jhat.value;
        }
        else if (event.type == SDL_MOUSEMOTION) {
            new_event = fs_ml_alloc_event();
            new_event->type = FS_ML_MOUSEMOTION;
            new_event->motion.device = g_fs_ml_first_mouse_index;
            new_event->motion.xrel = event.motion.xrel;
            new_event->motion.yrel = event.motion.yrel;
            /* Absolute window coordinates */
            new_event->motion.x = event.motion.x;
            new_event->motion.y = event.motion.y;
            //printf("ISREL %d\n", SDL_GetRelativeMouseMode());

            if (g_debug_input) {
                fs_log("SDL mouse event x: %4d y: %4d xrel: %4d yrel: %4d\n", 
                    event.motion.x, event.motion.y,
                    event.motion.xrel, event.motion.yrel);
            }
        }
        else if (event.type == SDL_MOUSEBUTTONDOWN) {
            new_event = fs_ml_alloc_event();
            new_event->type = FS_ML_MOUSEBUTTONDOWN;
            new_event->button.device = g_fs_ml_first_mouse_index;
            new_event->button.button = event.button.button;
#ifdef MACOSX
            if (new_event->button.button == 1) {
                int mod = SDL_GetModState();
                if (mod & KMOD_ALT) {
                    new_event->button.button = 2;
                }
                else if (mod & KMOD_CTRL) {
                    new_event->button.button = 3;
                }
            }
#endif
            new_event->button.state = event.button.state;
        }
        else if (event.type == SDL_MOUSEBUTTONUP) {
            new_event = fs_ml_alloc_event();
            new_event->type = FS_ML_MOUSEBUTTONUP;
            new_event->button.device = g_fs_ml_first_mouse_index;
            new_event->button.button = event.button.button;
#ifdef MACOSX
            if (new_event->button.button == 1) {
                int mod = SDL_GetModState();
                if (mod & KMOD_ALT) {
                    new_event->button.button = 2;
                }
                else if (mod & KMOD_CTRL) {
                    new_event->button.button = 3;
                }
            }
#endif
            new_event->button.state = event.button.state;
        }
#ifdef USE_SDL2
        else if (event.type == SDL_MOUSEWHEEL) {
            /*
            if (event.wheel.which == SDL_TOUCH_MOUSEID) {

            }
            */
            if (event.wheel.y) {
                if (g_debug_input) {
                    fs_log("SDL mouse event y-scroll: %4d\n",
                        event.wheel.y);
                }
                new_event = fs_ml_alloc_event();
                new_event->type = FS_ML_MOUSEBUTTONDOWN;
                if (event.wheel.y > 0) {
                    new_event->button.button = FS_ML_BUTTON_WHEELUP;
                }
                else {
                    new_event->button.button = FS_ML_BUTTON_WHEELDOWN;
                }
                new_event->button.device = g_fs_ml_first_mouse_index;
                new_event->button.state = 1;
            }
        }
        else if (event.type == SDL_TEXTINPUT) {
            new_event = fs_ml_alloc_event();
            new_event->type = FS_ML_TEXTINPUT;
            memcpy(&(new_event->text.text), &(event.text.text),
                   MIN(TEXTINPUTEVENT_TEXT_SIZE, SDL_TEXTINPUTEVENT_TEXT_SIZE));
            new_event->text.text[TEXTINPUTEVENT_TEXT_SIZE - 1] = 0;
        }
#endif
        if (new_event) {
            fs_ml_post_event(new_event);
        }
#if !defined(USE_SDL2)
        if (new_event_2) {
            fs_ml_post_event(new_event_2);
        }
#endif
    }
    return result;
}

void fs_ml_video_swap_buffers()
{
#ifdef USE_SDL2
    SDL_GL_SwapWindow(g_fs_ml_window);
#else
    SDL_GL_SwapBuffers();
#endif
}

int fs_ml_main_loop()
{
    while (g_fs_ml_running) {
        fs_ml_event_loop();
        process_video_events();

#if defined(WINDOWS) || defined (MACOSX)
        fs_ml_prevent_power_saving();
#endif
        fs_ml_render_iteration();
    }

    if (g_fs_emu_video_fullscreen) {
        if (SDL_getenv("FSGS_RETURN_CURSOR_TO") &&
                SDL_getenv("FSGS_RETURN_CURSOR_TO")[0]) {
            // we want to improve the transitioning from FS-UAE back to
            // e.g. FS-UAE Game Center - avoid blinking cursor - so we try
            // to move it (to the bottom right of the screen). This probably
            // requires that the cursor is not grabbed (SDL often keeps the
            // cursor in the center of the screen, then).
            int x = -1; int y = -1;
            sscanf(SDL_getenv("FSGS_RETURN_CURSOR_TO"), "%d,%d", &x, &y);
            if (x != -1 && y != -1) {
                fs_log("trying to move mouse cursor to x=%d y=%d\n", x, y);
                Uint8 data[] = "\0";
#ifdef USE_SDL2
                SDL_SetWindowGrab(g_fs_ml_window, SDL_FALSE);
#else
                SDL_WM_GrabInput(SDL_GRAB_OFF);
#endif
                // setting invisible cursor so we won't see it when we
                // enable the cursor in order to move it
                SDL_Cursor *cursor = SDL_CreateCursor(data, data, 8, 1, 0, 0);
                SDL_SetCursor(cursor);
                SDL_ShowCursor(SDL_ENABLE);
#ifdef USE_SDL2
                SDL_WarpMouseInWindow(g_fs_ml_window, x, y);
#else
                SDL_WarpMouse(x, y);
#endif
            }
        }
    }
    return 0;
}

void fs_ml_video_init()
{
    FS_ML_INIT_ONCE;

    fs_log("creating condition\n");
    g_video_cond = fs_condition_create();
    fs_log("creating mutex\n");
    g_video_mutex = fs_mutex_create();

    g_video_event_queue = g_queue_new();
    g_video_event_mutex = fs_mutex_create();

    g_debug_input = getenv("FS_DEBUG_INPUT") && \
            getenv("FS_DEBUG_INPUT")[0] == '1';

    fs_ml_render_init();
}

#endif
