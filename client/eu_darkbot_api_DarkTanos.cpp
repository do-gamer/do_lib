
#include "eu_darkbot_api_DarkTanos.h"
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <vector>

#include "bot_client.h"
#include "utils.h"

// Never destroyed: native threads (paste worker, marker, JNI callers) may still use it
// while the JVM exits. The browser is stopped from an exit handler instead.
static BotClient &client = *new BotClient();

namespace
{
    // RAII wrapper so every GetStringUTFChars is released (previously several calls leaked
    // on every invocation, e.g. sendNotification runs for every entity selection).
    class JString
    {
    public:
        JString(JNIEnv *env, jstring str) : m_env(env), m_str(str),
            m_chars(str ? env->GetStringUTFChars(str, nullptr) : nullptr) { }
        ~JString()
        {
            if (m_chars)
                m_env->ReleaseStringUTFChars(m_str, m_chars);
        }
        JString(const JString &) = delete;
        JString &operator=(const JString &) = delete;

        std::string str() const { return m_chars ? std::string(m_chars) : std::string(); }
    private:
        JNIEnv *m_env;
        jstring m_str;
        const char *m_chars;
    };

    std::vector<uintptr_t> to_vector(JNIEnv *env, jlongArray array)
    {
        std::vector<uintptr_t> out;
        if (!array)
            return out;
        jsize len = env->GetArrayLength(array);
        if (len > 0)
        {
            out.resize(static_cast<size_t>(len));
            env->GetLongArrayRegion(array, 0, len, reinterpret_cast<jlong *>(out.data()));
        }
        return out;
    }

    jlongArray to_jarray(JNIEnv *env, const std::vector<uintptr_t> &values)
    {
        jlongArray addresses = env->NewLongArray(static_cast<jsize>(values.size()));
        if (addresses && !values.empty())
            env->SetLongArrayRegion(addresses, 0, static_cast<jsize>(values.size()), reinterpret_cast<const jlong *>(values.data()));
        return addresses;
    }

    // Reads straight into the Java array (no intermediate buffer); unread bytes are zeroed.
    void read_into(JNIEnv *env, jbyteArray array, jlong address, jsize length)
    {
        if (length <= 0)
            return;

        void *data = env->GetPrimitiveArrayCritical(array, nullptr);
        if (!data)
            return;

        ssize_t n = ProcUtil::ReadMemoryBytes(client.FlashPid(), static_cast<uintptr_t>(address), data, static_cast<size_t>(length));
        size_t got = n > 0 ? static_cast<size_t>(n) : 0;
        if (got < static_cast<size_t>(length))
            std::memset(static_cast<uint8_t *>(data) + got, 0, static_cast<size_t>(length) - got);

        env->ReleasePrimitiveArrayCritical(array, data, 0);
    }
}

__attribute__((constructor)) static void lib_ctor()
{
    utils::log_timestamp_str(); // force timestamp init before any log call
    atexit([] { client.Shutdown(); });
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_setData
  (JNIEnv *env, jobject, jstring jurl, jstring jsid, jstring preloader, jstring vars)
{
    client.SetCredentials(JString(env, jsid).str(), JString(env, jurl).str());
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_createWindow
  (JNIEnv *env, jobject)
{
    client.LaunchBrowser();
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_setSize
  (JNIEnv *, jobject, jint jw, jint jh)
{
    client.SendBrowserCommand("setSize", {{"w", std::to_string(jw)}, {"h", std::to_string(jh)}});
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
    client.Refresh();
}

JNIEXPORT jboolean JNICALL Java_eu_darkbot_api_DarkTanos_isValid
  (JNIEnv *, jobject)
{
    return client.IsValid();
}

JNIEXPORT jlong JNICALL Java_eu_darkbot_api_DarkTanos_getMemoryUsage
  (JNIEnv *, jobject)
{
    pid_t pid = client.FlashPid() > 0 ? client.FlashPid() : client.Pid();
    return ProcUtil::GetMemoryUsage(pid) / 1024;
}

JNIEXPORT jdouble JNICALL Java_eu_darkbot_api_DarkTanos_getCpuUsage
  (JNIEnv *, jobject)
{
    pid_t pid = client.FlashPid() > 0 ? client.FlashPid() : client.Pid();
    return ProcUtil::GetCpuUsage(pid);
}

JNIEXPORT jint JNICALL Java_eu_darkbot_api_DarkTanos_getVersion
  (JNIEnv *, jobject)
{
    return API_VERSION;
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_keyClick
  (JNIEnv *, jobject, jint c)
{
    // Use legacy flash key click path for basic actions like attack, jump, and other simple keys.
    client.KeyClickLegacy(c);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_sendText
  (JNIEnv *env, jobject, jstring jtext)
{
    client.SendText(JString(env, jtext).str());
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_pasteText
  (JNIEnv *env, jobject, jstring jtext, jlongArray jactions)
{
    auto values = to_vector(env, jactions);
    std::vector<uint64_t> actions(values.begin(), values.end());

    client.PasteText(JString(env, jtext).str(), actions);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_mouseMove
  (JNIEnv *, jobject, jint x, jint y)
{
    client.MouseMove(x, y);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_mouseDown
  (JNIEnv *, jobject, jint x, jint y)
{
    client.MouseDown(x, y);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_mouseUp
  (JNIEnv *, jobject, jint x, jint y)
{
    client.MouseUp(x, y);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_mouseClick
  (JNIEnv *, jobject, jint x, jint y)
{
    client.MouseClick(x, y);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_setCursorMarker
  (JNIEnv *, jobject, jboolean enable)
{
    // Show red dot at the cursor position (useful for debugging)
    client.EnableCursorMarker(enable);
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_postActions
  (JNIEnv *env, jobject, jlongArray jactions)
{
    auto values = to_vector(env, jactions);
    client.PostActions(std::vector<uint64_t>(values.begin(), values.end()));
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
  (JNIEnv *env, jobject, jlong jaddr, jint jlength)
{
    jsize size = jlength > 0 ? jlength : 0;
    jbyteArray barray = env->NewByteArray(size);
    if (barray)
        read_into(env, barray, jaddr, size);
    return barray;
}

JNIEXPORT void JNICALL Java_eu_darkbot_api_DarkTanos_readBytes__J_3BI
  (JNIEnv *env, jobject, jlong jaddr, jbyteArray jout, jint jlength)
{
    if (!jout)
        return;
    // previously read the whole buffer (not |length|) and threw when length > buffer length
    jsize size = std::min<jsize>(jlength, env->GetArrayLength(jout));
    read_into(env, jout, jaddr, size);
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
    if (client.Read<jlong>(jaddr) == jold)
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
    if (!jval)
        return;
    jsize size = env->GetArrayLength(jval);
    if (size <= 0)
        return;

    void *data = env->GetPrimitiveArrayCritical(jval, nullptr);
    if (!data)
        return;
    ProcUtil::WriteMemoryBytes(client.FlashPid(), static_cast<uintptr_t>(jaddr), data, static_cast<size_t>(size));
    env->ReleasePrimitiveArrayCritical(jval, data, JNI_ABORT);
}

JNIEXPORT jlongArray JNICALL Java_eu_darkbot_api_DarkTanos_queryInt
  (JNIEnv *env, jobject, jint jquery, jint jamount)
{
    if (jamount <= 0)
        return env->NewLongArray(0);
    auto out = client.QueryMemory(reinterpret_cast<const uint8_t *>(&jquery), sizeof(jquery), static_cast<size_t>(jamount));
    return to_jarray(env, out);
}

JNIEXPORT jlongArray JNICALL Java_eu_darkbot_api_DarkTanos_queryLong
  (JNIEnv *env, jobject, jlong jquery, jint jamount)
{
    if (jamount <= 0)
        return env->NewLongArray(0);
    auto out = client.QueryMemory(reinterpret_cast<const uint8_t *>(&jquery), sizeof(jquery), static_cast<size_t>(jamount));
    return to_jarray(env, out);
}

JNIEXPORT jlongArray JNICALL Java_eu_darkbot_api_DarkTanos_queryBytes
  (JNIEnv * env, jobject, jbyteArray jquery, jint jamount)
{
    jsize query_size = jquery ? env->GetArrayLength(jquery) : 0;
    if (query_size <= 0 || jamount <= 0)
        return env->NewLongArray(0);

    std::vector<uint8_t> query(static_cast<size_t>(query_size));
    env->GetByteArrayRegion(jquery, 0, query_size, reinterpret_cast<jbyte*>(query.data()));

    auto out = client.QueryMemory(query.data(), query.size(), static_cast<size_t>(jamount));
    return to_jarray(env, out);
}


JNIEXPORT jboolean JNICALL Java_eu_darkbot_api_DarkTanos_sendNotification
  (JNIEnv *env, jobject, jlong screen_manager, jstring jname, jlongArray jargs)
{
    return client.SendNotification(screen_manager, JString(env, jname).str(), to_vector(env, jargs));
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
    return client.UseItem(JString(env, jname).str(), 1, 0);
}

JNIEXPORT jlong JNICALL Java_eu_darkbot_api_DarkTanos_callMethod
  (JNIEnv *env, jobject, jlong jthis, jint jindex, jlongArray jargs)
{
    return client.CallMethod(jthis, jindex, to_vector(env, jargs));
}

JNIEXPORT jint JNICALL Java_eu_darkbot_api_DarkTanos_checkMethodSignature
  (JNIEnv *env, jobject, jlong object, jint index, jboolean check_name, jstring sig)
{
    return client.CheckMethodSignature(object, index, check_name, JString(env, sig).str());
}

