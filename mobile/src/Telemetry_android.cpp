// Android crash reports: sentry-android (with its NDK part, so crashes in the
// native libjuce_jni.so are caught too), started from C++ through the Java
// class com.spectrocraft.mobile.Telemetry (mobile/android/app/src/main/java).
// The class is loaded through the app's class loader, which works from any
// thread JUCE calls us on.
#define JUCE_CORE_INCLUDE_JNI_HELPERS 1
#include "Telemetry.h"

namespace scm::telemetry::platform
{
namespace
{
/** Calls a static method of com.spectrocraft.mobile.Telemetry; false on failure. */
template <typename... Args>
bool callStatic (const char* name, const char* signature, Args... args)
{
    auto* env = juce::getEnv();
    if (env == nullptr) return false;
    auto context = juce::getAppContext();
    if (context.get() == nullptr) return false;

    const auto clear = [env]
    {
        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); return true; }
        return false;
    };
    juce::LocalRef<jclass> contextClass (env->GetObjectClass (context.get()));
    const auto getLoader = env->GetMethodID (contextClass.get(), "getClassLoader", "()Ljava/lang/ClassLoader;");
    juce::LocalRef<jobject> loader (env->CallObjectMethod (context.get(), getLoader));
    if (clear() || loader.get() == nullptr) return false;
    juce::LocalRef<jclass> loaderClass (env->FindClass ("java/lang/ClassLoader"));
    const auto loadClass = env->GetMethodID (loaderClass.get(), "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    juce::LocalRef<jstring> className (env->NewStringUTF ("com.spectrocraft.mobile.Telemetry"));
    juce::LocalRef<jclass> cls ((jclass) env->CallObjectMethod (loader.get(), loadClass, className.get()));
    if (clear() || cls.get() == nullptr) return false;
    const auto method = env->GetStaticMethodID (cls.get(), name, signature);
    if (clear() || method == nullptr) return false;
    env->CallStaticVoidMethod (cls.get(), method, args...);
    return ! clear();
}

jstring str (JNIEnv* env, const juce::String& s) { return env->NewStringUTF (s.toRawUTF8()); }
} // namespace

void startCrashReports (const juce::String& dsn, const juce::String& release, const juce::String& environment)
{
    auto* env = juce::getEnv();
    if (env == nullptr) return;
    auto context = juce::getAppContext();
    juce::LocalRef<jstring> d (str (env, dsn)), r (str (env, release)), e (str (env, environment));
    callStatic ("start", "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
                context.get(), d.get(), r.get(), e.get());
}

void stopCrashReports()
{
    callStatic ("stop", "()V");
}

void crashReportMessage (const juce::String& text)
{
    auto* env = juce::getEnv();
    if (env == nullptr) return;
    juce::LocalRef<jstring> t (str (env, text));
    callStatic ("message", "(Ljava/lang/String;)V", t.get());
}
} // namespace scm::telemetry::platform
