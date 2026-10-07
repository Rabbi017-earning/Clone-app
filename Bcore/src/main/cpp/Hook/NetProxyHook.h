#ifndef BLACKBOX_NETPROXYHOOK_H
#define BLACKBOX_NETPROXYHOOK_H

#include <jni.h>

jboolean enableNetProxy(JNIEnv *env, jclass clazz, jstring ip, jint port, jboolean socks5,
                        jstring user, jstring pass, jboolean proxyDns);

#endif
