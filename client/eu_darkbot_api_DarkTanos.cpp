
#include "eu_darkbot_api_DarkTanos.h"
#include <unistd.h>
#include <cstring>
#include <vector>
#include <mutex>
#include <chrono>

#include "bot_client.h"
#include "utils.h"

static BotClient client;
static std::mutex post_actions_mutex;


JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_setData
  (JNIEnv *env, jobject, jstring jurl, jstring jsid, jstring preloader, jstring vars)
{
    std::string temp_sid, temp_url;

    const char *sid_cstr = env->GetStringUTFChars(jsid , NULL);
    const char *url_cstr = env->GetStringUTFChars(jurl, NULL);

    client.SetCredentials(sid_cstr, url_cstr);

    env->ReleaseStringUTFChars(jurl, url_cstr);
    env->ReleaseStringUTFChars(jsid, sid_cstr);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_createWindow
  (JNIEnv *env, jobject)
{
    client.LaunchBrowser();
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_setSize
  (JNIEnv *, jobject, jint jw, jint jh)
{
    client.SendBrowserCommand(utils::format("setSize|{}|{}", jw, jh), 0);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_setVisible
  (JNIEnv *, jobject, jboolean jv)
{
    client.ToggleBrowserVisibility(jv);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_setMinimized
  (JNIEnv *, jobject, jboolean jv)
{
    // Using the same hiding approach as with "setVisible", since minimizing causes lags and increases the tick.
    // The boolean "jv" is inverted because "setMinimized(true)" should hide the window. 
    client.ToggleBrowserVisibility(!jv);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_reload
  (JNIEnv *, jobject)
{
    client.SendBrowserCommand("refresh", 1);
}

JNIEXPORT jboolean JNICALL Java_eu_darkbot_api_DarkTanos_isValid
  (JNIEnv *, jobject)
{
    return client.IsValid();
}

JNIEXPORT jlong JNICALL Java_eu_darkbot_api_DarkTanos_getMemoryUsage
  (JNIEnv *, jobject)
{
    if (client.FlashPid() > 0)
    {
        return ProcUtil::GetMemoryUsage(client.FlashPid()) / 1024;
    }
    return ProcUtil::GetMemoryUsage(client.Pid()) / 1024;
}

JNIEXPORT jint JNICALL Java_eu_darkbot_api_DarkTanos_getVersion
  (JNIEnv *, jobject)
{
    return API_VERSION;
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_keyClick
  (JNIEnv *, jobject, jint c)
{
    //client.SendBrowserCommand(utils::format("pressKey|{}", c), 1);
    client.ClickKey(c);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_sendText
  (JNIEnv *env, jobject, jstring jtext)
{
    const char *cstr = env->GetStringUTFChars(jtext, NULL);
    std::string text = cstr;
    env->ReleaseStringUTFChars(jtext, cstr);
    client.SendBrowserCommand("text|" + text, 1);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_mouseMove
  (JNIEnv *, jobject, jint x, jint y)
{
    client.MouseMove(x, y);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_mouseDown
  (JNIEnv *, jobject, jint x, jint y)
{
    client.MouseDown(x, y, 1);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_mouseUp
  (JNIEnv *, jobject, jint x, jint y)
{
    client.MouseUp(x, y, 1);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_mouseClick
  (JNIEnv *, jobject, jint x, jint y)
{
    client.MouseClick(x, y, 1);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_postActions
  (JNIEnv *env, jobject, jlongArray jactions)
{
    if (!jactions)
    {
        return;
    }

    jsize len = env->GetArrayLength(jactions);
    if (len <= 0)
    {
        return;
    }

    std::lock_guard<std::mutex> actions_lock(post_actions_mutex);

    std::vector<jlong> actions(static_cast<size_t>(len));
    env->GetLongArrayRegion(jactions, 0, len, actions.data());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);

    for (jsize i = 0; i < len; ++i)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
          break;
        }

        jlong action = actions[static_cast<size_t>(i)];
        uint64_t value = static_cast<uint64_t>(action);
        uint16_t message = static_cast<uint16_t>((value >> 48) & 0x7fff);
        int16_t wparam = static_cast<int16_t>((value >> 32) & 0xffff);
        int16_t lparam_low = static_cast<int16_t>(value & 0xffff);
        int16_t lparam_high = static_cast<int16_t>((value >> 16) & 0xffff);

        int x = lparam_low;
        int y = lparam_high;

        // Handle native mouse and keyboard events based on the message type.
        // https://github.com/darkbot-reloaded/DarkBot/blob/master/src/main/java/eu/darkbot/api/utils/NativeAction.java

        switch (message)
        {
        case 0x1FF: // Mouse CLICK
            client.MouseClick(x, y, 1);
            break;
        case 0x200: // Mouse MOVE
            client.MouseMove(x, y);
            break;
        case 0x201: // Mouse DOWN
            client.MouseDown(x, y, 1);
            break;
        case 0x202: // Mouse UP
            client.MouseUp(x, y, 1);
            break;
        case 0x20A: // Mouse WHEEL
            client.MouseScroll(x, y, wparam);
            break;
        case 0x1FE: // Key CLICK
        case 0x100: // Key DOWN
        case 0x101: // Key UP
        case 0x102: // Key CHAR
            client.ClickKey(static_cast<uint16_t>(wparam));
            break;
        default:
            // Unsupported message, ignore
            break;
        }
    }
}

JNIEXPORT jint JNICALL Java_eu_darkbot_api_DarkTanos_readInt
  (JNIEnv *, jobject, jlong addr)
{
    return client.Read<int>(addr);
}

JNIEXPORT jlong JNICALL Java_eu_darkbot_api_DarkTanos_readLong
  (JNIEnv *, jobject, jlong addr)
{
    return client.Read<uintptr_t>(addr);
}

JNIEXPORT jdouble JNICALL Java_eu_darkbot_api_DarkTanos_readDouble
  (JNIEnv *, jobject, jlong addr)
{
    return client.Read<double>(addr);
}

JNIEXPORT jboolean JNICALL Java_eu_darkbot_api_DarkTanos_readBoolean
  (JNIEnv *, jobject, jlong addr)
{
    return client.Read<bool>(addr);
}

JNIEXPORT jbyteArray JNICALL Java_eu_darkbot_api_DarkTanos_readBytes__JI
  (JNIEnv *env, jobject, jlong jaddr, jint jsize)
{
    std::vector<uint8_t> stuff(jsize);
    size_t bytes_read = ProcUtil::ReadMemoryBytes(client.FlashPid(), jaddr, &stuff[0], stuff.size());
    jbyteArray barray = env->NewByteArray(jsize);
    env->SetByteArrayRegion(barray, 0, jsize, (jbyte*)(&stuff[0]));
    return barray;
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_readBytes__J_3BI
  (JNIEnv *env, jobject, jlong jaddr, jbyteArray jout, jint jsize)
{
    std::vector<uint8_t> stuff(env->GetArrayLength(jout));
    size_t bytes_read = ProcUtil::ReadMemoryBytes(client.FlashPid(), jaddr, &stuff[0], stuff.size());
    env->SetByteArrayRegion(jout, 0, jsize, (jbyte*)(&stuff[0]));
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_replaceInt
  (JNIEnv *, jobject, jlong jaddr, jint jold, jint jnew)
{
    if (client.Read<int>(jaddr) == jold)
        client.Write(jaddr, jnew);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_replaceLong
  (JNIEnv *, jobject, jlong jaddr, jlong jold, jlong jnew)
{
    if (client.Read<uintptr_t>(jaddr) == jold)
        client.Write(jaddr, jnew);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_replaceDouble
  (JNIEnv *, jobject, jlong jaddr, jdouble jold, jdouble jnew)
{
    if (client.Read<double>(jaddr) == jold)
        client.Write(jaddr, jnew);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_replaceBoolean
  (JNIEnv *, jobject, jlong jaddr, jboolean jold, jboolean jnew)
{
    if (client.Read<bool>(jaddr) == jold)
        client.Write(jaddr, jnew);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_writeInt
  (JNIEnv *, jobject, jlong jaddr, jint jval)
{
    client.Write(jaddr, jval);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_writeLong
  (JNIEnv *, jobject, jlong jaddr, jlong jval)
{
    client.Write(jaddr, jval);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_writeDouble
  (JNIEnv *, jobject, jlong jaddr, jdouble jval)
{
    client.Write(jaddr, jval);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_writeBoolean
  (JNIEnv *, jobject, jlong jaddr, jboolean jval)
{
    client.Write(jaddr, jval);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_writeBytes
  (JNIEnv *env, jobject, jlong jaddr, jbyteArray jval)
{
    std::vector<uint8_t> data(env->GetArrayLength(jval));

    env->GetByteArrayRegion(jval, 0, data.size(), reinterpret_cast<jbyte*>(&data[0]));
    ProcUtil::WriteMemoryBytes(client.FlashPid(), jaddr, &data[0], data.size());
}

JNIEXPORT jlongArray JNICALL Java_eu_darkbot_api_DarkTanos_queryInt
  (JNIEnv *env, jobject, jint jquery, jint jamount)
{
    auto out = client.QueryMemory(reinterpret_cast<uint8_t *>(&jquery), sizeof(jquery), static_cast<uint32_t>(jamount));
    jlongArray addresses = env->NewLongArray(out.size());
    env->SetLongArrayRegion(addresses, (jsize)0, (jsize)out.size(), reinterpret_cast<jlong*>(&out[0]));
    return addresses;
}

JNIEXPORT jlongArray JNICALL Java_eu_darkbot_api_DarkTanos_queryLong
  (JNIEnv *env, jobject, jlong jquery, jint jamount)
{
    auto out = client.QueryMemory(reinterpret_cast<uint8_t *>(&jquery), sizeof(jquery), static_cast<uint32_t>(jamount));
    jlongArray addresses = env->NewLongArray(out.size());
    env->SetLongArrayRegion(addresses, (jsize)0, (jsize)out.size(), reinterpret_cast<jlong*>(&out[0]));
    return addresses;
}

JNIEXPORT jlongArray JNICALL Java_eu_darkbot_api_DarkTanos_queryBytes
  (JNIEnv * env, jobject, jbyteArray jquery, jint jamount)
{
    size_t query_size = env->GetArrayLength(jquery);

    std::vector<uint8_t> query(query_size);

    env->GetByteArrayRegion(jquery, 0, query_size, reinterpret_cast<jbyte*>(&query[0]));

    auto out = client.QueryMemory(query, jamount);
    
    jlongArray addresses = env->NewLongArray(out.size());
    env->SetLongArrayRegion(addresses, (jsize)0, (jsize)out.size(), reinterpret_cast<jlong*>(&out[0]));

    return addresses;
}


JNIEXPORT jboolean JNICALL Java_eu_darkbot_api_DarkTanos_sendNotification
  (JNIEnv *env, jobject, jlong screen_manager, jstring jname, jlongArray jargs)
{
    std::vector<uintptr_t> args(env->GetArrayLength(jargs));

    env->GetLongArrayRegion(jargs, 0, args.size(), reinterpret_cast<jlong *>(&args[0]));

    std::string name(env->GetStringUTFChars(jname, NULL));

    return client.SendNotification(screen_manager, name, args);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_selectEntity
  (JNIEnv *, jobject, jlong, jlong, jboolean)
{
    return;
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_refine
  (JNIEnv *env, jobject, jlong joreutils, jint jore, jint jamount)
{
    client.RefineOre(joreutils, jore, jamount);
}

JNIEXPORT jboolean JNICALL Java_eu_darkbot_api_DarkTanos_useItem
  (JNIEnv *env, jobject, jlong conn_manager, jstring jname, jint jdunno, jlongArray jargs)
{
    std::vector<uintptr_t> args(env->GetArrayLength(jargs));
    env->GetLongArrayRegion(jargs, 0, args.size(), reinterpret_cast<jlong *>(&args[0]));
    return client.UseItem(env->GetStringUTFChars(jname, NULL), 1, 0);
}

JNIEXPORT jlong JNICALL Java_eu_darkbot_api_DarkTanos_callMethod
  (JNIEnv *env, jobject, jlong jthis, jint jindex, jlongArray jargs)
{
    std::vector<uintptr_t> args(env->GetArrayLength(jargs));

    env->GetLongArrayRegion(jargs, 0, args.size(), reinterpret_cast<jlong *>(&args[0]));
    return client.CallMethod(jthis, jindex, args);
}

JNIEXPORT jint JNICALL Java_eu_darkbot_api_DarkTanos_checkMethodSignature
  (JNIEnv *env, jobject, jlong object, jint index, jboolean check_name, jstring sig)
{
    const char *sig_cstr = env->GetStringUTFChars(sig, NULL);

    int result = client.CheckMethodSignature(object, index, check_name, sig_cstr);

    env->ReleaseStringUTFChars(sig, sig_cstr);

    return result;
}

