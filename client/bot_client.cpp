#include "bot_client.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>

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


#define MEM_SIZE 1024

namespace
{
    Window browser_window = 0;

    bool get_window_pid(Display *display, Window window, pid_t &pid)
    {
        Atom atom_pid = XInternAtom(display, "_NET_WM_PID", True);
        if (atom_pid == None)
        {
            return false;
        }

        Atom actual_type = None;
        int actual_format = 0;
        unsigned long nitems = 0;
        unsigned long bytes_after = 0;
        unsigned char *prop = nullptr;

        int status = XGetWindowProperty(
                display,
                window,
                atom_pid,
                0,
                1,
                False,
                XA_CARDINAL,
                &actual_type,
                &actual_format,
                &nitems,
                &bytes_after,
                &prop);

        if (status != Success || !prop || nitems == 0)
        {
            if (prop)
            {
                XFree(prop);
            }
            return false;
        }

        pid = static_cast<pid_t>(*reinterpret_cast<unsigned long *>(prop));
        XFree(prop);
        return true;
    }

    bool is_browser_window_pid(pid_t owner_pid, pid_t browser_pid)
    {
        return owner_pid == browser_pid || ProcUtil::IsChildOf(owner_pid, browser_pid);
    }

    bool x11_window_control_available()
    {
        const char *display = std::getenv("DISPLAY");
        return display && *display;
    }

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

    Window find_browser_client_window(Display *display, pid_t browser_pid)
    {
        Window root = DefaultRootWindow(display);
        Atom atom_client_list = XInternAtom(display, "_NET_CLIENT_LIST", True);
        if (atom_client_list != None)
        {
            Atom actual_type = None;
            int actual_format = 0;
            unsigned long nitems = 0;
            unsigned long bytes_after = 0;
            unsigned char *prop = nullptr;

            int status = XGetWindowProperty(
                    display,
                    root,
                    atom_client_list,
                    0,
                    4096,
                    False,
                    XA_WINDOW,
                    &actual_type,
                    &actual_format,
                    &nitems,
                    &bytes_after,
                    &prop);

            if (status == Success && prop && actual_type == XA_WINDOW)
            {
                Window *windows = reinterpret_cast<Window *>(prop);
                Window child_fallback = 0;
                for (unsigned long i = 0; i < nitems; i++)
                {
                    pid_t owner_pid = -1;
                    if (!get_window_pid(display, windows[i], owner_pid))
                    {
                        continue;
                    }

                    if (owner_pid == browser_pid)
                    {
                        XFree(prop);
                        return windows[i];
                    }

                    if (!child_fallback && ProcUtil::IsChildOf(owner_pid, browser_pid))
                    {
                        child_fallback = windows[i];
                    }
                }

                XFree(prop);
                if (child_fallback)
                {
                    return child_fallback;
                }
            }
            else if (prop)
            {
                XFree(prop);
            }
        }

        Window any_owned = find_browser_owned_descendant_recursive(display, root, browser_pid);
        if (!any_owned)
        {
            return 0;
        }

        return find_toplevel_root_child(display, root, any_owned);
    }

    void toggle_browser_visibility(pid_t browser_pid, bool visible)
    {
        Display *display = XOpenDisplay(nullptr);
        if (!display)
        {
            return; // Failed to open display
        }
        
        if (!browser_window || !try_get_window_attrs(display, browser_window))
        {
            browser_window = find_browser_client_window(display, browser_pid);
        }

        if (!browser_window)
        {
            XCloseDisplay(display);
            return; // Failed to find browser window
        }

        // Show or hide the browser window
        visible ? XMapWindow(display, browser_window) : XUnmapWindow(display, browser_window);

        XFlush(display);
        XCloseDisplay(display);
    }

    // Execute X11 mouse action on the browser window.
    template<typename Func>
    bool execute_mouse_action(int flash_pid, int browser_pid, Func&& action)
    {
        if (flash_pid == -1 || !x11_window_control_available())
        {
            return false;
        }

        Display *display = XOpenDisplay(nullptr);
        if (!display)
        {
            return false;
        }

        if (!browser_window || !try_get_window_attrs(display, browser_window))
        {
            browser_window = find_browser_client_window(display, browser_pid);
        }

        bool result = false;
        if (browser_window)
        {
            result = action(display, browser_window);
        }

        XCloseDisplay(display);
        return result;
    }

    struct MouseEventContext
    {
        Display *display;
        Window window;
        Window root;
        int local_x, local_y;
        int root_x, root_y;
    };

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

    bool send_mouse_button(Display *display, Window window, int32_t x, int32_t y, int button, bool press, bool release)
    {
        MouseEventContext ctx;
        if (!prepare_mouse_event(display, window, x, y, ctx))
        {
            return false;
        }

        XEvent event;
        fill_mouse_event_common(event, ctx);
        event.xbutton.button = button;

        if (press)
        {
            event.xbutton.type = ButtonPress;
            if (XSendEvent(ctx.display, ctx.window, True, ButtonPressMask, &event) == 0)
            {
                return false;
            }
        }

        if (release)
        {
            event.xbutton.type = ButtonRelease;
            if (XSendEvent(ctx.display, ctx.window, True, ButtonReleaseMask, &event) == 0)
            {
                return false;
            }
        }

        XFlush(ctx.display);
        return true;
    }

    bool send_mouse_wheel(Display *display, Window window, int32_t x, int32_t y, int button)
    {
        return send_mouse_button(display, window, x, y, button, true, true);
    }

    bool send_mouse_move(Display *display, Window window, int32_t x, int32_t y)
    {
        MouseEventContext ctx;
        if (!prepare_mouse_event(display, window, x, y, ctx))
        {
            return false;
        }

        XEvent event;
        fill_mouse_event_common(event, ctx);
        event.xmotion.type = MotionNotify;

        if (XSendEvent(ctx.display, ctx.window, True, PointerMotionMask, &event) == 0)
        {
            return false;
        }

        XFlush(ctx.display);
        return true;
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
    MOUSE_CLICK,
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

struct MouseClickMessage
{
    MessageType type = MessageType::MOUSE_CLICK;
    uint32_t button;
    int32_t x;
    int32_t y;
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
    MouseClickMessage click;
    GetSignatureMessage sig;
};


BotClient::BotClient() :
    m_browser_ipc(new SockIpc())
{
}

void BotClient::ToggleBrowserVisibility(bool visible)
{
    if (m_flash_pid == -1 || !x11_window_control_available())
    {
        return;
    }

    toggle_browser_visibility(m_browser_pid, visible);
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

bool BotClient::MouseClick(int32_t x, int32_t y, uint32_t button)
{
    return execute_mouse_action(m_flash_pid, m_browser_pid, [=](Display *display, Window window) {
        return send_mouse_button(display, window, x, y, button, true, true);
    });
}

bool BotClient::MouseMove(int32_t x, int32_t y)
{
    return execute_mouse_action(m_flash_pid, m_browser_pid, [=](Display *display, Window window) {
        return send_mouse_move(display, window, x, y);
    });
}

bool BotClient::MouseDown(int32_t x, int32_t y, uint32_t button)
{
    return execute_mouse_action(m_flash_pid, m_browser_pid, [=](Display *display, Window window) {
        return send_mouse_button(display, window, x, y, button, true, false);
    });
}

bool BotClient::MouseUp(int32_t x, int32_t y, uint32_t button)
{
    return execute_mouse_action(m_flash_pid, m_browser_pid, [=](Display *display, Window window) {
        return send_mouse_button(display, window, x, y, button, false, true);
    });
}

bool BotClient::MouseScroll(int32_t x, int32_t y, int32_t delta)
{
    return execute_mouse_action(m_flash_pid, m_browser_pid, [=](Display *display, Window window) {
        int steps = delta == 0 ? 0 : (std::abs(delta) + 119) / 120;
        int button = delta >= 0 ? Button4 : Button5;
        for (int i = 0; i < steps; i++)
        {
            if (!send_mouse_wheel(display, window, x, y, button))
            {
                return false;
            }
        }
        return steps > 0;
    });
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
