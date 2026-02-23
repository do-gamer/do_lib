#include "bot_client.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <thread>
#include <chrono>

#include "utils.h"
#include "proc_util.h"
#include "sock_ipc.h"

#include <signal.h>
#include <sys/uio.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/wait.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/X.h>
#include <X11/extensions/shape.h>


#define MEM_SIZE 1024

namespace
{
    Window browser_window = 0;

    struct WindowProperty
    {
        Atom actual_type = None;
        int actual_format = 0;
        unsigned long nitems = 0;
        unsigned long bytes_after = 0;
        unsigned char *prop = nullptr;
    };

    /**
     * Helper function to free the memory allocated by XGetWindowProperty and reset the WindowProperty structure.
     */
    void free_window_property(WindowProperty &prop)
    {
        if (prop.prop)
        {
            XFree(prop.prop);
        }
        prop.actual_type = None;
        prop.actual_format = 0;
        prop.nitems = 0;
        prop.bytes_after = 0;
        prop.prop = nullptr;
    }

    /**
     * Helper function to get a window property with proper error handling and type checking.
     */
    bool get_window_property(Display *display,
                             Window window,
                             Atom property,
                             Atom type,
                             long length,
                             WindowProperty &out)
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
    bool get_window_pid(Display *display, Window window, pid_t &pid)
    {
        Atom atom_pid = XInternAtom(display, "_NET_WM_PID", True);
        if (atom_pid == None)
        {
            return false;
        }

        WindowProperty prop;
        bool property_ok = get_window_property(display, window, atom_pid, XA_CARDINAL, 1, prop);

        if (!property_ok || !prop.prop || prop.nitems == 0)
        {
            free_window_property(prop);
            return false;
        }

        pid = static_cast<pid_t>(*reinterpret_cast<unsigned long *>(prop.prop));
        free_window_property(prop);
        return true;
    }

    /**
     * Helper function to check if a given PID is the browser process or a child of it.
     */
    bool is_browser_window_pid(pid_t owner_pid, pid_t browser_pid)
    {
        return owner_pid == browser_pid || ProcUtil::IsChildOf(owner_pid, browser_pid);
    }

    /**
     * Checks if X11 window control is available by verifying the DISPLAY environment variable.
     */
    bool x11_window_control_available()
    {
        const char *display = std::getenv("DISPLAY");
        return display && *display;
    }

    /**
     * Helper function to attempt to get window attributes, handling potential X11 errors gracefully.
     */
    bool try_get_window_attrs(Display *display, Window window)
    {
        XWindowAttributes attrs;
        if (XGetWindowAttributes(display, window, &attrs) != 0)
        {
            return true;
        }

        XSync(display, False);
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
    Window find_browser_owned_descendant_recursive(Display *display, Window root, pid_t browser_pid)
    {
        if (!root)
        {
            return 0;
        }

        pid_t owner_pid = -1;
        if (get_window_pid(display, root, owner_pid) && is_browser_window_pid(owner_pid, browser_pid))
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
            found = find_browser_owned_descendant_recursive(display, children[i], browser_pid);
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
    Window find_browser_client_window(Display *display, pid_t browser_pid)
    {
        Window root = DefaultRootWindow(display);
        Atom atom_client_list = XInternAtom(display, "_NET_CLIENT_LIST", True);
        if (atom_client_list != None)
        {
            WindowProperty prop;
            bool property_ok = get_window_property(display, root, atom_client_list, XA_WINDOW, 4096, prop);

            if (property_ok && prop.prop && prop.actual_type == XA_WINDOW)
            {
                Window *windows = reinterpret_cast<Window *>(prop.prop);
                Window child_fallback = 0;
                for (unsigned long i = 0; i < prop.nitems; i++)
                {
                    pid_t owner_pid = -1;
                    if (!get_window_pid(display, windows[i], owner_pid))
                    {
                        continue;
                    }

                    if (owner_pid == browser_pid)
                    {
                        free_window_property(prop);
                        return windows[i];
                    }

                    if (!child_fallback && ProcUtil::IsChildOf(owner_pid, browser_pid))
                    {
                        child_fallback = windows[i];
                    }
                }

                if (child_fallback)
                {
                    free_window_property(prop);
                    return child_fallback;
                }
            }

            free_window_property(prop);
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
        Atom wm_state = XInternAtom(display, "WM_STATE", True);
        if (wm_state == None)
        {
            return false;
        }

        WindowProperty prop;
        bool property_ok = get_window_property(display, window, wm_state, wm_state, 2, prop);
        bool has_state = property_ok && prop.actual_type == wm_state && prop.nitems > 0;

        free_window_property(prop);
        return has_state;
    }

    /**
     * Main function to resolve the actual client window of the browser,
     * which may involve checking the top-level window and its children
     */
    Window resolve_client_window(Display *display, pid_t browser_pid)
    {
        if (!display)
        {
            return 0;
        }

        Window window = find_browser_client_window(display, browser_pid);
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
    void with_browser_window(int flash_pid, int browser_pid, Func&& action)
    {
        if (flash_pid == -1 || !x11_window_control_available())
        {
            return;
        }

        Display *display = XOpenDisplay(nullptr);
        if (!display)
        {
            return;
        }

        if (!browser_window || !try_get_window_attrs(display, browser_window))
        {
            browser_window = resolve_client_window(display, browser_pid);
        }

        if (browser_window)
        {
            action(display, browser_window);
        }

        XFlush(display);
        XCloseDisplay(display);
    }

    struct MouseEventContext
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
    bool prepare_mouse_event(Display *display, Window window, int32_t x, int32_t y, MouseEventContext &ctx)
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

        ctx.root = DefaultRootWindow(display);
        Window child = 0;
        XTranslateCoordinates(display, window, ctx.root, 0, 0, &ctx.root_x, &ctx.root_y, &child);

        return true;
    }

    /**
     * Fills the common fields of an XEvent structure for mouse events, based on the provided context.
     */
    void fill_mouse_event_common(XEvent &event, const MouseEventContext &ctx)
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
        event.xbutton.same_screen = True;
    }

    /**
     * Sends a mouse move event.
     */
    void send_mouse_move(Display *display, Window window, int32_t x, int32_t y)
    {
        MouseEventContext ctx;
        if (!prepare_mouse_event(display, window, x, y, ctx))
            return;

        XEvent event;
        fill_mouse_event_common(event, ctx);
        event.xmotion.type = MotionNotify;

        XSendEvent(ctx.display, ctx.window, True, PointerMotionMask, &event);
    }

    /**
     * Sends mouse button press/release events.
     */
    void send_mouse_button(Display *display, Window window, int32_t x, int32_t y, int button, bool press, bool release)
    {
        MouseEventContext ctx;
        if (!prepare_mouse_event(display, window, x, y, ctx))
            return;

        XEvent event;
        fill_mouse_event_common(event, ctx);
        event.xbutton.button = button;

        if (press)
        {
            event.xbutton.type = ButtonPress;
            XSendEvent(ctx.display, ctx.window, True, ButtonPressMask, &event);
        }

        if (release)
        {
            event.xbutton.type = ButtonRelease;
            XSendEvent(ctx.display, ctx.window, True, ButtonReleaseMask, &event);
        }
    }

    /**
     * Sends a mouse wheel event.
     */
    void send_mouse_wheel(Display *display, Window window, int32_t x, int32_t y, int button)
    {
        send_mouse_button(display, window, x, y, button, true, true);
    }

   
    // --- Cursor Marker state and helpers --- //
    struct CursorMarkerState
    {
        bool enabled = false;
        Display *display = nullptr;
        Window window = 0;
        std::chrono::steady_clock::time_point last_time;
        std::mutex mutex;
    };

    static CursorMarkerState cursor_marker;

    /**
     * Creates a small red window that will serve as a marker for the virtual cursor position.
     * This is useful for debugging and visualizing where the bot is "clicking" on the screen.
     */
    void create_cursor_marker()
    {
        cursor_marker.display = XOpenDisplay(NULL);
        if (!cursor_marker.display)
            return;

        int scr = DefaultScreen(cursor_marker.display);
        Window root = RootWindow(cursor_marker.display, scr);
        
        const int dot_size = 6; // 6x6 pixels marker
        const char *dot_color = "red"; // marker color name

        Colormap cmap = DefaultColormap(cursor_marker.display, scr);
        XColor color;
        XColor exact;
        if (!XAllocNamedColor(cursor_marker.display, cmap, dot_color, &color, &exact))
        {
            color.pixel = 0; // fallback black
        }

        XSetWindowAttributes attr;
        attr.override_redirect = True;
        attr.background_pixel = color.pixel;
        attr.background_pixmap = None;

        unsigned long mask = CWOverrideRedirect | CWBackPixel;
        cursor_marker.window = XCreateWindow(
            cursor_marker.display,
            root,
            0, 0, dot_size, dot_size, 0,
            CopyFromParent,
            InputOutput,
            CopyFromParent,
            mask,
            &attr);

        // make the window input‑transparent so it doesn't grab events
        int shape_event, shape_error;
        if (XShapeQueryExtension(cursor_marker.display, &shape_event, &shape_error))
        {
            XRectangle rect = {0, 0, 0, 0};
            XShapeCombineRectangles(cursor_marker.display,
                                    cursor_marker.window,
                                    ShapeInput,
                                    0, 0,
                                    &rect,
                                    1,
                                    ShapeSet,
                                    Unsorted);
        }

        XMapRaised(cursor_marker.display, cursor_marker.window);
        XFlush(cursor_marker.display);
    }

    /**
     * Destroys the cursor marker window and closes the display connection.
     */
    void destroy_cursor_marker()
    {
        if (cursor_marker.window && cursor_marker.display)
        {
            XDestroyWindow(cursor_marker.display, cursor_marker.window);
            XCloseDisplay(cursor_marker.display);
        }
        cursor_marker.window = 0;
        cursor_marker.display = nullptr;
    }
    
    /**
     * Checks if the cursor marker should be hidden due to inactivity (no updates for 3 seconds) and hides it if necessary.
     */
    void maybe_clear_marker()
    {
        std::lock_guard<std::mutex> lock(cursor_marker.mutex);
        if (!cursor_marker.enabled)
            return;

        auto now = std::chrono::steady_clock::now();
        if (now - cursor_marker.last_time >= std::chrono::seconds(3))
        {
            destroy_cursor_marker();
        }
    }

    /**
     * Updates the position of the cursor marker to the given coordinates.
     */
    void update_cursor_marker(int x, int y, int flash_pid, int browser_pid)
    {
        if (!cursor_marker.enabled || flash_pid == -1 || !x11_window_control_available())
            return;

        // record last update time
        {
            std::lock_guard<std::mutex> lock(cursor_marker.mutex);
            cursor_marker.last_time = std::chrono::steady_clock::now();
        }

        if (!cursor_marker.window)
            create_cursor_marker();

        if (cursor_marker.window && cursor_marker.display)
        {
            // translate coordinates from browser window space to root (screen) space
            with_browser_window(flash_pid, browser_pid, [&](Display *display, Window window) {
                Window root = DefaultRootWindow(display);
                int root_x = 0, root_y = 0;
                Window child = 0;
                XTranslateCoordinates(display, window, root, x, y, &root_x, &root_y, &child);

                // move marker on its own display (should be same as 'display' but we stored earlier when created)
                const int offset = 3; // center correction for a 6x6 dot
                XMoveWindow(cursor_marker.display, cursor_marker.window, root_x - offset, root_y - offset);
                XFlush(cursor_marker.display);
            });
        }

        // schedule a hide check in 3 seconds
        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            maybe_clear_marker();
        }).detach();
    }
}


enum class MessageType
{
    CALL,
    RESULT,
    SEND_NOTIFICATION,
    REFINE,
    UPGRADE,
    USE_ITEM,
    KEY_CLICK,
    CHECK_SIGNATURE,

    NONE
};

struct RefineMessage
{
    MessageType type = MessageType::REFINE;
    uintptr_t refine_util;
    int ore, amount;
};

struct SendNotificationMessage
{
    MessageType type = MessageType::SEND_NOTIFICATION;
    char name[64];
    uint32_t argc;
    uintptr_t argv[64];
};

struct FunctionResultMessage
{
    MessageType type = MessageType::RESULT;
    bool error = false;
    uintptr_t value;
};

struct CallFunctionMessage
{
    MessageType type = MessageType::CALL;;
    uintptr_t object;
    uint32_t index;
    int argc;
    uintptr_t argv[64];
};

struct UseItemMessage
{
    MessageType type = MessageType::USE_ITEM;
    char name[64];
    uint8_t action_type;
    bool action_bar;

    // ItemsControlMenuConstants.ACTION_SELECTION == 1
    // ItemsControlMenuConstants.ACTION_TOOGLE == 0
    // ItemsControlMenuConstants.ACTION_ONE_SHOT == 1
    // barId = _loc2_.barId == CATEGORY_BAR ? 0 : 1;

};

struct KeyClickMessage
{
    MessageType type = MessageType::KEY_CLICK;
    uint32_t key;
};

struct GetSignatureMessage
{
    MessageType type = MessageType::CHECK_SIGNATURE;;
    uintptr_t object;
    uint32_t index;
    bool method_name;
    char signature[0x100];

    int32_t result;
};

union Message
{
    Message() { };
    MessageType type = MessageType::NONE;;
    CallFunctionMessage call;
    FunctionResultMessage result;
    SendNotificationMessage notify;
    RefineMessage refine;
    UseItemMessage item;
    KeyClickMessage key;
    GetSignatureMessage sig;
};


BotClient::BotClient() :
    m_browser_ipc(new SockIpc())
{
}

void BotClient::ToggleBrowserVisibility(bool visible)
{
    with_browser_window(m_flash_pid, m_browser_pid, [=](Display *display, Window window) {
        visible ? XMapWindow(display, window) : XUnmapWindow(display, window);
    });
}

BotClient::~BotClient()
{
    if (m_browser_pid > 0)
    {
        kill(m_browser_pid, SIGKILL);
    }
}

void sigchld_handler(int signal)
{
    int status = 0;
    waitpid(0, &status, WNOHANG);
}

void BotClient::LaunchBrowser()
{
    int pid = fork();

    switch (pid)
    {
        case -1: // https://rachelbythebay.com/w/2014/08/19/fork/
        {
            perror("Fork failed.");
            break;
        }
        case 0:
        {
            const char *fpath = "lib/backpage-linux-x86_64.AppImage";

            std::vector<const char *> envp
            {
                "LD_PRELOAD=lib/libdo_lib.so",
            };
            for (int i = 0; environ[i]; i++)
            {
                envp.push_back(environ[i]);
            }
            envp.push_back(nullptr);

            std::string url = m_url;
            std::string sid = m_sid;

            while (*(url.end()-1) == '/')
            {
                url.resize(url.size()-1);
            }

            if (sid.find("dosid=") == 0)
            {
                sid.replace(0, 6, "");
            }

                execle(
                    fpath,
                    fpath,
                    "--sid", sid.c_str(),
                    "--url", url.c_str(),
                    "--launch",
                    "--ozone-platform=x11",
                    "--disable-background-timer-throttling",
                    "--disable-renderer-backgrounding",
                    NULL,
                    envp.data());
            break;
        }
        default:
        {
            signal(SIGCHLD, sigchld_handler);

            m_browser_pid = pid;
            break;
        }
    }
}

void BotClient::SendBrowserCommand(const std::string &&message, int sync)
{
    if (m_browser_pid > 0 && !ProcUtil::ProcessExists(m_browser_pid))
    {
        fprintf(stderr, "[SendBrowserCommand] Browser process not found, restarting it\n");
        LaunchBrowser();
        m_flash_pid = -1;
        return;
    }

    if (!m_browser_ipc->Connected())
    {
        if (m_browser_pid < 0)
        {
            return;
        }

        std::string ipc_path = utils::format("/tmp/darkbot_ipc_{}", m_browser_pid);

        //printf("[SendBrowserCommand] Connecting to %s\n", ipc_path.c_str());

        if (!m_browser_ipc->Connect(ipc_path))
        {
            printf("[SendBrowserCommand] Failed to connect to browser %d\n", m_browser_pid);
            return;
        }
    }

    //printf("[SendBrowserCommand] Sending message %s\n", message.c_str());

    m_browser_ipc->Send(message);
    return;
}

bool BotClient::find_flash_process()
{
    auto procs = ProcUtil::FindProcsByName("no-sandbox");
    for (int proc_pid : procs)
    {
        if (ProcUtil::IsChildOf(proc_pid, m_browser_pid) && ProcUtil::GetPages(proc_pid, "libpepflashplayer").size() > 0)
        {
            m_flash_pid = proc_pid;
            return true;
        }
    }
    return false;
}

void BotClient::reset()
{
    // Reset
    if (m_shared_mem_flash) shmdt(m_shared_mem_flash);
    if (m_flash_sem >= 0) semctl(m_flash_sem, 0, IPC_RMID, 1);


    m_shared_mem_flash = nullptr;
    m_flash_pid = -1;
    m_flash_sem = -1;
    m_flash_shmid = -1;
}

// Not a great name since it has side-effects like refreshgin or restarting the browser
bool BotClient::IsValid()
{
    if (m_browser_pid > 0 && !ProcUtil::ProcessExists(m_browser_pid))
    {
        fprintf(stderr, "[IsValid] Browser process not found, restarting it\n");
        LaunchBrowser();
        return false;
    }


    if (m_flash_pid == -1)
    {
        return find_flash_process();
    }

    if (!ProcUtil::ProcessExists(m_flash_pid))
    {
        fprintf(stderr, "[IsValid] Flash process not found, trying to refresh %d, %d\n", m_flash_pid, m_browser_pid);
        SendBrowserCommand("refresh", 1);
        reset();
        return false;
    }
    return true;
}

void BotClient::SendFlashCommand(Message *message, Message *response)
{
    if (!IsValid())
    {
        return;
    }

    if ((m_flash_shmid = shmget(m_flash_pid, MEM_SIZE, IPC_CREAT | 0666)) < 0)
    {
        fprintf(stderr, "[SendFlashCommand] Failed to get shared memory\n");
        return;
    }

    if (!m_shared_mem_flash || m_shared_mem_flash == (void *)-1)
    {
        if ((m_shared_mem_flash = reinterpret_cast<Message *>(shmat(m_flash_shmid, NULL, 0))) == (void *)-1)
        {
            fprintf(stderr, "[SendFlashCommand] Failed to attach shared memory to our process\n");
            return;
        }
    }

    if (m_flash_sem < 0)
    {
        if ((m_flash_sem = semget(m_flash_pid , 2, IPC_CREAT | 0600)) < 0)
        {
            m_flash_pid = -1;
            fprintf(stderr, "[SendFlashCommand] Failed to create semaphore");
            return;
        }
    }


    *m_shared_mem_flash = *message;

    static timespec timeout { .tv_sec = 1, .tv_nsec = 0 };
    sembuf sop[2] { { 0, -1, 0 }, { 1, 0, 0 } };

    // Notify
    if (semtimedop(m_flash_sem, &sop[0], 1, &timeout) == -1)
    {
        if (errno == EAGAIN)
        {
            fprintf(stderr, "[SendFlashCommand] Failed to send command to flash, timeout\n");
            return;
        }
        perror("[SendFlashCommand] semop failed");
        return;
    }

    // Wait
    if (semtimedop(m_flash_sem, &sop[1], 1, &timeout) == -1)
    {
        if (errno == EAGAIN)
        {
            fprintf(stderr, "[SendFlashCommand] Failed to send command to flash, timeout\n");
            return;
        }
        perror("[SendFlashCommand] semop failed");
        return;
    }

    if (response)
    {
        memcpy(response, m_shared_mem_flash, sizeof(Message));
    }
}

bool BotClient::SendNotification(uintptr_t screen_manager, const std::string &name, const std::vector<uintptr_t> &args)
{
    Message message;
    message.type = MessageType::SEND_NOTIFICATION;
    size_t cap = sizeof(message.notify.argv) / sizeof(message.notify.argv[0]);
    size_t to_copy = std::min(args.size(), cap);
    message.notify.argc = to_copy;
    if (to_copy)
        std::memcpy(message.notify.argv, args.data(), to_copy * sizeof(message.notify.argv[0]));
    std::strncpy(message.notify.name, name.c_str(), sizeof(message.notify.name));
    message.notify.name[sizeof(message.notify.name) - 1] = '\0';
    SendFlashCommand(&message);
    return true;
}

bool BotClient::RefineOre(uintptr_t refine_util, uint32_t ore, uint32_t amount)
{
    Message message;
    message.type = MessageType::REFINE;
    message.refine.refine_util = refine_util;
    message.refine.ore = ore;
    message.refine.amount = amount;

    SendFlashCommand(&message);
    return true;
}

bool BotClient::UseItem(const std::string &name, uint8_t type, uint8_t bar)
{
    Message message;
    message.type = MessageType::USE_ITEM;
    message.item.action_type = type;
    message.item.action_bar = bar;
    std::strncpy(message.item.name, name.c_str(), sizeof(message.item.name));
    message.item.name[sizeof(message.item.name) - 1] = '\0';
    SendFlashCommand(&message);
    return true;
}

uintptr_t BotClient::CallMethod(uintptr_t obj, uint32_t index, const std::vector<uintptr_t> &args)
{
    Message message;
    message.type = MessageType::CALL;

    message.call.object = obj;
    message.call.index = index;
    size_t cap = sizeof(message.call.argv) / sizeof(message.call.argv[0]);
    size_t to_copy = std::min(args.size(), cap);
    message.call.argc = to_copy;
    if (to_copy)
        memcpy(message.call.argv, args.data(), to_copy * sizeof(uintptr_t));

    Message response;

    SendFlashCommand(&message, &response);

    return response.result.value;
}

bool BotClient::ClickKey(uint32_t key)
{
    Message message;
    message.type = MessageType::KEY_CLICK;
    message.key.key = key;
    SendFlashCommand(&message);
    return true;
}

void BotClient::MouseClick(int32_t x, int32_t y)
{
    with_browser_window(m_flash_pid, m_browser_pid, [=](Display *display, Window window) {
        send_mouse_button(display, window, x, y, Button1, true, true);
    });
    UpdateCursorMarker(x, y);
}

void BotClient::MouseMove(int32_t x, int32_t y)
{
    with_browser_window(m_flash_pid, m_browser_pid, [=](Display *display, Window window) {
        send_mouse_move(display, window, x, y);
    });
    UpdateCursorMarker(x, y);
}

void BotClient::MouseDown(int32_t x, int32_t y)
{
    with_browser_window(m_flash_pid, m_browser_pid, [=](Display *display, Window window) {
        send_mouse_button(display, window, x, y, Button1, true, false);
    });
    UpdateCursorMarker(x, y);
}

void BotClient::MouseUp(int32_t x, int32_t y)
{
    with_browser_window(m_flash_pid, m_browser_pid, [=](Display *display, Window window) {
        send_mouse_button(display, window, x, y, Button1, false, true);
    });
    UpdateCursorMarker(x, y);
}

void BotClient::MouseScroll(int32_t x, int32_t y, int32_t delta)
{
    int button = delta >= 0 ? Button4 : Button5;
    with_browser_window(m_flash_pid, m_browser_pid, [=](Display *display, Window window) {
        send_mouse_wheel(display, window, x, y, button);
    });
    UpdateCursorMarker(x, y);
}

int BotClient::CheckMethodSignature(uintptr_t object, uint32_t index, bool check_name, const std::string &sig)
{
    Message message;
    message.type = MessageType::CHECK_SIGNATURE;
    message.sig.object = object;
    message.sig.index = index;
    message.sig.method_name = check_name;

    strncpy(message.sig.signature, sig.c_str(), sizeof(message.sig.signature));
    message.sig.signature[sizeof(message.sig.signature) - 1] = '\0';

    Message response;
    SendFlashCommand(&message, &response);

    return response.sig.result;
}

void BotClient::EnableCursorMarker(bool enable)
{
    if (enable == cursor_marker.enabled)
        return;

    cursor_marker.enabled = enable;
    if (!enable)
    {
        std::lock_guard<std::mutex> lock(cursor_marker.mutex);
        destroy_cursor_marker();
    }
}

void BotClient::UpdateCursorMarker(int32_t x, int32_t y)
{
    update_cursor_marker(x, y, m_flash_pid, m_browser_pid);
}
