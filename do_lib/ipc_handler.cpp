// Executes flash IPC requests (see tools/flash_ipc.h) on the game thread.
#include <cstring>
#include <memory>

#include "darkorbit.h"
#include "utils.h"

using flash_ipc::Message;
using flash_ipc::MessageType;
using flash_ipc::ResultCode;

// Waits for a queued game-thread call; cancels it on timeout so it never runs late.
static ResultCode wait_call(std::future<uintptr_t> &res, const std::shared_ptr<std::atomic<bool>> &cancelled, uintptr_t *out)
{
    if (!res.valid())
        return ResultCode::QUEUE_FULL;

    if (res.wait_for(std::chrono::milliseconds(flash_ipc::SERVER_CALL_TIMEOUT_MS)) != std::future_status::ready)
    {
        cancelled->store(true);
        return ResultCode::TIMEOUT;
    }

    try
    {
        uintptr_t value = res.get();
        if (out)
            *out = value;
        return ResultCode::OK;
    }
    catch (const std::future_error &)
    {
        // Task dropped (hooks uninstalled before it ran)
        return ResultCode::NOT_READY;
    }
}

ResultCode Darkorbit::handle_ipc_message(Message &msg)
{
    auto &darkorbit = *this;

    // An async command is acknowledged as soon as it's queued and dropped if the game thread
    // doesn't run it within ASYNC_MAX_DELAY_MS. Don't accept it (and report success) when the
    // game thread is already lagging that much: the caller can fall back (e.g. to a keybind).
    if (flash_ipc::is_async(msg.type) && m_ipc.HeartbeatAgeMs() >= flash_ipc::ASYNC_MAX_DELAY_MS)
        return ResultCode::NOT_READY;

    switch (msg.type)
    {
        case MessageType::CALL:
        {
            auto call = msg.call;

            if (!call.object || call.argc < 0 || static_cast<size_t>(call.argc) > std::size(call.argv))
            {
                utils::log("[Darkorbit::handle_ipc_message] invalid call request\n");
                msg.result = {};
                msg.result.error = true;
                return ResultCode::BAD_REQUEST;
            }

            auto cancelled = std::make_shared<std::atomic<bool>>(false);
            auto res = darkorbit.call_sync([call] () mutable
            {
                auto *object = reinterpret_cast<avm::ScriptObject *>(call.object);
                return object->call_method(call.index, call.argc, call.argv);
            }, cancelled);

            uintptr_t value = 0;
            ResultCode status = wait_call(res, cancelled, &value);

            msg.result = {};
            msg.result.error = status != ResultCode::OK;
            msg.result.value = value;
            return status;
        }
        case MessageType::CHECK_SIGNATURE:
        {
            auto object = reinterpret_cast<avm::ScriptObject *>(msg.sig.object);
            uint32_t index = msg.sig.index;
            bool method_name = msg.sig.method_name;
            std::string signature(msg.sig.signature, strnlen(msg.sig.signature, sizeof(msg.sig.signature)));

            auto cancelled = std::make_shared<std::atomic<bool>>(false);
            auto res = darkorbit.call_sync([object, index, method_name, signature = std::move(signature)]()
            {
                return static_cast<uintptr_t>(static_cast<intptr_t>(
                    Darkorbit::get().check_method_signature(object, index, method_name, signature)));
            }, cancelled);

            uintptr_t value = 0;
            ResultCode status = wait_call(res, cancelled, &value);
            if (status != ResultCode::OK)
                utils::log("[Darkorbit::handle_ipc_message] Signature check failed, status {}\n", static_cast<int>(status));

            msg.sig.result = status == ResultCode::OK ? static_cast<int32_t>(static_cast<intptr_t>(value)) : -1;
            return status;
        }
        case MessageType::SEND_NOTIFICATION:
        {
            const auto &notify = msg.notify;
            if (static_cast<size_t>(notify.argc) > std::size(notify.argv))
                return ResultCode::BAD_REQUEST;

            std::string name(notify.name, strnlen(notify.name, sizeof(notify.name)));
            std::vector<uintptr_t> args(&notify.argv[0], &notify.argv[notify.argc]);

            return darkorbit.post_async([name = std::move(name), args = std::move(args)] ()
            {
                return static_cast<uintptr_t>(Darkorbit::get().send_notification(name, args));
            }) ? ResultCode::OK : ResultCode::QUEUE_FULL;
        }
        case MessageType::USE_ITEM:
        {
            std::string name(msg.item.name, strnlen(msg.item.name, sizeof(msg.item.name)));

            return darkorbit.post_async([name = std::move(name)] ()
            {
                return static_cast<uintptr_t>(Darkorbit::get().use_item(name, 0, 1));
            }) ? ResultCode::OK : ResultCode::QUEUE_FULL;
        }
        case MessageType::REFINE:
        {
            return darkorbit.post_async([ore = msg.refine.ore, amount = msg.refine.amount]
            {
                return static_cast<uintptr_t>(Darkorbit::get().refine_ore(ore, amount));
            }) ? ResultCode::OK : ResultCode::QUEUE_FULL;
        }
        case MessageType::KEY_CLICK:
        {
            return darkorbit.post_async([key = msg.key.key]
            {
                return static_cast<uintptr_t>(Darkorbit::get().key_click(key));
            }) ? ResultCode::OK : ResultCode::QUEUE_FULL;
        }
        case MessageType::MOUSE_CLICK:
        {
            return darkorbit.post_async([x = msg.click.x, y = msg.click.y, button = msg.click.button]
            {
                return static_cast<uintptr_t>(Darkorbit::get().mouse_click(x, y, button));
            }) ? ResultCode::OK : ResultCode::QUEUE_FULL;
        }
        default:
            utils::log("[Darkorbit::handle_ipc_message] Unknown ipc message type {}\n", static_cast<int>(msg.type));
            return ResultCode::BAD_REQUEST;
    }
}
