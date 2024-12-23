#include "screen.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <sys/stat.h>
#include <errno.h>

#include "video_preprocess.h"
#include "device_time.h"

#include "events.h"
#include "icon.h"
#include "options.h"
#include "util/log.h"

#ifdef __cplusplus 
}
#endif
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#define DISPLAY_MARGINS 96

#define DOWNCAST(SINK) container_of(SINK, struct sc_screen, frame_sink)

// Forward declaration of save_frame_as_image function
static void
save_frame_as_image(const AVFrame *frame, const char *directory, uint64_t frame_number, int64_t boot_time_ms);

static void pipe_frame(const AVFrame *frame, int64_t boot_time_ms);

static inline struct sc_size
get_oriented_size(struct sc_size size, enum sc_orientation orientation) {
    struct sc_size oriented_size;
    if (sc_orientation_is_swap(orientation)) {
        oriented_size.width = size.height;
        oriented_size.height = size.width;
    } else {
        oriented_size.width = size.width;
        oriented_size.height = size.height;
    }
    return oriented_size;
}

// get the window size in a struct sc_size
static struct sc_size
get_window_size(const struct sc_screen *screen) {
    int width;
    int height;
    SDL_GetWindowSize(screen->window, &width, &height);

    struct sc_size size;
    size.width = width;
    size.height = height;
    return size;
}

static struct sc_point
get_window_position(const struct sc_screen *screen) {
    int x;
    int y;
    SDL_GetWindowPosition(screen->window, &x, &y);

    struct sc_point point;
    point.x = x;
    point.y = y;
    return point;
}

// set the window size to be applied when fullscreen is disabled
static void
set_window_size(struct sc_screen *screen, struct sc_size new_size) {
    assert(!screen->fullscreen);
    assert(!screen->maximized);
    assert(!screen->minimized);
    SDL_SetWindowSize(screen->window, new_size.width, new_size.height);
}

// get the preferred display bounds (i.e. the screen bounds with some margins)
static bool
get_preferred_display_bounds(struct sc_size *bounds) {
    SDL_Rect rect;
    if (SDL_GetDisplayUsableBounds(0, &rect)) {
        LOGW("Could not get display usable bounds: %s", SDL_GetError());
        return false;
    }

    bounds->width = MAX(0, rect.w - DISPLAY_MARGINS);
    bounds->height = MAX(0, rect.h - DISPLAY_MARGINS);
    return true;
}

static bool
is_optimal_size(struct sc_size current_size, struct sc_size content_size) {
    // The size is optimal if we can recompute one dimension of the current
    // size from the other
    return current_size.height == current_size.width * content_size.height
                                                     / content_size.width
        || current_size.width == current_size.height * content_size.width
                                                     / content_size.height;
}

// return the optimal size of the window, with the following constraints:
//  - it attempts to keep at least one dimension of the current_size (i.e. it
//    crops the black borders)
//  - it keeps the aspect ratio
//  - it scales down to make it fit in the display_size
static struct sc_size
get_optimal_size(struct sc_size current_size, struct sc_size content_size,
                 bool within_display_bounds) {
    if (content_size.width == 0 || content_size.height == 0) {
        // avoid division by 0
        return current_size;
    }

    struct sc_size window_size;

    struct sc_size display_size;
    if (!within_display_bounds ||
            !get_preferred_display_bounds(&display_size)) {
        // do not constraint the size
        window_size = current_size;
    } else {
        window_size.width = MIN(current_size.width, display_size.width);
        window_size.height = MIN(current_size.height, display_size.height);
    }

    if (is_optimal_size(window_size, content_size)) {
        return window_size;
    }

    bool keep_width = content_size.width * window_size.height
                    > content_size.height * window_size.width;
    if (keep_width) {
        // remove black borders on top and bottom
        window_size.height = content_size.height * window_size.width
                           / content_size.width;
    } else {
        // remove black borders on left and right (or none at all if it already
        // fits)
        window_size.width = content_size.width * window_size.height
                          / content_size.height;
    }

    return window_size;
}

// initially, there is no current size, so use the frame size as current size
// req_width and req_height, if not 0, are the sizes requested by the user
static inline struct sc_size
get_initial_optimal_size(struct sc_size content_size, uint16_t req_width,
                         uint16_t req_height) {
    struct sc_size window_size;
    if (!req_width && !req_height) {
        window_size = get_optimal_size(content_size, content_size, true);
    } else {
        if (req_width) {
            window_size.width = req_width;
        } else {
            // compute from the requested height
            window_size.width = (uint32_t) req_height * content_size.width
                              / content_size.height;
        }
        if (req_height) {
            window_size.height = req_height;
        } else {
            // compute from the requested width
            window_size.height = (uint32_t) req_width * content_size.height
                               / content_size.width;
        }
    }
    return window_size;
}

static inline bool
sc_screen_is_relative_mode(struct sc_screen *screen) {
    // screen->im.mp may be NULL if --no-control
    return screen->im.mp && screen->im.mp->relative_mode;
}

static void
sc_screen_set_mouse_capture(struct sc_screen *screen, bool capture) {
#ifdef __APPLE__
    // Workaround for SDL bug on macOS:
    // <https://github.com/libsdl-org/SDL/issues/5340>
    if (capture) {
        int mouse_x, mouse_y;
        SDL_GetGlobalMouseState(&mouse_x, &mouse_y);

        int x, y, w, h;
        SDL_GetWindowPosition(screen->window, &x, &y);
        SDL_GetWindowSize(screen->window, &w, &h);

        bool outside_window = mouse_x < x || mouse_x >= x + w
                           || mouse_y < y || mouse_y >= y + h;
        if (outside_window) {
            SDL_WarpMouseInWindow(screen->window, w / 2, h / 2);
        }
    }
#else
    (void) screen;
#endif
    if (SDL_SetRelativeMouseMode(capture)) {
        LOGE("Could not set relative mouse mode to %s: %s",
             capture ? "true" : "false", SDL_GetError());
    }
}

static inline bool
sc_screen_get_mouse_capture(struct sc_screen *screen) {
    (void) screen;
    return SDL_GetRelativeMouseMode();
}

static inline void
sc_screen_toggle_mouse_capture(struct sc_screen *screen) {
    (void) screen;
    bool new_value = !sc_screen_get_mouse_capture(screen);
    sc_screen_set_mouse_capture(screen, new_value);
}

static void
sc_screen_update_content_rect(struct sc_screen *screen) {
    assert(screen->video);

    int dw;
    int dh;
    SDL_GL_GetDrawableSize(screen->window, &dw, &dh);

    struct sc_size content_size = screen->content_size;
    // The drawable size is the window size * the HiDPI scale
    struct sc_size drawable_size = {dw, dh};

    SDL_Rect *rect = &screen->rect;

    if (is_optimal_size(drawable_size, content_size)) {
        rect->x = 0;
        rect->y = 0;
        rect->w = drawable_size.width;
        rect->h = drawable_size.height;
        return;
    }

    bool keep_width = content_size.width * drawable_size.height
                    > content_size.height * drawable_size.width;
    if (keep_width) {
        rect->x = 0;
        rect->w = drawable_size.width;
        rect->h = drawable_size.width * content_size.height
                                      / content_size.width;
        rect->y = (drawable_size.height - rect->h) / 2;
    } else {
        rect->y = 0;
        rect->h = drawable_size.height;
        rect->w = drawable_size.height * content_size.width
                                       / content_size.height;
        rect->x = (drawable_size.width - rect->w) / 2;
    }
}

// render the texture to the renderer
//
// Set the update_content_rect flag if the window or content size may have
// changed, so that the content rectangle is recomputed
static void
sc_screen_render(struct sc_screen *screen, bool update_content_rect) {
    assert(screen->video);

    if (update_content_rect) {
        sc_screen_update_content_rect(screen);
    }

    enum sc_display_result res =
        sc_display_render(&screen->display, &screen->rect, screen->orientation);
    (void) res; // any error already logged
}

static void
sc_screen_render_novideo(struct sc_screen *screen) {
    enum sc_display_result res =
        sc_display_render(&screen->display, NULL, SC_ORIENTATION_0);
    (void) res; // any error already logged
}

#if defined(__APPLE__) || defined(__WINDOWS__)
# define CONTINUOUS_RESIZING_WORKAROUND
#endif

#ifdef CONTINUOUS_RESIZING_WORKAROUND
// On Windows and MacOS, resizing blocks the event loop, so resizing events are
// not triggered. As a workaround, handle them in an event handler.
//
// <https://bugzilla.libsdl.org/show_bug.cgi?id=2077>
// <https://stackoverflow.com/a/40693139/1987178>
static int
event_watcher(void *data, SDL_Event *event) {
    struct sc_screen *screen = data;
    assert(screen->video);

    if (event->type == SDL_WINDOWEVENT
            && event->window.event == SDL_WINDOWEVENT_RESIZED) {
        // In practice, it seems to always be called from the same thread in
        // that specific case. Anyway, it's just a workaround.
        sc_screen_render(screen, true);
    }
    return 0;
}
#endif

static bool
sc_screen_frame_sink_open(struct sc_frame_sink *sink,
                          const AVCodecContext *ctx) {
    assert(ctx->pix_fmt == AV_PIX_FMT_YUV420P);
    (void) ctx;

    struct sc_screen *screen = DOWNCAST(sink);

    if (ctx->width <= 0 || ctx->width > 0xFFFF
            || ctx->height <= 0 || ctx->height > 0xFFFF) {
        LOGE("Invalid video size: %dx%d", ctx->width, ctx->height);
        return false;
    }

    assert(ctx->width > 0 && ctx->width <= 0xFFFF);
    assert(ctx->height > 0 && ctx->height <= 0xFFFF);
    // screen->frame_size is never used before the event is pushed, and the
    // event acts as a memory barrier so it is safe without mutex
    screen->frame_size.width = ctx->width;
    screen->frame_size.height = ctx->height;

    // Post the event on the UI thread (the texture must be created from there)
    bool ok = sc_push_event(SC_EVENT_SCREEN_INIT_SIZE);
    if (!ok) {
        return false;
    }

#ifndef NDEBUG
    screen->open = true;
#endif

    // nothing to do, the screen is already open on the main thread
    return true;
}

static void
sc_screen_frame_sink_close(struct sc_frame_sink *sink) {
    struct sc_screen *screen = DOWNCAST(sink);
    (void) screen;
#ifndef NDEBUG
    screen->open = false;
#endif

    // nothing to do, the screen lifecycle is not managed by the frame producer
}

static bool
sc_screen_frame_sink_push(struct sc_frame_sink *sink, const AVFrame *frame) {
    struct sc_screen *screen = DOWNCAST(sink);
    assert(screen->video);

    bool previous_skipped;
    bool ok = sc_frame_buffer_push(&screen->fb, frame, &previous_skipped);
    if (!ok) {
        return false;
    }

    if (previous_skipped) {
        sc_fps_counter_add_skipped_frame(&screen->fps_counter);
        // The SC_EVENT_NEW_FRAME triggered for the previous frame will consume
        // this new frame instead
    } else {
        // Post the event on the UI thread
        bool ok = sc_push_event(SC_EVENT_NEW_FRAME);
        if (!ok) {
            return false;
        }
    }

    return true;
}

bool
sc_screen_init(struct sc_screen *screen,
               const struct sc_screen_params *params) {
    screen->resize_pending = false;
    screen->has_frame = false;
    screen->fullscreen = false;
    screen->maximized = false;
    screen->minimized = false;
    screen->mouse_capture_key_pressed = 0;
    screen->paused = false;
    screen->resume_frame = NULL;
    screen->orientation = SC_ORIENTATION_0;

    screen->video = params->video;

    screen->req.x = params->window_x;
    screen->req.y = params->window_y;
    screen->req.width = params->window_width;
    screen->req.height = params->window_height;
    screen->req.fullscreen = params->fullscreen;
    screen->req.start_fps_counter = params->start_fps_counter;

    if (params->save_frames || params->pipe_output || params->show_timestamps) {
        const char *adb_path = params->adb_path ? params->adb_path : "adb";
        screen->device_boot_time = get_device_boot_time(params->serial, adb_path);
        LOGI("Device boot time: %lld", screen->device_boot_time);
    } else {
        screen->device_boot_time = 0;
    }
    
    screen->pipe_output = params->pipe_output;

    bool ok = sc_frame_buffer_init(&screen->fb);
    if (!ok) {
        return false;
    }

    if (!sc_fps_counter_init(&screen->fps_counter)) {
        goto error_destroy_frame_buffer;
    }

    if (screen->video) {
        screen->orientation = params->orientation;
        if (screen->orientation != SC_ORIENTATION_0) {
            LOGI("Initial display orientation set to %s",
                 sc_orientation_get_name(screen->orientation));
        }
    }

    uint32_t window_flags = SDL_WINDOW_ALLOW_HIGHDPI;
    if (params->always_on_top) {
        window_flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    }
    if (params->window_borderless) {
        window_flags |= SDL_WINDOW_BORDERLESS;
    }
    if (params->video) {
        // The window will be shown on first frame
        window_flags |= SDL_WINDOW_HIDDEN
                      | SDL_WINDOW_RESIZABLE;
    }

    const char *title = params->window_title;
    assert(title);

    int x = SDL_WINDOWPOS_UNDEFINED;
    int y = SDL_WINDOWPOS_UNDEFINED;
    int width = 256;
    int height = 256;
    if (params->window_x != SC_WINDOW_POSITION_UNDEFINED) {
        x = params->window_x;
    }
    if (params->window_y != SC_WINDOW_POSITION_UNDEFINED) {
        y = params->window_y;
    }
    if (params->window_width) {
        width = params->window_width;
    }
    if (params->window_height) {
        height = params->window_height;
    }

    // The window will be positioned and sized on first video frame
    screen->window = SDL_CreateWindow(title, x, y, width, height, window_flags);
    if (!screen->window) {
        LOGE("Could not create window: %s", SDL_GetError());
        goto error_destroy_fps_counter;
    }

    SDL_Surface *icon = scrcpy_icon_load();
    if (icon) {
        SDL_SetWindowIcon(screen->window, icon);
    } else if (params->video) {
        // just a warning
        LOGW("Could not load icon");
    } else {
        // without video, the icon is used as window content, it must be present
        LOGE("Could not load icon");
        goto error_destroy_fps_counter;
    }

    SDL_Surface *icon_novideo = params->video ? NULL : icon;
    bool mipmaps = params->video && params->mipmaps;
    ok = sc_display_init(&screen->display, screen->window, icon_novideo,
                         mipmaps);
    if (icon) {
        scrcpy_icon_destroy(icon);
    }
    if (!ok) {
        goto error_destroy_window;
    }

    screen->frame = av_frame_alloc();
    if (!screen->frame) {
        LOG_OOM();
        goto error_destroy_display;
    }

    screen->processed_frame = av_frame_alloc();
    if (!screen->processed_frame) {
        LOG_OOM();
        av_frame_free(&screen->frame);
        goto error_destroy_display;
    }

    struct sc_input_manager_params im_params = {
        .controller = params->controller,
        .fp = params->fp,
        .screen = screen,
        .kp = params->kp,
        .mp = params->mp,
        .gp = params->gp,
        .mouse_bindings = params->mouse_bindings,
        .legacy_paste = params->legacy_paste,
        .clipboard_autosync = params->clipboard_autosync,
        .shortcut_mods = params->shortcut_mods,
    };

    sc_input_manager_init(&screen->im, &im_params);

#ifdef CONTINUOUS_RESIZING_WORKAROUND
    if (screen->video) {
        SDL_AddEventWatch(event_watcher, screen);
    }
#endif

    static const struct sc_frame_sink_ops ops = {
        .open = sc_screen_frame_sink_open,
        .close = sc_screen_frame_sink_close,
        .push = sc_screen_frame_sink_push,
    };

    screen->frame_sink.ops = &ops;

#ifndef NDEBUG
    screen->open = false;
#endif

    if (!screen->video && sc_screen_is_relative_mode(screen)) {
        // Capture mouse immediately if video mirroring is disabled
        sc_screen_set_mouse_capture(screen, true);
    }

    screen->frame_count = 0;
    screen->save_frames = params->save_frames;
    screen->frame_dir = params->frame_dir;
    screen->opencv_enabled = params->opencv_enabled;
    screen->opencv_map_path = params->opencv_map_path;
    // LOGI("Saving frames: %d, Frame directory: %s", screen->save_frames, screen->frame_dir);
    // Create directory if it doesn't exist and saving is enabled
    if (screen->save_frames && screen->frame_dir) {
#ifdef _WIN32
        if (mkdir(screen->frame_dir) < 0 && errno != EEXIST) {
#else
        if (mkdir(screen->frame_dir, 0777) < 0 && errno != EEXIST) {
#endif
            LOGE("Could not create frame directory: %s", screen->frame_dir);
            return false;
        }
    }
    screen->show_timestamps = params->show_timestamps;
    return true;

error_destroy_display:
    sc_display_destroy(&screen->display);
error_destroy_window:
    SDL_DestroyWindow(screen->window);
error_destroy_fps_counter:
    sc_fps_counter_destroy(&screen->fps_counter);
error_destroy_frame_buffer:
    sc_frame_buffer_destroy(&screen->fb);

    return false;
}

static void
sc_screen_show_initial_window(struct sc_screen *screen) {
    int x = screen->req.x != SC_WINDOW_POSITION_UNDEFINED
          ? screen->req.x : (int) SDL_WINDOWPOS_CENTERED;
    int y = screen->req.y != SC_WINDOW_POSITION_UNDEFINED
          ? screen->req.y : (int) SDL_WINDOWPOS_CENTERED;

    struct sc_size window_size =
        get_initial_optimal_size(screen->content_size, screen->req.width,
                                                       screen->req.height);

    set_window_size(screen, window_size);
    SDL_SetWindowPosition(screen->window, x, y);

    if (screen->req.fullscreen) {
        sc_screen_switch_fullscreen(screen);
    }

    if (screen->req.start_fps_counter) {
        sc_fps_counter_start(&screen->fps_counter);
    }

    SDL_ShowWindow(screen->window);
    sc_screen_update_content_rect(screen);
}

void
sc_screen_hide_window(struct sc_screen *screen) {
    SDL_HideWindow(screen->window);
}

void
sc_screen_interrupt(struct sc_screen *screen) {
    sc_fps_counter_interrupt(&screen->fps_counter);
}

void
sc_screen_join(struct sc_screen *screen) {
    sc_fps_counter_join(&screen->fps_counter);
}

void
sc_screen_destroy(struct sc_screen *screen) {
#ifndef NDEBUG
    assert(!screen->open);
#endif
    sc_display_destroy(&screen->display);
    av_frame_free(&screen->frame);
    av_frame_free(&screen->processed_frame);
    SDL_DestroyWindow(screen->window);
    sc_fps_counter_destroy(&screen->fps_counter);
    sc_frame_buffer_destroy(&screen->fb);
}

static void
resize_for_content(struct sc_screen *screen, struct sc_size old_content_size,
                   struct sc_size new_content_size) {
    assert(screen->video);

    struct sc_size window_size = get_window_size(screen);
    struct sc_size target_size = {
        .width = (uint32_t) window_size.width * new_content_size.width
                / old_content_size.width,
        .height = (uint32_t) window_size.height * new_content_size.height
                / old_content_size.height,
    };
    target_size = get_optimal_size(target_size, new_content_size, true);
    set_window_size(screen, target_size);
}

static void
set_content_size(struct sc_screen *screen, struct sc_size new_content_size) {
    assert(screen->video);

    if (!screen->fullscreen && !screen->maximized && !screen->minimized) {
        resize_for_content(screen, screen->content_size, new_content_size);
    } else if (!screen->resize_pending) {
        // Store the windowed size to be able to compute the optimal size once
        // fullscreen/maximized/minimized are disabled
        screen->windowed_content_size = screen->content_size;
        screen->resize_pending = true;
    }

    screen->content_size = new_content_size;
}

static void
apply_pending_resize(struct sc_screen *screen) {
    assert(screen->video);

    assert(!screen->fullscreen);
    assert(!screen->maximized);
    assert(!screen->minimized);
    if (screen->resize_pending) {
        resize_for_content(screen, screen->windowed_content_size,
                                   screen->content_size);
        screen->resize_pending = false;
    }
}

void
sc_screen_set_orientation(struct sc_screen *screen,
                          enum sc_orientation orientation) {
    assert(screen->video);

    if (orientation == screen->orientation) {
        return;
    }

    struct sc_size new_content_size =
        get_oriented_size(screen->frame_size, orientation);

    set_content_size(screen, new_content_size);

    screen->orientation = orientation;
    LOGI("Display orientation set to %s", sc_orientation_get_name(orientation));

    sc_screen_render(screen, true);
}

static bool
sc_screen_init_size(struct sc_screen *screen) {
    // Before first frame
    assert(!screen->has_frame);

    // The requested size is passed via screen->frame_size

    struct sc_size content_size =
        get_oriented_size(screen->frame_size, screen->orientation);
    screen->content_size = content_size;

    enum sc_display_result res =
        sc_display_set_texture_size(&screen->display, screen->frame_size);
    return res != SC_DISPLAY_RESULT_ERROR;
}

// recreate the texture and resize the window if the frame size has changed
static enum sc_display_result
prepare_for_frame(struct sc_screen *screen, struct sc_size new_frame_size) {
    assert(screen->video);

    if (screen->frame_size.width == new_frame_size.width
            && screen->frame_size.height == new_frame_size.height) {
        return SC_DISPLAY_RESULT_OK;
    }

    // frame dimension changed
    screen->frame_size = new_frame_size;

    struct sc_size new_content_size =
        get_oriented_size(new_frame_size, screen->orientation);
    set_content_size(screen, new_content_size);

    sc_screen_update_content_rect(screen);

    return sc_display_set_texture_size(&screen->display, screen->frame_size);
}

static bool
sc_screen_apply_frame(struct sc_screen *screen, AVFrame *frame) {
    assert(screen->video);

    sc_fps_counter_add_rendered_frame(&screen->fps_counter);

    struct sc_size new_frame_size = {frame->width, frame->height};
    enum sc_display_result res = prepare_for_frame(screen, new_frame_size);
    if (res == SC_DISPLAY_RESULT_ERROR) {
        return false;
    }
    if (res == SC_DISPLAY_RESULT_PENDING) {
        // Not an error, but do not continue
        return true;
    }

    res = sc_display_update_texture(&screen->display, frame);
    if (res == SC_DISPLAY_RESULT_ERROR) {
        return false;
    }
    if (res == SC_DISPLAY_RESULT_PENDING) {
        // Not an error, but do not continue
        return true;
    }

    if (!screen->has_frame) {
        screen->has_frame = true;
        // this is the very first frame, show the window
        sc_screen_show_initial_window(screen);

        if (sc_screen_is_relative_mode(screen)) {
            // Capture mouse on start
            sc_screen_set_mouse_capture(screen, true);
        }
    }

    sc_screen_render(screen, false);
    return true;
}

static bool
sc_screen_update_frame(struct sc_screen *screen) {
    assert(screen->video);

    if (screen->paused) {
        if (!screen->resume_frame) {
            screen->resume_frame = av_frame_alloc();
            if (!screen->resume_frame) {
                LOG_OOM();
                return false;
            }
        } else {
            av_frame_unref(screen->resume_frame);
        }
        sc_frame_buffer_consume(&screen->fb, screen->resume_frame);

        // Create a copy of the frame for processing
        av_frame_unref(screen->processed_frame);
        av_frame_ref(screen->processed_frame, screen->resume_frame);

        if ((screen->opencv_enabled && screen->opencv_map_path) || screen->show_timestamps) {
            if (screen->show_timestamps) {
                // Get PTS (Presentation TimeStamp) from frame
                int64_t pts = screen->processed_frame->pts;
                // Convert PTS from microseconds to milliseconds
                int64_t pts_ms = pts / 1000;
                char timestamp_str[64];
                if (pts == AV_NOPTS_VALUE) {
                    snprintf(timestamp_str, sizeof(timestamp_str), "%s", "No timestamps");
                } else {
                    // Calculate actual timestamp by adding PTS (converted to ms) to boot time
                    int64_t timestamp_ms = screen->device_boot_time + pts_ms;
                    snprintf(timestamp_str, sizeof(timestamp_str), "%s", fromTimestamp(timestamp_ms));
                }
                apply_video_effects(screen->processed_frame, screen->opencv_map_path, timestamp_str);
            }else{
                apply_video_effects(screen->processed_frame, screen->opencv_map_path, NULL);
            }
        }

        return true;
    }

    av_frame_unref(screen->frame);
    sc_frame_buffer_consume(&screen->fb, screen->frame);

    // Create a deep copy instead of just referencing
    screen->processed_frame->format = screen->frame->format;
    screen->processed_frame->width = screen->frame->width;
    screen->processed_frame->height = screen->frame->height;
    screen->processed_frame->pts = screen->frame->pts;
    av_frame_get_buffer(screen->processed_frame, 0);
    av_frame_copy(screen->processed_frame, screen->frame);
    av_frame_copy_props(screen->processed_frame, screen->frame);

    // Apply video effects if enabled
    if ((screen->opencv_enabled && screen->opencv_map_path) || screen->show_timestamps) {
        if (screen->show_timestamps) {
            // Get PTS (Presentation TimeStamp) from frame
            int64_t pts = screen->processed_frame->pts;
            // Convert PTS from microseconds to milliseconds
            int64_t pts_ms = pts / 1000;
            char timestamp_str[64];
            if (pts == AV_NOPTS_VALUE) {
                snprintf(timestamp_str, sizeof(timestamp_str), "%s", "No timestamps");
            } else {
                // Calculate actual timestamp by adding PTS (converted to ms) to boot time
                int64_t timestamp_ms = screen->device_boot_time + pts_ms;
                snprintf(timestamp_str, sizeof(timestamp_str), "%s", fromTimestamp(timestamp_ms));
            }
            apply_video_effects(screen->processed_frame, screen->opencv_map_path, timestamp_str);
        }else{
            apply_video_effects(screen->processed_frame, screen->opencv_map_path, NULL);
        }
    }

    if (screen->save_frames && screen->frame_dir){
        save_frame_as_image(screen->processed_frame, screen->frame_dir, 
                            screen->frame_count++, screen->device_boot_time);
    }

    if (screen->pipe_output) {
        pipe_frame(screen->processed_frame, screen->device_boot_time);
    }

    return sc_screen_apply_frame(screen, screen->processed_frame);
}

void
sc_screen_set_paused(struct sc_screen *screen, bool paused) {
    assert(screen->video);

    if (!paused && !screen->paused) {
        // nothing to do
        return;
    }

    if (screen->paused && screen->resume_frame) {
        // If display screen was paused, refresh the frame immediately, even if
        // the new state is also paused.
        av_frame_free(&screen->frame);
        screen->frame = screen->resume_frame;
        screen->resume_frame = NULL;
        sc_screen_apply_frame(screen, screen->frame);
    }

    if (!paused) {
        LOGI("Display screen unpaused");
    } else if (!screen->paused) {
        LOGI("Display screen paused");
    } else {
        LOGI("Display screen re-paused");
    }

    screen->paused = paused;
}

void
sc_screen_switch_fullscreen(struct sc_screen *screen) {
    assert(screen->video);

    uint32_t new_mode = screen->fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (SDL_SetWindowFullscreen(screen->window, new_mode)) {
        LOGW("Could not switch fullscreen mode: %s", SDL_GetError());
        return;
    }

    screen->fullscreen = !screen->fullscreen;
    if (!screen->fullscreen && !screen->maximized && !screen->minimized) {
        apply_pending_resize(screen);
    }

    LOGD("Switched to %s mode", screen->fullscreen ? "fullscreen" : "windowed");
    sc_screen_render(screen, true);
}

void
sc_screen_resize_to_fit(struct sc_screen *screen) {
    assert(screen->video);

    if (screen->fullscreen || screen->maximized || screen->minimized) {
        return;
    }

    struct sc_point point = get_window_position(screen);
    struct sc_size window_size = get_window_size(screen);

    struct sc_size optimal_size =
        get_optimal_size(window_size, screen->content_size, false);

    // Center the window related to the device screen
    assert(optimal_size.width <= window_size.width);
    assert(optimal_size.height <= window_size.height);
    uint32_t new_x = point.x + (window_size.width - optimal_size.width) / 2;
    uint32_t new_y = point.y + (window_size.height - optimal_size.height) / 2;

    SDL_SetWindowSize(screen->window, optimal_size.width, optimal_size.height);
    SDL_SetWindowPosition(screen->window, new_x, new_y);
    LOGD("Resized to optimal size: %ux%u", optimal_size.width,
                                           optimal_size.height);
}

void
sc_screen_resize_to_pixel_perfect(struct sc_screen *screen) {
    assert(screen->video);

    if (screen->fullscreen || screen->minimized) {
        return;
    }

    if (screen->maximized) {
        SDL_RestoreWindow(screen->window);
        screen->maximized = false;
    }

    struct sc_size content_size = screen->content_size;
    SDL_SetWindowSize(screen->window, content_size.width, content_size.height);
    LOGD("Resized to pixel-perfect: %ux%u", content_size.width,
                                            content_size.height);
}

static inline bool
sc_screen_is_mouse_capture_key(SDL_Keycode key) {
    return key == SDLK_LALT || key == SDLK_LGUI || key == SDLK_RGUI;
}

bool
sc_screen_handle_event(struct sc_screen *screen, const SDL_Event *event) {
    bool relative_mode = sc_screen_is_relative_mode(screen);

    switch (event->type) {
        case SC_EVENT_SCREEN_INIT_SIZE: {
            // The initial size is passed via screen->frame_size
            bool ok = sc_screen_init_size(screen);
            if (!ok) {
                LOGE("Could not initialize screen size");
                return false;
            }
            return true;
        }
        case SC_EVENT_NEW_FRAME: {
            bool ok = sc_screen_update_frame(screen);
            if (!ok) {
                LOGE("Frame update failed\n");
                return false;
            }
            return true;
        }
        case SDL_WINDOWEVENT:
            if (!screen->video
                    && event->window.event == SDL_WINDOWEVENT_EXPOSED) {
                sc_screen_render_novideo(screen);
            }

            // !video implies !has_frame
            assert(screen->video || !screen->has_frame);
            if (!screen->has_frame) {
                // Do nothing
                return true;
            }
            switch (event->window.event) {
                case SDL_WINDOWEVENT_EXPOSED:
                    sc_screen_render(screen, true);
                    break;
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    sc_screen_render(screen, true);
                    break;
                case SDL_WINDOWEVENT_MAXIMIZED:
                    screen->maximized = true;
                    break;
                case SDL_WINDOWEVENT_MINIMIZED:
                    screen->minimized = true;
                    break;
                case SDL_WINDOWEVENT_RESTORED:
                    if (screen->fullscreen) {
                        // On Windows, in maximized+fullscreen, disabling
                        // fullscreen mode unexpectedly triggers the "restored"
                        // then "maximized" events, leaving the window in a
                        // weird state (maximized according to the events, but
                        // not maximized visually).
                        break;
                    }
                    screen->maximized = false;
                    screen->minimized = false;
                    apply_pending_resize(screen);
                    sc_screen_render(screen, true);
                    break;
                case SDL_WINDOWEVENT_FOCUS_LOST:
                    if (relative_mode) {
                        sc_screen_set_mouse_capture(screen, false);
                    }
                    break;
            }
            return true;
        case SDL_KEYDOWN:
            if (relative_mode) {
                SDL_Keycode key = event->key.keysym.sym;
                if (sc_screen_is_mouse_capture_key(key)) {
                    if (!screen->mouse_capture_key_pressed) {
                        screen->mouse_capture_key_pressed = key;
                    } else {
                        // Another mouse capture key has been pressed, cancel
                        // mouse (un)capture
                        screen->mouse_capture_key_pressed = 0;
                    }
                    // Mouse capture keys are never forwarded to the device
                    return true;
                }
            }
            break;
        case SDL_KEYUP:
            if (relative_mode) {
                SDL_Keycode key = event->key.keysym.sym;
                SDL_Keycode cap = screen->mouse_capture_key_pressed;
                screen->mouse_capture_key_pressed = 0;
                if (sc_screen_is_mouse_capture_key(key)) {
                    if (key == cap) {
                        // A mouse capture key has been pressed then released:
                        // toggle the capture mouse mode
                        sc_screen_toggle_mouse_capture(screen);
                    }
                    // Mouse capture keys are never forwarded to the device
                    return true;
                }
            }
            break;
        case SDL_MOUSEWHEEL:
        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
            if (relative_mode && !sc_screen_get_mouse_capture(screen)) {
                // Do not forward to input manager, the mouse will be captured
                // on SDL_MOUSEBUTTONUP
                return true;
            }
            break;
        case SDL_FINGERMOTION:
        case SDL_FINGERDOWN:
        case SDL_FINGERUP:
            if (relative_mode) {
                // Touch events are not compatible with relative mode
                // (coordinates are not relative)
                return true;
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (relative_mode && !sc_screen_get_mouse_capture(screen)) {
                sc_screen_set_mouse_capture(screen, true);
                return true;
            }
            break;
    }

    sc_input_manager_handle_event(&screen->im, event);
    return true;
}

struct sc_point
sc_screen_convert_drawable_to_frame_coords(struct sc_screen *screen,
                                           int32_t x, int32_t y) {
    assert(screen->video);

    enum sc_orientation orientation = screen->orientation;

    int32_t w = screen->content_size.width;
    int32_t h = screen->content_size.height;

    // screen->rect must be initialized to avoid a division by zero
    assert(screen->rect.w && screen->rect.h);

    x = (int64_t) (x - screen->rect.x) * w / screen->rect.w;
    y = (int64_t) (y - screen->rect.y) * h / screen->rect.h;

    struct sc_point result;
    switch (orientation) {
        case SC_ORIENTATION_0:
            result.x = x;
            result.y = y;
            break;
        case SC_ORIENTATION_90:
            result.x = y;
            result.y = w - x;
            break;
        case SC_ORIENTATION_180:
            result.x = w - x;
            result.y = h - y;
            break;
        case SC_ORIENTATION_270:
            result.x = h - y;
            result.y = x;
            break;
        case SC_ORIENTATION_FLIP_0:
            result.x = w - x;
            result.y = y;
            break;
        case SC_ORIENTATION_FLIP_90:
            result.x = h - y;
            result.y = w - x;
            break;
        case SC_ORIENTATION_FLIP_180:
            result.x = x;
            result.y = h - y;
            break;
        default:
            assert(orientation == SC_ORIENTATION_FLIP_270);
            result.x = y;
            result.y = x;
            break;
    }

    return result;
}

struct sc_point
sc_screen_convert_window_to_frame_coords(struct sc_screen *screen,
                                         int32_t x, int32_t y) {
    sc_screen_hidpi_scale_coords(screen, &x, &y);
    return sc_screen_convert_drawable_to_frame_coords(screen, x, y);
}

void
sc_screen_hidpi_scale_coords(struct sc_screen *screen, int32_t *x, int32_t *y) {
    // take the HiDPI scaling (dw/ww and dh/wh) into account
    int ww, wh, dw, dh;
    SDL_GetWindowSize(screen->window, &ww, &wh);
    SDL_GL_GetDrawableSize(screen->window, &dw, &dh);

    // scale for HiDPI (64 bits for intermediate multiplications)
    *x = (int64_t) *x * dw / ww;
    *y = (int64_t) *y * dh / wh;
}

// Add this function to save frames
static void
save_frame_as_image(const AVFrame *frame, const char *directory, uint64_t frame_number, int64_t boot_time_ms) {
    char filename[256];
    // Get PTS (Presentation TimeStamp) from frame
    int64_t pts = frame->pts;
    // Convert PTS from microseconds to milliseconds
    int64_t pts_ms = pts / 1000;
    if (pts == AV_NOPTS_VALUE) {
        // If no PTS available, fallback to just frame number
        snprintf(filename, sizeof(filename), "%s/frame_%06d.ppm", 
                 directory, (int)frame_number);
    } else {
        // Calculate actual timestamp by adding PTS (converted to ms) to boot time
        int64_t timestamp_ms = boot_time_ms + pts_ms;
        snprintf(filename, sizeof(filename), "%s/frame_%06d_%lld.ppm", 
                 directory, (int)frame_number, timestamp_ms);
    }

    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        LOGE("Could not open file for frame saving: %s", filename);
        return;
    }

    // Create a copy of the frame for processing
    AVFrame *processed = av_frame_alloc();
    if (!processed) {
        LOGE("Could not allocate processed frame");
        fclose(fp);
        return;
    }
    
    // Copy the frame data
    av_frame_copy_props(processed, frame);
    processed->format = frame->format;
    processed->width = frame->width;
    processed->height = frame->height;
    if (av_frame_get_buffer(processed, 0) < 0) {
        LOGE("Could not allocate frame data");
        av_frame_free(&processed);
        fclose(fp);
        return;
    }
    av_frame_copy(processed, frame);

    // Convert from YUV420P to RGB24
    int rgb_linesize[1] = { 3 * processed->width }; // RGB stride
    uint8_t *rgb_data[1] = { NULL };               // RGB data buffer
    rgb_data[0] = malloc(rgb_linesize[0] * processed->height);
    if (!rgb_data[0]) {
        LOGE("Could not allocate RGB buffer");
        av_frame_free(&processed);
        fclose(fp);
        return;
    }

    struct SwsContext *sws_ctx = sws_getContext(
        processed->width, processed->height, AV_PIX_FMT_YUV420P,
        processed->width, processed->height, AV_PIX_FMT_RGB24,
        SWS_BICUBIC, NULL, NULL, NULL);

    if (!sws_ctx) {
        LOGE("Could not initialize SwsContext");
        free(rgb_data[0]);
        av_frame_free(&processed);
        fclose(fp);
        return;
    }

    sws_scale(sws_ctx, (const uint8_t * const *)processed->data, 
              processed->linesize, 0, processed->height,
              rgb_data, rgb_linesize);

    // Write PPM header
    fprintf(fp, "P6\n%d %d\n255\n", processed->width, processed->height);
    
    // Write RGB data
    fwrite(rgb_data[0], 1, rgb_linesize[0] * processed->height, fp);

    // Cleanup
    sws_freeContext(sws_ctx);
    free(rgb_data[0]);
    av_frame_free(&processed);
    fclose(fp);
}

// Define frame header structure
#pragma pack(push, 1)  // Ensure struct is packed without padding
struct frame_header {
    uint8_t delimiter[8];  // 8-byte delimiter that's unlikely to appear in YUV data
    int64_t timestamp_ms;  // 8-byte timestamp
    int32_t width;        // 4-byte width
    int32_t height;       // 4-byte height
    uint32_t frame_size;  // 4-byte frame size
    uint32_t checksum;    // 4-byte checksum for header validation
};
#pragma pack(pop)

// Define a unique delimiter that cannot appear in YUV420P data
static const uint8_t FRAME_DELIMITER[8] = {
    0xFF, 0xFF, 0xFF, 0xFF,  // Y max is 235
    0xFF, 0xFF, 0xFF, 0xFF   // U,V max is 240
};

// Calculate checksum for header validation
static uint32_t calculate_header_checksum(const struct frame_header *header) {
    uint32_t checksum = 0;
    const uint8_t *data = (const uint8_t *)header;
    // Skip the checksum field itself in calculation
    size_t size = sizeof(struct frame_header) - sizeof(uint32_t);
    
    for (size_t i = 0; i < size; i++) {
        checksum = (checksum << 8) ^ data[i];
    }
    return checksum;
}

static void pipe_frame(const AVFrame *frame, int64_t boot_time_ms) {
    // Calculate timestamp
    int64_t pts_ms = frame->pts / 1000;
    int64_t timestamp_ms = boot_time_ms + pts_ms;
    
    // Calculate frame size
    uint32_t frame_size = frame->width * frame->height;          // Y plane
    frame_size += (frame->width * frame->height) / 4;            // U plane
    frame_size += (frame->width * frame->height) / 4;            // V plane

    // Prepare header
    struct frame_header header = {
        .timestamp_ms = timestamp_ms,
        .width = frame->width,
        .height = frame->height,
        .frame_size = frame_size
    };
    memcpy(header.delimiter, FRAME_DELIMITER, sizeof(FRAME_DELIMITER));
    
    // Calculate and set checksum
    header.checksum = calculate_header_checksum(&header);

    // Write header
    if (fwrite(&header, sizeof(header), 1, stdout) != 1) {
        LOGE("Failed to write frame header");
        return;
    }

    // Write YUV420P frame data plane by plane
    // Y plane
    for (int i = 0; i < frame->height; i++) {
        if (fwrite(frame->data[0] + i * frame->linesize[0], 
                    frame->width, 1, stdout) != 1) {
            LOGE("Failed to write Y plane");
            return;
        }
    }

    // U plane
    for (int i = 0; i < frame->height/2; i++) {
        if (fwrite(frame->data[1] + i * frame->linesize[1], 
                    frame->width/2, 1, stdout) != 1) {
            LOGE("Failed to write U plane");
            return;
        }
    }

    // V plane
    for (int i = 0; i < frame->height/2; i++) {
        if (fwrite(frame->data[2] + i * frame->linesize[2], 
                    frame->width/2, 1, stdout) != 1) {
            LOGE("Failed to write V plane");
            return;
        }
    }

    fflush(stdout);
    return;
}

