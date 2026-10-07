package top.niunaijun.blackbox.core.system;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;

import java.io.IOException;
import java.net.Authenticator;
import java.net.InetSocketAddress;
import java.net.PasswordAuthentication;
import java.net.Proxy;
import java.net.ProxySelector;
import java.net.SocketAddress;
import java.net.URI;
import java.util.Collections;
import java.util.List;

import top.niunaijun.blackbox.BlackBoxCore;
import top.niunaijun.blackbox.proxy.ProxyManifest;
import top.niunaijun.blackbox.utils.Slog;
import top.niunaijun.blackbox.utils.compat.ContentProviderCompat;

/**
 * Per-clone proxy. Config is stored by SystemCallProvider (black process),
 * read by clone processes at startup, edited from the host UI.
 * Covers Java-level networking (HttpURLConnection, OkHttp, SOCKS sockets using ProxySelector).
 * Native sockets / some WebViews bypass it.
 */
public class CloneProxy {
    private static final String TAG = "CloneProxy";
    private static final String PREF = "clone_proxy";

    public static final String METHOD_GET = "PROXY_GET";
    public static final String METHOD_SET = "PROXY_SET";

    // ---------- provider side (black process) ----------
    static Bundle handle(Context ctx, String method, Bundle extras) {
        Bundle out = new Bundle();
        if (extras == null) return out;
        int userId = extras.getInt("userId", 0);
        String pkg = extras.getString("pkg", "");
        String key = userId + ":" + pkg;
        SharedPreferences sp = ctx.getSharedPreferences(PREF, Context.MODE_PRIVATE);
        if (METHOD_SET.equals(method)) {
            String host = extras.getString("host", "");
            if (host == null || host.trim().isEmpty()) {
                sp.edit().remove(key).apply();
            } else {
                sp.edit().putString(key, host.trim() + "\n" + extras.getInt("port", 0) + "\n"
                        + extras.getString("type", "HTTP") + "\n"
                        + extras.getString("user", "") + "\n"
                        + extras.getString("pass", "") + "\n"
                        + (extras.getBoolean("dns", false) ? "1" : "0")).apply();
            }
        } else {
            String raw = sp.getString(key, null);
            if (raw != null) {
                String[] p = raw.split("\n", -1);
                if (p.length >= 5) {
                    out.putString("host", p[0]);
                    out.putInt("port", Integer.parseInt(p[1]));
                    out.putString("type", p[2]);
                    out.putString("user", p[3]);
                    out.putString("pass", p[4]);
                    out.putBoolean("dns", p.length >= 6 && "1".equals(p[5]));
                }
            }
        }
        return out;
    }

    // ---------- client side (host UI or clone process) ----------
    public static Bundle remoteGet(Context ctx, int userId, String pkg) {
        Bundle b = new Bundle();
        b.putInt("userId", userId);
        b.putString("pkg", pkg);
        return remote(ctx, METHOD_GET, b);
    }

    public static void remoteSet(Context ctx, int userId, String pkg, String host, int port,
                                 String type, String user, String pass, boolean dns) {
        Bundle b = new Bundle();
        b.putInt("userId", userId);
        b.putString("pkg", pkg);
        b.putString("host", host);
        b.putInt("port", port);
        b.putString("type", type);
        b.putString("user", user);
        b.putString("pass", pass);
        b.putBoolean("dns", dns);
        remote(ctx, METHOD_SET, b);
    }

    private static Bundle remote(Context ctx, String method, Bundle extras) {
        try {
            Uri uri = Uri.parse("content://" + ProxyManifest.getBindProvider());
            Bundle r = ContentProviderCompat.call(ctx, uri, method, null, extras, 5);
            return r == null ? new Bundle() : r;
        } catch (Throwable e) {
            Slog.w(TAG, "remote " + method + " failed: " + e.getMessage());
            return new Bundle();
        }
    }

    // ---------- clone process: apply ----------
    private static boolean tryNative(final String host, final int port, final boolean socks,
                                     final String user, final String pass, final boolean dns) {
        final String[] ip = new String[1];
        Thread t = new Thread(() -> {
            try {
                for (java.net.InetAddress a : java.net.InetAddress.getAllByName(host)) {
                    if (a instanceof java.net.Inet4Address) { ip[0] = a.getHostAddress(); return; }
                }
            } catch (Throwable ignored) { }
        });
        t.start();
        try { t.join(6000); } catch (InterruptedException ignored) { }
        if (ip[0] == null) return false;
        try {
            return top.niunaijun.blackbox.core.NativeCore.enableNetProxy(ip[0], port, socks, user, pass, dns);
        } catch (Throwable e) {
            Slog.w(TAG, "native proxy failed: " + e);
            return false;
        }
    }

    public static void applyInProcess(int userId, String pkg) {
        try {
            Bundle cfg = remoteGet(BlackBoxCore.getContext(), userId, pkg);
            final String host = cfg.getString("host", "");
            if (host == null || host.isEmpty()) return;
            final int port = cfg.getInt("port", 0);
            final boolean socks = "SOCKS5".equals(cfg.getString("type", "HTTP"));
            final String user = cfg.getString("user", "");
            final String pass = cfg.getString("pass", "");

            // 1) native: tunnel ALL TCP (native libs, WebView, Java sockets)
            if (tryNative(host, port, socks, user, pass, cfg.getBoolean("dns", false))) {
                Slog.d(TAG, "native proxy active for " + pkg + " -> " + host + ":" + port);
                return;
            }

            // 2) fallback: Java-level proxy only
            final Proxy proxy = new Proxy(socks ? Proxy.Type.SOCKS : Proxy.Type.HTTP,
                    InetSocketAddress.createUnresolved(host, port));

            ProxySelector.setDefault(new ProxySelector() {
                @Override
                public List<Proxy> select(URI uri) {
                    String h = uri == null ? null : uri.getHost();
                    if (h != null && (h.equals("localhost") || h.equals("127.0.0.1") || h.equals("::1"))) {
                        return Collections.singletonList(Proxy.NO_PROXY);
                    }
                    return Collections.singletonList(proxy);
                }

                @Override
                public void connectFailed(URI uri, SocketAddress sa, IOException ioe) {
                    Slog.w(TAG, "proxy connect failed: " + ioe);
                }
            });

            if (!user.isEmpty()) {
                Authenticator.setDefault(new Authenticator() {
                    @Override
                    protected PasswordAuthentication getPasswordAuthentication() {
                        return new PasswordAuthentication(user, pass.toCharArray());
                    }
                });
            }

            if (socks) {
                System.setProperty("socksProxyHost", host);
                System.setProperty("socksProxyPort", String.valueOf(port));
            } else {
                System.setProperty("http.proxyHost", host);
                System.setProperty("http.proxyPort", String.valueOf(port));
                System.setProperty("https.proxyHost", host);
                System.setProperty("https.proxyPort", String.valueOf(port));
            }
            Slog.d(TAG, "proxy applied for " + pkg + " user " + userId + " -> " + host + ":" + port);
        } catch (Throwable t) {
            Slog.w(TAG, "applyInProcess failed: " + t);
        }
    }
}
