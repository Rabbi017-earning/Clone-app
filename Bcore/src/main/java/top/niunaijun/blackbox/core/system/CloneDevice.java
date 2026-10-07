package top.niunaijun.blackbox.core.system;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;

import java.lang.reflect.Field;

import top.niunaijun.blackbox.BlackBoxCore;
import top.niunaijun.blackbox.core.NativeCore;
import top.niunaijun.blackbox.proxy.ProxyManifest;
import top.niunaijun.blackbox.utils.Slog;
import top.niunaijun.blackbox.utils.compat.ContentProviderCompat;

/** Per-clone device model/brand shown to the app (Build.* and ro.product.* properties). */
public class CloneDevice {
    private static final String TAG = "CloneDevice";
    private static final String PREF = "clone_device";
    public static final String METHOD_GET = "DEVICE_GET";
    public static final String METHOD_SET = "DEVICE_SET";

    // id, label, model, brand, manufacturer, device
    public static final String[][] PRESETS = {
            {"default", "Default (engine)", "", "", "", ""},
            {"pixel8", "Google Pixel 8", "Pixel 8", "google", "Google", "shiba"},
            {"s23", "Samsung Galaxy S23", "SM-S911B", "samsung", "samsung", "dm1q"},
            {"xiaomi13", "Xiaomi 13", "2211133G", "Xiaomi", "Xiaomi", "fuxi"},
            {"oneplus11", "OnePlus 11", "CPH2449", "OnePlus", "OnePlus", "OP594DL1"},
            {"redmi12", "Redmi Note 12", "23021RAAEG", "Redmi", "Xiaomi", "topaz"},
    };

    static Bundle handle(Context ctx, String method, Bundle extras) {
        Bundle out = new Bundle();
        if (extras == null) return out;
        String key = extras.getInt("userId", 0) + ":" + extras.getString("pkg", "");
        SharedPreferences sp = ctx.getSharedPreferences(PREF, Context.MODE_PRIVATE);
        if (METHOD_SET.equals(method)) {
            String id = extras.getString("preset", "default");
            if ("default".equals(id)) sp.edit().remove(key).apply();
            else sp.edit().putString(key, id).apply();
        } else {
            out.putString("preset", sp.getString(key, "default"));
        }
        return out;
    }

    public static String remoteGet(Context ctx, int userId, String pkg) {
        Bundle b = new Bundle();
        b.putInt("userId", userId);
        b.putString("pkg", pkg);
        return remote(ctx, METHOD_GET, b).getString("preset", "default");
    }

    public static void remoteSet(Context ctx, int userId, String pkg, String preset) {
        Bundle b = new Bundle();
        b.putInt("userId", userId);
        b.putString("pkg", pkg);
        b.putString("preset", preset);
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

    public static void applyInProcess(int userId, String pkg) {
        try {
            String id = remoteGet(BlackBoxCore.getContext(), userId, pkg);
            for (String[] p : PRESETS) {
                if (!p[0].equals(id) || "default".equals(id)) continue;
                setBuild("MODEL", p[2]);
                setBuild("BRAND", p[3]);
                setBuild("MANUFACTURER", p[4]);
                setBuild("DEVICE", p[5]);
                setBuild("PRODUCT", p[5]);
                NativeCore.setDeviceProfile(p[2], p[3], p[4], p[5]);
                Slog.d(TAG, "device profile " + id + " applied for " + pkg);
                return;
            }
        } catch (Throwable t) {
            Slog.w(TAG, "applyInProcess failed: " + t);
        }
    }

    private static void setBuild(String field, String value) {
        try {
            Field f = android.os.Build.class.getDeclaredField(field);
            f.setAccessible(true);
            f.set(null, value);
        } catch (Throwable t) {
            Slog.w(TAG, "Build." + field + " not set: " + t);
        }
    }
}
