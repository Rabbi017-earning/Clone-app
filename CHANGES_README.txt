Base: ALEX5402/NewBlackbox (Apache-2.0). Added per-clone proxy.

NEW FILES
  Bcore/src/main/java/top/niunaijun/blackbox/core/system/CloneProxy.java
  app/src/main/java/top/niunaijun/blackboxa/view/proxy/ProxyActivity.kt
  .github/workflows/build_apk.yml   (replaces build_and_telegram.yml)

EDITED FILES
  Bcore/.../core/system/SystemCallProvider.java   (PROXY_GET / PROXY_SET)
  Bcore/.../app/BActivityThread.java              (apply proxy before app starts)
  app/src/main/AndroidManifest.xml                (ProxyActivity)
  app/src/main/res/menu/app_menu.xml              (Proxy Settings item)
  app/src/main/res/values/strings.xml             (app_proxy)
  app/.../view/apps/AppsFragment.kt               (menu click)

USE
  Clone app: tap + in app. Long-press clone > Proxy Settings (HTTP/SOCKS5, user/pass).
  Per-clone GPS: main menu > Fake Location (already in engine, per app).
  After saving proxy: long-press > Stop, then open clone again.

BUILD
  GitHub: push all files > Actions > Build APK > download artifact.
  Local: ./gradlew :app:assembleDebug  (JDK 21, NDK 29.0.13846066)

UPDATE 2: native proxy
  NEW  Bcore/src/main/cpp/Hook/NetProxyHook.cpp + .h   (libc connect() hook -> HTTP CONNECT / SOCKS5)
  EDIT Bcore/src/main/cpp/Android.mk, BoxCore.cpp       (register hook + JNI)
  EDIT Bcore/.../core/NativeCore.java                   (enableNetProxy)
  EDIT Bcore/.../core/system/CloneProxy.java            (native first, Java fallback)
  EDIT .github/workflows/build_apk.yml                  (release + debug APK)
  Release APK = minified, use this one. If it crashes, install the debug APK.

UPDATE 3: DNS through proxy + device model picker
  NEW  Bcore/.../core/system/CloneDevice.java      (per-clone model/brand: Pixel 8, S23, Xiaomi 13, OnePlus 11, Redmi Note 12)
  EDIT Hook/NetProxyHook.cpp/.h                    (getaddrinfo hook: fake-IP + CONNECT by hostname)
  EDIT Utils/VirtualSpoof.cpp, BoxCore.cpp         (settable device profile, JNI)
  EDIT NativeCore.java, CloneProxy.java, SystemCallProvider.java, BActivityThread.java
  EDIT app/.../view/proxy/ProxyActivity.kt         (proxy + DNS switch + device model spinner)
