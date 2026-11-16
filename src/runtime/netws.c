/* netws.c - netws.h on Android (the game's Java NetWs over JNI), and the
 * stand-ins elsewhere (weak, so netws_apple.m's take over on macOS) */
#include "netws.h"
#include <string.h>

#if defined(__ANDROID__)
#include <jni.h>
#include <stdio.h>
#include <SDL.h>

static jclass g_cls;
static jmethodID g_open, g_state, g_send, g_recv, g_close;

/* the class beside the game's activity (org.lindbergh.<game>.NetWs), found
 * through the activity's own class loader - FindClass from a native thread
 * only sees the system's classes */
static int bind_class(JNIEnv *env)
{
    if (g_cls) return 1;
    jobject act = (jobject)SDL_AndroidGetActivity();
    if (!act) return 0;
    jclass ac = (*env)->GetObjectClass(env, act);
    jmethodID gcl = (*env)->GetMethodID(env, (*env)->FindClass(env, "java/lang/Class"), "getClassLoader", "()Ljava/lang/ClassLoader;");
    jmethodID gname = (*env)->GetMethodID(env, (*env)->FindClass(env, "java/lang/Class"), "getName", "()Ljava/lang/String;");
    jobject loader = (*env)->CallObjectMethod(env, ac, gcl);
    jstring jn = (jstring)(*env)->CallObjectMethod(env, ac, gname);
    const char *an = (*env)->GetStringUTFChars(env, jn, NULL);
    char name[256]; snprintf(name, sizeof name, "%s", an);
    (*env)->ReleaseStringUTFChars(env, jn, an);
    char *dot = strrchr(name, '.'); if (dot) snprintf(dot + 1, sizeof name - (size_t)(dot + 1 - name), "NetWs");
    jmethodID lc = (*env)->GetMethodID(env, (*env)->FindClass(env, "java/lang/ClassLoader"), "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring js = (*env)->NewStringUTF(env, name);
    jclass c = (jclass)(*env)->CallObjectMethod(env, loader, lc, js);
    if ((*env)->ExceptionCheck(env) || !c) { (*env)->ExceptionClear(env); fprintf(stderr, "[netws] no %s in this app\n", name); return 0; }
    g_cls = (jclass)(*env)->NewGlobalRef(env, c);
    g_open = (*env)->GetStaticMethodID(env, g_cls, "open", "(Ljava/lang/String;)V");
    g_state = (*env)->GetStaticMethodID(env, g_cls, "state", "()I");
    g_send = (*env)->GetStaticMethodID(env, g_cls, "send", "([B)Z");
    g_recv = (*env)->GetStaticMethodID(env, g_cls, "recv", "()[B");
    g_close = (*env)->GetStaticMethodID(env, g_cls, "close", "()V");
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); g_cls = NULL; return 0; }
    return 1;
}
static JNIEnv *env_of(void) { return (JNIEnv *)SDL_AndroidGetJNIEnv(); }

int lgws_open(const char *url)
{
    JNIEnv *env = env_of();
    if (!env || !bind_class(env)) return -1;
    jstring js = (*env)->NewStringUTF(env, url);
    (*env)->CallStaticVoidMethod(env, g_cls, g_open, js);
    (*env)->DeleteLocalRef(env, js);
    return 0;
}
int lgws_state(void)
{
    JNIEnv *env = env_of();
    if (!env || !g_cls) return -1;
    return (*env)->CallStaticIntMethod(env, g_cls, g_state);
}
int lgws_send(const uint8_t *b, size_t n)
{
    JNIEnv *env = env_of();
    if (!env || !g_cls) return 0;
    jbyteArray a = (*env)->NewByteArray(env, (jsize)n);
    (*env)->SetByteArrayRegion(env, a, 0, (jsize)n, (const jbyte *)b);
    jboolean ok = (*env)->CallStaticBooleanMethod(env, g_cls, g_send, a);
    (*env)->DeleteLocalRef(env, a);
    return ok ? 1 : 0;
}
int lgws_recv(uint8_t *b, size_t cap)
{
    JNIEnv *env = env_of();
    if (!env || !g_cls) return 0;
    jbyteArray a = (jbyteArray)(*env)->CallStaticObjectMethod(env, g_cls, g_recv);
    if (!a) return 0;
    jsize n = (*env)->GetArrayLength(env, a);
    if ((size_t)n > cap) n = (jsize)cap;
    (*env)->GetByteArrayRegion(env, a, 0, n, (jbyte *)b);
    (*env)->DeleteLocalRef(env, a);
    return (int)n;
}
void lgws_close(void)
{
    JNIEnv *env = env_of();
    if (env && g_cls) (*env)->CallStaticVoidMethod(env, g_cls, g_close);
}
#else
#define WEAK __attribute__((weak))
WEAK int  lgws_open(const char *url) { (void)url; return -1; }
WEAK int  lgws_state(void) { return -1; }
WEAK int  lgws_send(const uint8_t *b, size_t n) { (void)b; (void)n; return 0; }
WEAK int  lgws_recv(uint8_t *b, size_t cap) { (void)b; (void)cap; return 0; }
WEAK void lgws_close(void) {}
#endif
