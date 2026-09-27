#include "bot_client.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <cmath>
#include <charconv>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <thread>
#include <chrono>
#include <climits>
#include <vector>
#include <sstream>
#include <string_view>

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "utils.h"
#include "proc_util.h"
#include "sock_ipc.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/X.h>
#include <X11/extensions/shape.h>

extern char **environ;

// close_range(2) exists since Linux 5.9; older headers don't define it. On older
// kernels the call fails with ENOSYS and we fall back to closing descriptors in a loop.
#ifndef SYS_close_range
#define SYS_close_range 436
#endif

using flash_ipc::Message;
using flash_ipc::MessageType;

namespace
{
    constexpr const char *BROWSER_PATH = "lib/darkbot_browser_linux.AppImage";
    constexpr const char *DO_LIB_PATH = "lib/libdo_lib.so";

    // Don't relaunch a crashing browser more often than this.
    constexpr uint64_t RELAUNCH_BACKOFF_MS = 10'000;
    // Don't rescan /proc for the flash process more often than this.
    constexpr uint64_t FLASH_SCAN_INTERVAL_MS = 500;
    // Game thread not ticking for this long is reported as invalid, so the bot refreshes.
    constexpr uint64_t FLASH_FROZEN_MS = 60'000;

    // A browser younger than this is still starting (AppImage mount/extraction, window).
    constexpr uint64_t BROWSER_STARTUP_MS = 60'000;

    constexpr int BROWSER_SEND_TIMEOUT_MS = 250;
    constexpr int BROWSER_ACK_TIMEOUT_MS = 1500;

    inline uint64_t now_ms() { return flash_ipc::now_ms(); }

    // Logs at most once per |interval_ms| for a given call site.
    class RateLimit
    {
    public:
        explicit RateLimit(uint64_t interval_ms) : m_interval(interval_ms) { }
        bool Allow()
        {
            uint64_t now = now_ms();
            uint64_t last = m_last.load(std::memory_order_relaxed);
            if (last && now - last < m_interval)
                return false;
            return m_last.compare_exchange_strong(last, now);
        }
    private:
        uint64_t m_interval;
        std::atomic<uint64_t> m_last { 0 };
    };
}

namespace window
{
    // One persistent X connection for all input/window operations. Opening a connection
    // per event (as before) costs a socket connect + auth handshake every mouse action.
    std::mutex x_mutex;
    Display *x_display = nullptr;
    std::atomic<Display *> marker_display { nullptr }; // cursor marker connection
    Window browser_window = 0;
    std::atomic<XErrorHandler> previous_error_handler { nullptr };

    Atom atom_pid = None, atom_client_list = None, atom_wm_state = None;

    struct Property
    {
        Atom actual_type = None;
        int actual_format = 0;
        unsigned long nitems = 0;
        unsigned long bytes_after = 0;
        unsigned char *prop = nullptr;
    };

    /**
     * Ignores errors caused by our own requests (e.g. BadWindow when a window disappears
     * while we query it) and chains everything else to the previous handler (AWT or Xlib's).
     * Xlib's default handler calls exit(): without this a vanished window kills the JVM.
     */
    int error_handler(Display *display, XErrorEvent *event)
    {
        if (display == x_display || display == marker_display.load())
            return 0;
        XErrorHandler previous = previous_error_handler.load();
        return previous ? previous(display, event) : 0;
    }

    /**
     * The handler is process-global; AWT may install its own after us. Re-assert ours
     * (a pointer swap, no X round trip), chaining to whatever was installed.
     */
    void ensure_error_handler()
    {
        XErrorHandler current = XSetErrorHandler(error_handler);
        if (current != error_handler)
            previous_error_handler = current;
    }

    /**
     * Checks if X11 window control is available by verifying the DISPLAY environment variable.
     */
    bool x11_control_available()
    {
        const char *display = std::getenv("DISPLAY");
        return display && *display;
    }

    Display *get_display()
    {
        if (x_display)
            return x_display;

        if (!x11_control_available())
            return nullptr;

        static RateLimit open_fail_log(30'000);
        x_display = XOpenDisplay(nullptr);
        if (!x_display)
        {
            if (open_fail_log.Allow())
                utils::log("[X11] Failed to open display {}\n", std::getenv("DISPLAY"));
            return nullptr;
        }

        atom_pid = XInternAtom(x_display, "_NET_WM_PID", False);
        atom_client_list = XInternAtom(x_display, "_NET_CLIENT_LIST", False);
        atom_wm_state = XInternAtom(x_display, "WM_STATE", False);
        return x_display;
    }

    void reset_browser_window()
    {
        std::lock_guard<std::mutex> lock(x_mutex);
        browser_window = 0;
    }

    /**
     * Helper function to free the memory allocated by XGetWindowProperty and reset the WindowProperty structure.
     */
    void free_property(Property &property)
    {
        if (property.prop)
        {
            XFree(property.prop);
        }
        property = Property {};
    }

    /**
     * Helper function to get a window property with proper error handling and type checking.
     */
    bool get_property(Display *display,
                      Window window,
                      Atom property,
                      Atom type,
                      long length,
                      Property &out)
    {
        if (!display || property == None)
        {
            return false;
        }

        int status = XGetWindowProperty(display, window, property, 0, length, False, type,
                                        &out.actual_type,
                                        &out.actual_format,
                                        &out.nitems,
                                        &out.bytes_after,
                                        &out.prop);

        return status == Success;
    }

    /**
     * Helper function to get the PID of the process owning a window, using the _NET_WM_PID property.
     */
    bool get_pid(Display *display, Window window, pid_t &pid)
    {
        Property property;
        bool read_ok = get_property(display, window, atom_pid, XA_CARDINAL, 1, property);

        if (!read_ok || !property.prop || property.nitems == 0 || property.actual_format != 32)
        {
            free_property(property);
            return false;
        }

        pid = static_cast<pid_t>(*reinterpret_cast<unsigned long *>(property.prop));
        free_property(property);
        return true;
    }

    /**
     * Helper function to check if a given PID is the browser process or a child of it.
     */
    bool is_browser_pid(pid_t owner_pid, pid_t browser_pid)
    {
        return owner_pid == browser_pid || ProcUtil::IsChildOf(owner_pid, browser_pid);
    }

    /**
     * Helper function to attempt to get window attributes.
     */
    bool try_get_attrs(Display *display, Window window)
    {
        XWindowAttributes attrs;
        return XGetWindowAttributes(display, window, &attrs) != 0;
    }

    /**
     * Helper function to find the top-level root child of a given window,
     * which is likely the actual browser window we want to control.
     */
    Window find_toplevel_root_child(Display *display, Window root, Window window)
    {
        if (!window)
        {
            return 0;
        }

        Window current = window;
        while (current)
        {
            Window root_return = 0;
            Window parent_return = 0;
            Window *children = nullptr;
            unsigned int nchildren = 0;

            if (!XQueryTree(display, current, &root_return, &parent_return, &children, &nchildren))
            {
                return current;
            }

            if (children)
            {
                XFree(children);
            }

            if (parent_return == 0 || parent_return == root)
            {
                return current;
            }

            current = parent_return;
        }

        return window;
    }

    /**
     * Recursive helper function to find any descendant window owned by the browser process,
     * in case the top-level window doesn't have a PID or isn't directly owned by the browser.
     */
    Window find_browser_owned_descendant_recursive(Display *display, Window root, pid_t browser_pid, int depth = 0)
    {
        if (!root || depth > 32)
        {
            return 0;
        }

        pid_t owner_pid = -1;
        if (get_pid(display, root, owner_pid) && is_browser_pid(owner_pid, browser_pid))
        {
            return root;
        }

        Window root_return = 0;
        Window parent_return = 0;
        Window *children = nullptr;
        unsigned int nchildren = 0;

        if (!XQueryTree(display, root, &root_return, &parent_return, &children, &nchildren))
        {
            return 0;
        }

        Window found = 0;
        for (unsigned int i = 0; i < nchildren && !found; i++)
        {
            found = find_browser_owned_descendant_recursive(display, children[i], browser_pid, depth + 1);
        }

        if (children)
        {
            XFree(children);
        }
        return found;
    }

    /**
     * Main function to find the browser window by first checking the _NET_CLIENT_LIST for windows owned by the browser PID.
     */
    Window find_browser_client(Display *display, pid_t browser_pid)
    {
        Window root = DefaultRootWindow(display);
        if (atom_client_list != None)
        {
            Property property;
            bool read_ok = get_property(display, root, atom_client_list, XA_WINDOW, 4096, property);

            if (read_ok && property.prop && property.actual_type == XA_WINDOW)
            {
                Window *windows = reinterpret_cast<Window *>(property.prop);
                Window child_fallback = 0;
                for (unsigned long i = 0; i < property.nitems; i++)
                {
                    pid_t owner_pid = -1;
                    if (!get_pid(display, windows[i], owner_pid))
                    {
                        continue;
                    }

                    if (owner_pid == browser_pid)
                    {
                        Window found = windows[i];
                        free_property(property);
                        return found;
                    }

                    if (!child_fallback && ProcUtil::IsChildOf(owner_pid, browser_pid))
                    {
                        child_fallback = windows[i];
                    }
                }

                if (child_fallback)
                {
                    free_property(property);
                    return child_fallback;
                }
            }

            free_property(property);
        }

        Window any_owned = find_browser_owned_descendant_recursive(display, root, browser_pid);
        if (!any_owned)
        {
            return 0;
        }

        return find_toplevel_root_child(display, root, any_owned);
    }

    /**
     * Helper function to check if a window has the WM_STATE property, which is a strong indicator
     * that it's a top-level application window rather than a transient or child window.
     */
    bool has_wm_state(Display *display, Window window)
    {
        if (atom_wm_state == None)
        {
            return false;
        }

        Property property;
        bool read_ok = get_property(display, window, atom_wm_state, atom_wm_state, 2, property);
        bool has_state = read_ok && property.actual_type == atom_wm_state && property.nitems > 0;

        free_property(property);
        return has_state;
    }

    /**
     * Main function to resolve the actual client window of the browser,
     * which may involve checking the top-level window and its children
     */
    Window resolve_client(Display *display, pid_t browser_pid)
    {
        if (!display || browser_pid <= 0)
        {
            return 0;
        }

        Window window = find_browser_client(display, browser_pid);
        if (!window)
        {
            return 0;
        }

        if (has_wm_state(display, window))
        {
            return window;
        }

        Window root_return = 0;
        Window parent_return = 0;
        Window *children = nullptr;
        unsigned int nchildren = 0;

        if (!XQueryTree(display, window, &root_return, &parent_return, &children, &nchildren))
        {
            return window;
        }

        Window client = 0;
        for (unsigned int i = 0; i < nchildren; i++)
        {
            if (has_wm_state(display, children[i]))
            {
                client = children[i];
                break;
            }
        }

        if (children)
        {
            XFree(children);
        }

        return client ? client : window;
    }

    /**
     * Helper function to execute an action in the context of the browser window.
     */
    template<typename Func>
    bool with_browser(int flash_pid, int browser_pid, Func&& action)
    {
        if (flash_pid <= 0 || browser_pid <= 0)
            return false;

        std::lock_guard<std::mutex> lock(x_mutex);

        Display *display = get_display();
        if (!display)
            return false;

        ensure_error_handler();

        if (!browser_window || !try_get_attrs(display, browser_window))
            browser_window = resolve_client(display, browser_pid);

        bool result = false;
        if (browser_window)
            result = action(display, browser_window);

        XFlush(display);
        return result;
    }
}

namespace mouse
{
    struct EventContext
    {
        Display *display;
        Window window;
        Window root;
        int local_x, local_y;
        int root_x, root_y;
    };

    /**
     * Prepares the context for a mouse event by translating local coordinates to root coordinates.
     */
    bool prepare_event(Display *display, Window window, int32_t x, int32_t y, EventContext &ctx)
    {
        if (!display || !window)
        {
            return false;
        }

        XWindowAttributes attrs;
        if (XGetWindowAttributes(display, window, &attrs) == 0)
        {
            return false;
        }

        ctx.display = display;
        ctx.window = window;
        ctx.local_x = x;
        ctx.local_y = y;

        // Clamp coordinates to the window bounds to avoid unexpected behavior
        if (ctx.local_x < 0)
            ctx.local_x = 0;
        if (ctx.local_y < 0)
            ctx.local_y = 0;
        if (attrs.width > 0 && ctx.local_x >= attrs.width)
            ctx.local_x = attrs.width - 1;
        if (attrs.height > 0 && ctx.local_y >= attrs.height)
            ctx.local_y = attrs.height - 1;

        ctx.root = attrs.root;
        Window child = 0;
        if (!XTranslateCoordinates(display, window, ctx.root, 0, 0, &ctx.root_x, &ctx.root_y, &child))
        {
            ctx.root_x = attrs.x;
            ctx.root_y = attrs.y;
        }

        return true;
    }

    /**
     * Fills the common fields of an XEvent structure for mouse events, based on the provided context.
     */
    void fill_event_common(XEvent &event, const EventContext &ctx)
    {
        std::memset(&event, 0, sizeof(event));
        event.xany.display = ctx.display;
        event.xany.window = ctx.window;
        event.xbutton.root = ctx.root;
        event.xbutton.subwindow = None;
        event.xbutton.time = CurrentTime;
        event.xbutton.x = ctx.local_x;
        event.xbutton.y = ctx.local_y;
        event.xbutton.x_root = ctx.root_x + ctx.local_x;
        event.xbutton.y_root = ctx.root_y + ctx.local_y;
        event.xbutton.send_event = True;
        event.xbutton.same_screen = True;
    }

    /**
     * Sends a mouse move event.
     */
    bool send_move(Display *display, Window window, int32_t x, int32_t y)
    {
        EventContext ctx;
        if (!prepare_event(display, window, x, y, ctx))
            return false;

        XEvent event;
        fill_event_common(event, ctx);
        event.type = MotionNotify;

        return XSendEvent(ctx.display, ctx.window, True, PointerMotionMask, &event) != 0;
    }

    /**
     * Sends mouse button press/release events.
     */
    bool send_button(Display *display, Window window, int32_t x, int32_t y, int button, bool press, bool release)
    {
        EventContext ctx;
        if (!prepare_event(display, window, x, y, ctx))
            return false;

        XEvent event;
        fill_event_common(event, ctx);
        event.xbutton.button = button;

        bool ok = true;
        if (press)
        {
            event.type = ButtonPress;
            ok &= XSendEvent(ctx.display, ctx.window, True, ButtonPressMask, &event) != 0;
        }

        if (release)
        {
            event.type = ButtonRelease;
            ok &= XSendEvent(ctx.display, ctx.window, True, ButtonReleaseMask, &event) != 0;
        }
        return ok;
    }

    /**
     * Sends a mouse wheel event.
     */
    bool send_wheel(Display *display, Window window, int32_t x, int32_t y, int button)
    {
        return send_button(display, window, x, y, button, true, true);
    }
}

namespace cursor_marker
{
    static constexpr int dot_size = 6; // 6x6 marker size
    static constexpr const char *dot_color = "red";

    // State for the cursor marker, including whether it's enabled.
    struct State
    {
        std::atomic<bool> enabled { false };
        Display *display = nullptr;
        Window window = 0;
        Window parent = 0;
        std::chrono::steady_clock::time_point last_time;
        std::atomic<bool> clear_scheduled { false };
        std::mutex mutex;
    };

    static State state;

    /**
     * Destroys the cursor marker window and closes the display connection. Requires state.mutex.
     */
    void destroy()
    {
        if (state.display)
        {
            if (state.window)
                XDestroyWindow(state.display, state.window);
            XCloseDisplay(state.display);
            window::marker_display = nullptr;
        }
        state.window = 0;
        state.parent = 0;
        state.display = nullptr;
    }

    /**
     * Creates a small red window that will serve as a marker for the virtual cursor position.
     * This is useful for debugging and visualizing where the bot is "clicking" on the screen.
     * Requires state.mutex.
     */
    void create(Window parent)
    {
        if (!parent)
            return;

        destroy();

        state.display = XOpenDisplay(NULL);
        if (!state.display)
            return;
        window::marker_display = state.display;

        int scr = DefaultScreen(state.display);

        Colormap cmap = DefaultColormap(state.display, scr);
        XColor color;
        XColor exact;
        if (!XAllocNamedColor(state.display, cmap, dot_color, &color, &exact))
        {
            color.pixel = 0; // fallback black
        }

        XSetWindowAttributes attr;
        attr.background_pixel = color.pixel;
        attr.background_pixmap = None;

        unsigned long mask = CWBackPixel;
        state.window = XCreateWindow(
            state.display,
            parent,
            0, 0, dot_size, dot_size, 0,
            CopyFromParent,
            InputOutput,
            CopyFromParent,
            mask,
            &attr);
        state.parent = parent;

        // make the window input-transparent so it doesn't grab events
        int shape_event, shape_error;
        if (XShapeQueryExtension(state.display, &shape_event, &shape_error))
        {
            XRectangle rect = {0, 0, 0, 0};
            XShapeCombineRectangles(state.display,
                                    state.window,
                                    ShapeInput,
                                    0, 0,
                                    &rect,
                                    1,
                                    ShapeSet,
                                    Unsorted);
        }

        XMapRaised(state.display, state.window);
        XFlush(state.display);
    }

    /**
     * Hides the cursor marker after 3 seconds without updates. A single checker thread is
     * kept alive while updates keep coming instead of spawning one thread per mouse event.
     */
    void schedule_clear()
    {
        if (state.clear_scheduled.exchange(true))
            return;

        std::thread([]() {
            while (true)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                std::lock_guard<std::mutex> lock(state.mutex);
                if (!state.enabled || std::chrono::steady_clock::now() - state.last_time >= std::chrono::seconds(3))
                {
                    destroy();
                    state.clear_scheduled = false;
                    return;
                }
            }
        }).detach();
    }

    /**
     * Updates the position of the cursor marker to the given coordinates.
     */
    void update(int x, int y, int flash_pid, int browser_pid)
    {
        if (!state.enabled || flash_pid <= 0 || !window::x11_control_available())
            return;

        window::with_browser(flash_pid, browser_pid, [&](Display *, Window browser) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.last_time = std::chrono::steady_clock::now();

            if (!state.window || !state.display || state.parent != browser)
                create(browser);

            if (!state.window || !state.display)
                return false;

            const int offset = dot_size / 2;
            XMoveWindow(state.display, state.window, x - offset, y - offset);
            XMapRaised(state.display, state.window);
            XFlush(state.display);
            return true;
        });

        schedule_clear();
    }
}

BotClient::BotClient() : m_browser_ipc(new SockIpc()) {}

BotClient::~BotClient()
{
    Shutdown();
}

void BotClient::Shutdown()
{
    // runs during JVM shutdown: no new threads, no relaunch, nothing left behind
    std::lock_guard<std::recursive_mutex> lock(m_browser_mutex);
    m_want_browser = false;
    kill_browser(false);
}

/**
 * Continuously reads from the browser process's log output pipe
 * and logs any lines that contain the "[browser]" tag.
 */
static void browser_log_drain(int read_fd)
{
    static constexpr std::string_view browser_tag = "[browser]";
    char buf[4096];
    std::string partial;

    while (true)
    {
        ssize_t n = read(read_fd, buf, sizeof(buf));
        if (n == 0)
            break;

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }

        partial.append(buf, static_cast<size_t>(n));

        size_t scan_start = 0;
        size_t newline_pos = 0;
        while ((newline_pos = partial.find('\n', scan_start)) != std::string::npos)
        {
            const std::string_view line(partial.data() + scan_start, newline_pos - scan_start);
            if (line.find(browser_tag) != std::string_view::npos)
                utils::log("{}\n", std::string(line));

            scan_start = newline_pos + 1;
        }

        if (scan_start > 0)
            partial.erase(0, scan_start);

        // guard against a producer that never writes newlines
        if (partial.size() > 64 * 1024)
            partial.clear();
    }

    if (!partial.empty() && partial.find(browser_tag) != std::string::npos)
        utils::log("{}\n", partial);

    close(read_fd);
}

void BotClient::ToggleBrowserVisibility(bool visible)
{
    window::with_browser(FlashPid(), Pid(), [=](Display *display, Window browser) {
        visible ? XMapWindow(display, browser) : XUnmapWindow(display, browser);
        return true;
    });
}

static std::string shell_escape(const std::string &value)
{
    std::string escaped = "'";
    for (char c : value)
    {
        if (c == '\'')
            escaped += "'\\''";
        else
            escaped += c;
    }
    escaped += "'";
    return escaped;
}

static bool compute_sha256(const std::string &file_path, std::string &out_hash)
{
    std::string command = "sha256sum " + shell_escape(file_path);
    FILE *pipe = popen(command.c_str(), "re");
    if (!pipe)
        return false;

    char buffer[256];
    if (!fgets(buffer, sizeof(buffer), pipe))
    {
        pclose(pipe);
        return false;
    }

    int status = pclose(pipe);
    if (status != 0)
        return false;

    std::istringstream iss(buffer);
    if (!(iss >> out_hash))
        return false;

    if (out_hash.size() != 64)
        return false;

    for (unsigned char c : out_hash)
    {
        if (!std::isxdigit(c))
            return false;
    }

    std::transform(out_hash.begin(), out_hash.end(), out_hash.begin(), [](unsigned char c) { return std::tolower(c); });
    return true;
}

/**
 * Validates the file hash; the result is cached by (device, inode, size, mtime) so
 * relaunches after crashes don't re-hash the ~80MB AppImage every time.
 */
static bool validate_file_sha256(const std::string &file_path, const std::string &expected_hex)
{
    struct Cached
    {
        dev_t dev = 0;
        ino_t ino = 0;
        off_t size = -1;
        struct timespec mtime {};
        bool valid = false;
    };
    static Cached cached;

    if (expected_hex.empty())
        return false;

    struct stat st {};
    if (stat(file_path.c_str(), &st) != 0)
        return false;

    // cached either way: a bad file isn't re-hashed on every relaunch attempt (tick thread)
    if (cached.size == st.st_size && cached.dev == st.st_dev && cached.ino == st.st_ino
        && cached.mtime.tv_sec == st.st_mtim.tv_sec && cached.mtime.tv_nsec == st.st_mtim.tv_nsec)
        return cached.valid;

    std::string actual_hash;
    if (!compute_sha256(file_path, actual_hash))
        return false; // transient (e.g. popen failed), retry next time

    std::string normalized_expected = expected_hex;
    std::transform(normalized_expected.begin(), normalized_expected.end(), normalized_expected.begin(), [](unsigned char c) { return std::tolower(c); });

    cached = Cached { st.st_dev, st.st_ino, st.st_size, st.st_mtim, actual_hash == normalized_expected };
    return cached.valid;
}

/**
 * AppImages (type 2 runtime) mount themselves with FUSE 2. Recent distributions
 * (Ubuntu 22.04+, Mint 21+, Fedora, ...) don't ship libfuse.so.2 by default and
 * containers usually have no /dev/fuse; in that case run in extract-and-run mode.
 */
static bool appimage_fuse_available()
{
    if (access("/dev/fuse", R_OK | W_OK) != 0)
        return false;

    void *handle = dlopen("libfuse.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!handle)
        return false;
    dlclose(handle);
    return true;
}

static std::string absolute_path(const char *path)
{
    char resolved[PATH_MAX];
    if (realpath(path, resolved))
        return resolved;
    return path;
}

void BotClient::LaunchBrowser()
{
    std::lock_guard<std::recursive_mutex> lock(m_browser_mutex);

    if (browser_alive())
    {
        utils::log("[LaunchBrowser] browser already running ({})\n", Pid());
        return;
    }

    const char *fpath = BROWSER_PATH;
    static const std::string expected_sha256 = BROWSER_APPIMAGE_SHA256;

    m_want_browser = true; // keep it running from now on (retried by IsValid on failures)
    m_last_launch_ms = now_ms();

    /* ensure the browser binary exists and is executable before attempting to fork/exec */
    if (access(fpath, F_OK) != 0)
    {
        utils::log("[LaunchBrowser] browser binary not found: {}\n", fpath);
        return;
    }
    if (!validate_file_sha256(fpath, expected_sha256))
    {
        utils::log("[LaunchBrowser] browser binary SHA256 validation failed: {}\n", fpath);
        return;
    }
    if (access(fpath, X_OK) != 0)
    {
        mode_t mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
        if (chmod(fpath, mode) != 0)
        {
            utils::log("[LaunchBrowser] browser binary not executable and chmod failed: {} ({})\n", fpath, std::strerror(errno));
            return;
        }
    }

    // Everything is prepared before fork(): the JVM is multi-threaded, so between fork and
    // exec the child may only call async-signal-safe functions (no malloc, no locks).
    std::string url = m_url;
    std::string sid = m_sid;

    while (!url.empty() && url.back() == '/')
        url.pop_back();

    if (sid.rfind("dosid=", 0) == 0)
        sid.erase(0, 6);

    static std::atomic<uint32_t> launch_counter { 0 };
    m_browser_ipc_path = utils::format("/tmp/darkbot_ipc_{}_{}", getpid(), ++launch_counter);
    unlink(m_browser_ipc_path.c_str());

    std::vector<std::string> args {
        fpath,
        "--sid=" + sid,
        "--url=" + url,
        "--api-version=" + std::to_string(API_VERSION),
        "--ipc-path=" + m_browser_ipc_path,
        "--parent-pid=" + std::to_string(getpid()), // browser exits if the bot dies
        "--launch",
        "--no-sandbox", // required on distros restricting unprivileged user namespaces (Ubuntu 24.04+)
        "--ozone-platform=x11",
        "--disable-background-timer-throttling",
        "--disable-renderer-backgrounding",
        "--disable-backgrounding-occluded-windows",
    };

    std::vector<std::string> env;
    // ld.so splits LD_PRELOAD on spaces and colons without any escaping: an install path
    // like "/home/u/My Bots/..." would silently not load do_lib. The browser inherits our
    // working directory, so the relative path works there.
    std::string do_lib = absolute_path(DO_LIB_PATH);
    if (do_lib.find_first_of(" :") != std::string::npos)
        do_lib = DO_LIB_PATH;
    std::string preload = "LD_PRELOAD=" + do_lib;
    for (int i = 0; environ[i]; i++)
    {
        std::string_view entry(environ[i]);
        if (entry.rfind("LD_PRELOAD=", 0) == 0)
        {
            // keep libraries the user preloads (e.g. gtk3-nocsd on Mint), ours goes first;
            // sanitizer runtimes (debug builds of the bot) would break Electron
            std::string_view libs = entry.substr(11);
            while (!libs.empty())
            {
                size_t end = libs.find_first_of(" :");
                std::string_view lib = libs.substr(0, end);
                if (!lib.empty() && lib.find("libasan") == std::string_view::npos
                    && lib.find("libtsan") == std::string_view::npos && lib.find("libubsan") == std::string_view::npos)
                    preload.append(" ").append(lib);
                if (end == std::string_view::npos)
                    break;
                libs.remove_prefix(end + 1);
            }
            continue;
        }
        if (entry.rfind("APPIMAGE_EXTRACT_AND_RUN=", 0) == 0)
            continue;
        env.emplace_back(entry);
    }
    env.push_back(preload);

    m_launched_with_fuse = !m_force_extract_and_run && appimage_fuse_available();
    if (!m_launched_with_fuse)
    {
        utils::log("[LaunchBrowser] FUSE 2 {}, using AppImage extract-and-run mode\n",
                   m_force_extract_and_run ? "mount failed before" : "unavailable");
        env.emplace_back("APPIMAGE_EXTRACT_AND_RUN=1");
    }

    std::vector<char *> argv, envp;
    for (auto &a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    for (auto &e : env) envp.push_back(e.data());
    envp.push_back(nullptr);

    int log_pipe[2] = {-1, -1};
    if (pipe2(log_pipe, O_CLOEXEC) != 0)
    {
        utils::log("[LaunchBrowser] pipe() failed: {}\n", std::strerror(errno));
        log_pipe[0] = log_pipe[1] = -1;
    }

    int dev_null = open("/dev/null", O_RDONLY | O_CLOEXEC);

    struct rlimit rl {};
    int max_fd = (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        ? static_cast<int>(std::min<rlim_t>(rl.rlim_cur, 65536)) : 4096;

    sigset_t empty_mask;
    sigemptyset(&empty_mask);

    pid_t pid = fork();

    switch (pid)
    {
        case -1: // https://rachelbythebay.com/w/2014/08/19/fork/
        {
            utils::log("[LaunchBrowser] fork failed: {}\n", std::strerror(errno));
            if (log_pipe[0] != -1) { close(log_pipe[0]); close(log_pipe[1]); }
            break;
        }
        case 0:
        {
            // --- child: async-signal-safe calls only ---

            // own session/process group: the whole browser tree can be killed at once even
            // after the AppImage runtime died and its children got re-parented to init
            setsid();

            if (dev_null != -1)
                dup2(dev_null, STDIN_FILENO);

            // redirect browser stdout/stderr into the pipe so the parent can log them
            if (log_pipe[1] != -1)
            {
                dup2(log_pipe[1], STDOUT_FILENO);
                dup2(log_pipe[1], STDERR_FILENO);
            }

            // don't leak JVM file descriptors (sockets, jars, ...) into the browser
            if (syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
            {
                for (int fd = 3; fd < max_fd; fd++)
                    close(fd);
            }

            // the forking JVM thread may have signals blocked; exec keeps the mask
            sigprocmask(SIG_SETMASK, &empty_mask, nullptr);
            signal(SIGPIPE, SIG_DFL);

            execve(fpath, argv.data(), envp.data());
            _exit(127);
        }
        default:
        {
            SetPid(pid);
            utils::log("[LaunchBrowser] browser started, pid {}\n", pid);

            // close write end in parent; read end passed to drain thread
            if (log_pipe[1] != -1) close(log_pipe[1]);
            if (log_pipe[0] != -1)
            {
                int read_fd = log_pipe[0];
                std::thread([read_fd]() { browser_log_drain(read_fd); }).detach();
            }
            break;
        }
    }

    if (dev_null != -1)
        close(dev_null);
}

/**
 * Checks if our browser child is alive, reaping it if it exited (no global SIGCHLD
 * handler: that would steal exit statuses of processes spawned by the JVM).
 */
bool BotClient::browser_alive()
{
    int pid = Pid();
    if (pid <= 0)
        return false;

    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == 0)
        return true;

    if (r == pid)
    {
        // The AppImage runtime exits with 127 right away when it can't mount itself
        // (libfuse2 present but fusermount unusable, containers, hardened systems):
        // use extract-and-run from now on and relaunch without waiting for the backoff.
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127 && m_launched_with_fuse
            && now_ms() - m_last_launch_ms < 15'000 && !m_force_extract_and_run)
        {
            utils::log("[Browser] AppImage failed to mount with FUSE, switching to extract-and-run\n");
            m_force_extract_and_run = true;
            m_last_launch_ms = 0;
        }

        if (WIFEXITED(status))
            utils::log("[Browser] process {} exited with code {}\n", pid, WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            utils::log("[Browser] process {} killed by signal {}\n", pid, WTERMSIG(status));

        // Kill what's left of its process group right away (a pgid isn't reused while it
        // has members) and forget the pid: once reaped it may be reused by another process.
        kill(-pid, SIGKILL);
        SetPid(-1);
        m_browser_ipc->Close();
        return false;
    }

    // ECHILD: not our child (shouldn't happen), fall back to a liveness probe
    return ProcUtil::ProcessExists(pid);
}

void BotClient::kill_browser(bool reap_async)
{
    int pid = Pid();
    if (pid > 0)
    {
        // Kill the whole tree: in extract-and-run mode the direct child is only the AppImage
        // runtime, and orphaned flash processes would keep the game session alive.
        auto descendants = ProcUtil::GetDescendants(pid);
        kill(-pid, SIGKILL); // process group created by setsid() in LaunchBrowser
        kill(pid, SIGKILL);
        for (pid_t child : descendants)
            kill(child, SIGKILL);

        // reap without blocking the caller
        if (reap_async)
            std::thread([pid]() { waitpid(pid, nullptr, 0); }).detach();
    }

    SetPid(-1);
    m_last_valid = false;
    m_browser_ipc->Close();

    if (!m_browser_ipc_path.empty())
        unlink(m_browser_ipc_path.c_str());
}

void BotClient::restart_browser(const char *reason)
{
    std::lock_guard<std::recursive_mutex> lock(m_browser_mutex);

    utils::log("[Browser] restarting: {}\n", reason);
    kill_browser();
    reset();
    window::reset_browser_window();
    LaunchBrowser();
}

void BotClient::Refresh()
{
    std::lock_guard<std::recursive_mutex> lock(m_browser_mutex);

    utils::log("[Refresh] Triggering browser refresh\n");

    const uint64_t launch_before = m_last_launch_ms;
    if (!SendBrowserCommand("refresh"))
    {
        // SendBrowserCommand may already have relaunched a dead browser, or the browser is
        // alive but still starting (socket/window not ready yet): don't kill it mid-startup,
        // it's loading the game anyway
        bool starting = browser_alive() && now_ms() - m_last_launch_ms < BROWSER_STARTUP_MS;
        if (m_last_launch_ms == launch_before && !starting)
            restart_browser("refresh command failed");
        else
            reset();
        return;
    }

    // if there's an existing flash process, kill it so we don't keep
    // reusing the same PID after a refresh.
    if (FlashPid() > 0)
        kill(FlashPid(), SIGKILL);

    reset();
}

// helper used within SendBrowserCommand; returns true when the IPC
// connection is ready (either already connected or successfully created).
bool BotClient::ensure_browser_ipc_connected()
{
    if (m_browser_ipc->Connected())
        return true;

    if (Pid() <= 0 || m_browser_ipc_path.empty())
        return false;

    if (!m_browser_ipc->Connect(m_browser_ipc_path))
    {
        static RateLimit log_limit(10'000);
        if (log_limit.Allow())
            utils::log("[SendBrowserCommand] Failed to connect to browser {} ({})\n", Pid(), m_browser_ipc_path);
        return false;
    }
    return true;
}

/**
 * Builds a single-line JSON string for the given command and parameters.
 */
static std::string build_browser_command_json(uint32_t id, const std::string &cmd, std::initializer_list<JsonParam> params)
{
    size_t reserve_size = cmd.size() + 32;
    for (const JsonParam &param : params)
    {
        reserve_size += 4 + std::strlen(param.key) + param.value.size();
    }

    std::string json;
    json.reserve(reserve_size);
    json.append("{\"id\":");
    json.append(std::to_string(id));
    json.append(",\"cmd\":\"");
    json.append(cmd);
    json.push_back('"');

    for (const JsonParam &param : params)
    {
        json.append(",\"");
        json.append(param.key);
        json.append("\":");
        json.append(param.value.data(), param.value.size());
    }

    json.push_back('}');
    return json;
}

/**
 * Sends a command to the browser process via IPC and waits for its acknowledgment.
 * Params format: {"arg1": "value1", "arg2": "value2"} which gets converted to JSON and sent to the browser.
 * Values must already be valid JSON (numbers or escaped strings).
 */
bool BotClient::SendBrowserCommand(const std::string &cmd, std::initializer_list<JsonParam> params)
{
    std::lock_guard<std::recursive_mutex> lock(m_browser_mutex);

    if (!browser_alive())
    {
        maybe_relaunch_browser();
        return false;
    }

    if (!ensure_browser_ipc_connected())
    {
        return false;
    }

    const uint32_t id = ++m_browser_cmd_id;
    const std::string json = build_browser_command_json(id, cmd, params);
    const std::string id_prefix = std::to_string(id) + "|";

    // Only resend when the write itself failed (the browser never saw the command);
    // resending after a missing ack could duplicate key presses.
    bool sent = false;
    for (int attempt = 0; attempt < 2 && !sent; ++attempt)
    {
        sent = m_browser_ipc->Send(json, BROWSER_SEND_TIMEOUT_MS);
        if (!sent && !ensure_browser_ipc_connected())
            break;
    }

    if (!sent)
    {
        utils::log("[SendBrowserCommand] send failed for '{}'\n", cmd);
        return false;
    }

    // replies are "<id>|ok" / "<id>|err"; skip stale replies of earlier timed-out commands
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(BROWSER_ACK_TIMEOUT_MS);
    std::string line;
    while (true)
    {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0 || !m_browser_ipc->RecvLine(line, static_cast<int>(left)))
            break;

        if (line.rfind(id_prefix, 0) == 0)
        {
            if (line.compare(id_prefix.size(), std::string::npos, "ok") == 0)
                return true;

            utils::log("[SendBrowserCommand] browser rejected '{}': {}\n", cmd, line);
            return false;
        }
    }

    utils::log("[SendBrowserCommand] no ack for '{}'\n", json);
    return false;
}

bool BotClient::find_flash_process()
{
    // Also called from memory search / flash calls on the bot's tick thread: don't wait
    // behind another thread that's busy with the browser (restart, command ack).
    std::unique_lock<std::recursive_mutex> lock(m_browser_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return false;

    if (FlashPid() > 0)
        return true;

    int browser = Pid();
    if (browser <= 0)
        return false;

    uint64_t now = now_ms();
    if (now - m_last_flash_scan_ms < FLASH_SCAN_INTERVAL_MS)
        return false;
    m_last_flash_scan_ms = now;

    int best_pid = -1;
    uint64_t best_memory = 0;

    for (pid_t proc_pid : ProcUtil::GetDescendants(browser))
    {
        // the pepper plugin process runs with --type=ppapi (the broker also matches,
        // but it doesn't map the flash library or is much smaller)
        if (ProcUtil::GetCmdline(proc_pid).find("--type=ppapi") == std::string::npos)
            continue;

        if (!ProcUtil::HasMapping(proc_pid, "libpepflashplayer"))
            continue;

        // Search for the flash process with the most memory usage,
        // since the browser can spawn multiple and we want to target the main one
        uint64_t memory = ProcUtil::GetMemoryUsage(proc_pid);
        if (memory >= best_memory)
        {
            best_memory = memory;
            best_pid = proc_pid;
        }
    }

    if (best_pid > 0)
    {
        utils::log("[Flash] found flash process {}\n", best_pid);
        SetFlashPid(best_pid);
        return true;
    }

    return false;
}

void BotClient::reset()
{
    m_last_valid = false;
    // the old flash process is gone (or being killed): drop its shared memory segment
    m_flash_ipc.Reset(FlashPid(), true);
    SetFlashPid(-1);
    m_last_flash_scan_ms = 0;
}

/**
 * Relaunches the browser if it should be running but isn't (crashed, or a previous launch
 * failed), rate limited so a browser that can't start doesn't respawn in a tight loop.
 * Returns true if a launch was attempted.
 */
bool BotClient::maybe_relaunch_browser()
{
    if (!m_want_browser || now_ms() - m_last_launch_ms < RELAUNCH_BACKOFF_MS)
        return false;
    restart_browser("browser process not running");
    return true;
}

// Not a great name since it has side-effects like refreshing or restarting the browser
bool BotClient::IsValid()
{
    // Called every tick by the bot's main loop: if another thread is busy with the browser
    // (e.g. waiting for a command ack), answer with the last result instead of blocking.
    std::unique_lock<std::recursive_mutex> lock(m_browser_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return m_last_valid;

    m_last_valid = check_valid();
    return m_last_valid;
}

bool BotClient::check_valid()
{

    if (!browser_alive())
    {
        maybe_relaunch_browser();
        return false;
    }

    if (FlashPid() <= 0)
    {
        return find_flash_process();
    }

    if (!ProcUtil::ProcessExists(FlashPid()))
    {
        utils::log("[IsValid] Flash process not found, trying to refresh {}, {}\n", FlashPid(), Pid());
        Refresh();
        return false;
    }

    // The game thread ticks do_lib's timer hook; if it stops for a long time the game is
    // frozen (or lost its hooks) and every direct call would fail. Reporting it as invalid
    // makes the bot run its normal stuck-recovery (refresh).
    return !m_flash_ipc.IsFrozen(FlashPid(), FLASH_FROZEN_MS);
}

/**
 * Sends a command message to the flash process through shared memory (see tools/flash_ipc.h)
 * and optionally copies back the response.
 */
bool BotClient::SendFlashCommand(const Message &message, Message *response)
{
    if (FlashPid() <= 0 && !find_flash_process())
    {
        return false;
    }

    return m_flash_ipc.Send(FlashPid(), message, response);
}

bool BotClient::SendNotification(uintptr_t screen_manager, const std::string &name, const std::vector<uintptr_t> &args)
{
    Message message;
    message.notify = {};
    size_t cap = std::size(message.notify.argv);
    size_t to_copy = std::min(args.size(), cap);
    message.notify.argc = static_cast<uint32_t>(to_copy);
    if (to_copy)
        std::memcpy(message.notify.argv, args.data(), to_copy * sizeof(message.notify.argv[0]));
    std::strncpy(message.notify.name, name.c_str(), sizeof(message.notify.name) - 1);
    return SendFlashCommand(message);
}

bool BotClient::RefineOre(uintptr_t refine_util, uint32_t ore, uint32_t amount)
{
    Message message;
    message.refine = {};
    message.refine.refine_util = refine_util;
    message.refine.ore = static_cast<int>(ore);
    message.refine.amount = static_cast<int>(amount);

    return SendFlashCommand(message);
}

bool BotClient::UseItem(const std::string &name, uint8_t type, uint8_t bar)
{
    Message message;
    message.item = {};
    message.item.action_type = type;
    message.item.action_bar = bar;
    std::strncpy(message.item.name, name.c_str(), sizeof(message.item.name) - 1);
    return SendFlashCommand(message);
}

uintptr_t BotClient::CallMethod(uintptr_t obj, uint32_t index, const std::vector<uintptr_t> &args)
{
    Message message;
    message.call = {};
    message.call.object = obj;
    message.call.index = index;
    size_t to_copy = std::min(args.size(), std::size(message.call.argv));
    message.call.argc = static_cast<int>(to_copy);
    if (to_copy)
        std::memcpy(message.call.argv, args.data(), to_copy * sizeof(uintptr_t));

    Message response;
    if (!SendFlashCommand(message, &response) || response.result.error)
        return 0;

    return response.result.value;
}

/**
 * Sends a key click event to the flash process via shared memory.
 *
 * Note: may not work properly for some game actions.
 */
bool BotClient::KeyClickLegacy(uint32_t key)
{
    Message message;
    message.key = {};
    message.key.key = key;
    return SendFlashCommand(message);
}

void BotClient::KeyClick(uint32_t key)
{
    // First try sending key click via browser command
    // If failed, then send via legacy flash IPC method
    if (!SendBrowserCommand("keyClick", {{"key", std::to_string(key)}}))
        KeyClickLegacy(key);
}

void BotClient::KeyDown(uint32_t key)
{
    SendBrowserCommand("keyDown", {{"key", std::to_string(key)}});
}

void BotClient::KeyUp(uint32_t key)
{
    SendBrowserCommand("keyUp", {{"key", std::to_string(key)}});
}

void BotClient::SendText(const std::string &text)
{
    SendBrowserCommand("text", {{"text", utils::escape_json(text)}});
}

/**
 * Sends a mouse click event to the flash process via shared memory,
 * used when X11 control is unavailable.
 *
 * Note: may not work properly for some game actions.
 */
bool BotClient::MouseClickLegacy(int32_t x, int32_t y)
{
    Message message;
    message.click = {};
    message.click.x = x;
    message.click.y = y;
    message.click.button = 1;
    return SendFlashCommand(message);
}

void BotClient::MouseClick(int32_t x, int32_t y)
{
    // First try sending click via X11 for better compatibility with all game actions
    bool success = window::with_browser(FlashPid(), Pid(), [=](Display *display, Window browser) {
        return mouse::send_button(display, browser, x, y, Button1, true, true);
    });

    // If X11 method failed, fall back to legacy flash IPC method.
    if (!success)
        success = MouseClickLegacy(x, y);

    if (success)
        UpdateCursorMarker(x, y);
}

void BotClient::MouseMove(int32_t x, int32_t y)
{
    bool success = window::with_browser(FlashPid(), Pid(), [=](Display *display, Window browser) {
        return mouse::send_move(display, browser, x, y);
    });

    if (success)
        UpdateCursorMarker(x, y);
}

void BotClient::MouseDown(int32_t x, int32_t y)
{
    bool success = window::with_browser(FlashPid(), Pid(), [=](Display *display, Window browser) {
        return mouse::send_button(display, browser, x, y, Button1, true, false);
    });

    if (success)
        UpdateCursorMarker(x, y);
}

void BotClient::MouseUp(int32_t x, int32_t y)
{
    bool success = window::with_browser(FlashPid(), Pid(), [=](Display *display, Window browser) {
        return mouse::send_button(display, browser, x, y, Button1, false, true);
    });

    if (success)
        UpdateCursorMarker(x, y);
}

void BotClient::MouseScroll(int32_t x, int32_t y, int32_t delta)
{
    int button = delta >= 0 ? Button4 : Button5;
    bool success = window::with_browser(FlashPid(), Pid(), [=](Display *display, Window browser) {
        return mouse::send_wheel(display, browser, x, y, button);
    });

    if (success)
        UpdateCursorMarker(x, y);
}

// process a batch of encoded native actions.
void BotClient::PostActions(const std::vector<uint64_t> &actions)
{
    std::lock_guard<std::mutex> lock(m_post_actions_mutex);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);

    for (size_t i = 0; i < actions.size(); ++i)
    {
        if (std::chrono::steady_clock::now() >= deadline)
            break;

        uint64_t value = actions[i];
        uint16_t message = static_cast<uint16_t>((value >> 48) & 0x7fff);
        int16_t wparam = static_cast<int16_t>((value >> 32) & 0xffff);
        int16_t lparam_low = static_cast<int16_t>(value & 0xffff);
        int16_t lparam_high = static_cast<int16_t>((value >> 16) & 0xffff);

        int32_t x = static_cast<int32_t>(lparam_low);
        int32_t y = static_cast<int32_t>(lparam_high);
        uint32_t key = static_cast<uint16_t>(wparam);

        // Handle native mouse and keyboard events based on the message type.
        // https://github.com/darkbot-reloaded/DarkBot/blob/master/src/main/java/eu/darkbot/api/utils/NativeAction.java

        switch (message)
        {
            case 0x1FF: // Mouse CLICK
                MouseClick(x, y);
                break;
            case 0x200: // Mouse MOVE
                MouseMove(x, y);
                break;
            case 0x201: // Mouse DOWN
                MouseDown(x, y);
                break;
            case 0x202: // Mouse UP
                MouseUp(x, y);
                break;
            case 0x20A: // Mouse WHEEL
                MouseScroll(x, y, wparam);
                break;
            case 0x1FE: // Key CLICK
                KeyClick(key);
                break;
            case 0x100: // Key DOWN
                KeyDown(key);
                break;
            case 0x101: // Key UP
                KeyUp(key);
                break;
            case 0x102: // Key CHAR
                {
                    std::string text(1, static_cast<char>(wparam));
                    SendText(text);
                }
                break;
            default:
                // unsupported message, ignore
                break;
        }
        // small delay between actions
        if (i + 1 < actions.size())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// paste a string to the game, optionally performing native actions before/after
void BotClient::PasteText(const std::string &text, const std::vector<uint64_t> &actions)
{
    // split inline vector into before/after lists using high-bit flag
    const uint64_t AFTER_MASK = (1ULL << 63);
    std::vector<uint64_t> before;
    std::vector<uint64_t> after;
    for (uint64_t v : actions) {
        if (v & AFTER_MASK)
            after.push_back(v);
        else
            before.push_back(v);
    }

    {
        std::lock_guard<std::mutex> lock(m_paste_mutex);
        m_paste_queue.push({std::move(before), text, std::move(after)});
    }

    // start worker thread once
    if (!m_paste_worker_running.exchange(true)) {
        std::thread([this]() {
            while (true) {
                std::tuple<std::vector<uint64_t>, std::string, std::vector<uint64_t>> item;
                {
                    std::lock_guard<std::mutex> lock(m_paste_mutex);
                    if (m_paste_queue.empty())
                    {
                        // cleared under the lock so a concurrent PasteText either sees
                        // the worker running (and its item gets picked up) or starts a new one
                        m_paste_worker_running = false;
                        break;
                    }
                    item = std::move(m_paste_queue.front());
                    m_paste_queue.pop();
                }

                auto &[before_actions, str, after_actions] = item;

                if (!before_actions.empty())
                {
                    PostActions(before_actions);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }

                SendText(str);
                // the browser types one character every 10 ms (key_handler.js handleText);
                // wait until it's done so e.g. an Enter "after" action doesn't cut the text
                std::this_thread::sleep_for(std::chrono::milliseconds(std::max<size_t>(750, 250 + str.size() * 12)));

                if (!after_actions.empty())
                {
                    PostActions(after_actions);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            }
        }).detach();
    }
}

int BotClient::CheckMethodSignature(uintptr_t object, uint32_t index, bool check_name, const std::string &sig)
{
    Message message;
    message.sig = {};
    message.sig.object = object;
    message.sig.index = index;
    message.sig.method_name = check_name;
    message.sig.result = -1;

    std::strncpy(message.sig.signature, sig.c_str(), sizeof(message.sig.signature) - 1);

    Message response;
    // -1 on any transport failure: the Java side treats only 0 as "invalid signature"
    // (which stops the bot), so it must never see garbage here
    if (!SendFlashCommand(message, &response))
        return -1;

    return response.sig.result;
}

void BotClient::EnableCursorMarker(bool enable)
{
    if (cursor_marker::state.enabled.exchange(enable) == enable)
        return;

    if (!enable)
    {
        std::lock_guard<std::mutex> lock(cursor_marker::state.mutex);
        cursor_marker::destroy();
    }
}

void BotClient::UpdateCursorMarker(int32_t x, int32_t y)
{
    cursor_marker::update(x, y, FlashPid(), Pid());
}
