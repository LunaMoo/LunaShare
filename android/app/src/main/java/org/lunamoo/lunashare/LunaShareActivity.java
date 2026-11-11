package org.lunamoo.lunashare;

import android.Manifest;
import android.annotation.SuppressLint;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothServerSocket;
import android.bluetooth.BluetoothSocket;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.Uri;
import android.net.wifi.SoftApConfiguration;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiManager;
import android.net.wifi.WifiNetworkSpecifier;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.Settings;
import android.webkit.MimeTypeMap;
import android.widget.Toast;

import androidx.core.content.FileProvider;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.UUID;

/**
 * Hosts the native LunaShare UI (SDL2 + Dear ImGui) and provides the Android-only pieces the C++
 * engine cannot do itself: Bluetooth RFCOMM sockets, LocalOnlyHotspot, joining a Wi-Fi network,
 * the multicast lock for LAN discovery, permissions, and opening files with other apps.
 */
public class LunaShareActivity extends SDLActivity {
    // ---- system text fallback: characters none of the app's fonts have (e.g. Japanese track names)
    private static final android.graphics.Paint textPaint = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
    private static float textScale = 0;

    /** Stable id of this phone for this app (survives reinstalling; changes with a factory reset). */
    public static String machineId() {
        try {
            String id = android.provider.Settings.Secure.getString(getContext().getContentResolver(),
                    android.provider.Settings.Secure.ANDROID_ID);
            return id == null || id.equals("9774d56d682e549c") ? "" : id;  // (a value broken old phones share)
        } catch (Throwable t) {
            return "";
        }
    }

    public static boolean canDrawText(String s) {
        try {
            return android.os.Build.VERSION.SDK_INT < 23 || textPaint.hasGlyph(s);
        } catch (Throwable t) {
            return false;
        }
    }

    /** {w, h, left, top (from the baseline), advance*64, alpha...}; size is ImGui's pixel height (ascent - descent). */
    public static int[] renderText(String s, float size) {
        try {
            if (textScale == 0) {
                textPaint.setTextSize(100);
                android.graphics.Paint.FontMetrics fm = textPaint.getFontMetrics();
                float hgt = fm.descent - fm.ascent;
                textScale = hgt > 1 ? 100f / hgt : 0.85f;
            }
            textPaint.setTextSize(size * textScale);
            textPaint.setColor(0xFFFFFFFF);
            android.graphics.Rect b = new android.graphics.Rect();
            textPaint.getTextBounds(s, 0, s.length(), b);
            float adv = textPaint.measureText(s);
            int w = b.isEmpty() ? 0 : b.width() + 2, h = b.isEmpty() ? 0 : b.height() + 2;
            int[] out = new int[5 + w * h];
            out[0] = w;
            out[1] = h;
            out[2] = b.left - 1;
            out[3] = b.top - 1;
            out[4] = Math.round(adv * 64);
            if (w > 0) {
                android.graphics.Bitmap bmp = android.graphics.Bitmap.createBitmap(w, h, android.graphics.Bitmap.Config.ARGB_8888);
                new android.graphics.Canvas(bmp).drawText(s, 1 - b.left, 1 - b.top, textPaint);
                int[] px = new int[w * h];
                bmp.getPixels(px, 0, w, 0, 0, w, h);
                bmp.recycle();
                for (int k = 0; k < w * h; k++) out[5 + k] = px[k] >>> 24;
            }
            return out;
        } catch (Throwable t) {
            return null;
        }
    }

    static final UUID SERVICE_UUID = UUID.fromString("6c756e61-7368-6172-6500-000000000001");
    static LunaShareActivity self;

    static WifiManager.MulticastLock multicastLock;
    static WifiManager.LocalOnlyHotspotReservation hotspot;
    static volatile BluetoothServerSocket btServer;
    static ConnectivityManager.NetworkCallback joinCallback;

    // ---- natives implemented in src/app/android_bridge.cpp
    static native int nativeAdoptBluetooth(String addr, boolean initiator);
    static native void nativeOnHotspot(boolean active, String ssid, String pass, String error);
    static native void nativeOnError(String message);
    static native void nativeOnIntent(String action, String key);  // a notification was tapped (or "decline")
    static native void nativeMusic(int cmd, long arg);             // the music notification / lock screen / headset buttons
    static Context appCtx;

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        self = this;
        appCtx = getApplicationContext();
        super.onCreate(savedInstanceState);
        handleIntent(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleIntent(intent);
    }

    /** Files opened with LunaShare ("Open with") or shared to it ("Share"): their paths go to the native side - the
     *  real file where it can be read directly, otherwise a copy in the cache (incoming/). On a thread of its own. */
    void handleFiles(Intent intent) {
        final String a = intent.getAction();
        final ArrayList<Uri> uris = new ArrayList<>();
        String text = null;
        if (Intent.ACTION_VIEW.equals(a) && intent.getData() != null && "magnet".equalsIgnoreCase(intent.getData().getScheme())) {
            // a magnet link: the link itself goes on (no file behind it)
            final String link = intent.getData().toString();
            intent.setAction(null);
            try { nativeOnIntent("org.lunamoo.lunashare.VIEW", link); } catch (Throwable ignored) {}
            return;
        }
        if (Intent.ACTION_VIEW.equals(a)) {
            if (intent.getData() != null) uris.add(intent.getData());
        } else if (Intent.ACTION_SEND.equals(a)) {
            Uri u = intent.getParcelableExtra(Intent.EXTRA_STREAM);
            if (u != null) uris.add(u);
            else text = intent.getStringExtra(Intent.EXTRA_TEXT);
        } else if (Intent.ACTION_SEND_MULTIPLE.equals(a)) {
            ArrayList<Uri> l = intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM);
            if (l != null) uris.addAll(l);
        }
        intent.setAction(null);
        final String sharedText = text;
        final boolean view = Intent.ACTION_VIEW.equals(a);
        new Thread(() -> {
            if (sharedText != null && !sharedText.isEmpty()) {
                try { nativeOnIntent("org.lunamoo.lunashare.SENDTEXT", sharedText); } catch (Throwable ignored) {}
                return;
            }
            StringBuilder paths = new StringBuilder();
            File in = new File(getCacheDir(), "incoming");
            cleanIncoming(in);
            File batch = new File(in, Long.toString(System.currentTimeMillis()));
            for (Uri u : uris) {
                String p = localPath(u, batch);
                if (p != null) paths.append(p).append('\n');
            }
            if (paths.length() == 0) return;
            try { nativeOnIntent(view ? "org.lunamoo.lunashare.VIEW" : "org.lunamoo.lunashare.SEND", paths.toString()); } catch (Throwable ignored) {}
        }).start();
    }

    /** A content:// (or file://) address as a file: the file itself when it can be read, else copied into dir. */
    String localPath(Uri u, File dir) {
        try {
            if ("file".equals(u.getScheme())) return u.getPath();
            String name = null;
            try (android.database.Cursor c = getContentResolver().query(u, null, null, null, null)) {
                if (c != null && c.moveToFirst()) {
                    int di = c.getColumnIndex("_data");
                    if (di >= 0) {
                        String p = c.getString(di);
                        if (p != null && new File(p).canRead()) return p;  // (all-files access: the real one)
                    }
                    int ni = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME);
                    if (ni >= 0) name = c.getString(ni);
                }
            } catch (Throwable ignored) {}
            if (name == null || name.isEmpty()) {
                String last = u.getLastPathSegment();
                name = last != null ? last.replaceAll(".*/", "") : "file";
                String ext = MimeTypeMap.getSingleton().getExtensionFromMimeType(getContentResolver().getType(u));
                if (ext != null && !name.contains(".")) name += "." + ext;
            }
            name = name.replaceAll("[/\\\\:*?\"<>|]", "_");
            dir.mkdirs();
            File out = new File(dir, name);
            try (InputStream is = getContentResolver().openInputStream(u); OutputStream os = new FileOutputStream(out)) {
                if (is == null) return null;
                byte[] buf = new byte[1 << 16];
                int n;
                while ((n = is.read(buf)) > 0) os.write(buf, 0, n);
            }
            return out.getAbsolutePath();
        } catch (Throwable t) {
            return null;
        }
    }

    /** Copies from earlier days go. */
    static void cleanIncoming(File in) {
        File[] l = in.listFiles();
        if (l == null) return;
        long old = System.currentTimeMillis() - 24L * 3600 * 1000;
        for (File b : l)
            if (b.lastModified() < old) deleteTree(b);
    }

    static void deleteTree(File f) {
        File[] l = f.listFiles();
        if (l != null) for (File c : l) deleteTree(c);
        f.delete();
    }

    /** Notifications open the app with an action: open a chat, show the ringing call, answer it. */
    void handleIntent(Intent intent) {
        if (intent == null || intent.getAction() == null) return;
        String a = intent.getAction();
        if (Intent.ACTION_VIEW.equals(a) || Intent.ACTION_SEND.equals(a) || Intent.ACTION_SEND_MULTIPLE.equals(a)) {
            handleFiles(intent);
            return;
        }
        if (!a.startsWith("org.lunamoo.lunashare.")) return;
        if (a.equals(Notify.ACTION_CALL) || a.equals(Notify.ACTION_ANSWER)) showOverLockScreen(true);
        if (a.equals(Notify.ACTION_ANSWER)) Notify.cancelCallNotification();
        String key = intent.getStringExtra("key");
        try {
            nativeOnIntent(a, key == null ? "" : key);
        } catch (Throwable ignored) {}  // (native library not loaded yet: the app simply starts)
        intent.setAction(null);
    }

    /** During an incoming call: show the app over the lock screen and turn the screen on. */
    static void showOverLockScreen(final boolean on) {
        ui(() -> {
            LunaShareActivity a = self;
            if (a == null) return;
            if (Build.VERSION.SDK_INT >= 27) {
                a.setShowWhenLocked(on);
                a.setTurnScreenOn(on);
            } else if (on) {
                a.getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED | android.view.WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON);
            } else {
                a.getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED | android.view.WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON);
            }
        });
    }

    /** Watching a movie: sideways (even with rotation locked), no status / navigation bars, screen stays on. */
    private static boolean barsWereShown = true;
    private static int uiFlagsBefore = 0;

    @SuppressWarnings("deprecation")
    public static void movieMode(final boolean on) {
        ui(() -> {
            LunaShareActivity a = self;
            if (a == null) return;
            a.setRequestedOrientation(on ? android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                                         : android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
            android.view.Window w = a.getWindow();
            if (on) w.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            else w.clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            if (Build.VERSION.SDK_INT >= 30) {
                android.view.WindowInsetsController c = w.getInsetsController();
                android.view.WindowInsets wi = w.getDecorView().getRootWindowInsets();
                if (on) barsWereShown = wi == null || wi.isVisible(android.view.WindowInsets.Type.statusBars());
                if (c != null) {
                    if (on) {
                        c.setSystemBarsBehavior(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                        c.hide(android.view.WindowInsets.Type.systemBars());
                    } else if (barsWereShown) {  // (back to how the app was)
                        c.show(android.view.WindowInsets.Type.systemBars());
                    }
                }
            } else {
                android.view.View d = w.getDecorView();
                if (on) uiFlagsBefore = d.getSystemUiVisibility();
                d.setSystemUiVisibility(on ? android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | android.view.View.SYSTEM_UI_FLAG_FULLSCREEN
                                               | android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | android.view.View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                                               | android.view.View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | android.view.View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                                           : uiFlagsBefore);
            }
        });
    }

    /** How much of the app's height the on-screen keyboard covers (0..1), kept up to date by a layout listener. */
    private static volatile float kbFraction = 0;
    private static boolean kbListening = false;
    public static float keyboardFraction() {
        if (!kbListening) {
            kbListening = true;
            ui(() -> {
                LunaShareActivity a = self;
                if (a == null) { kbListening = false; return; }
                final android.view.View content = a.findViewById(android.R.id.content);
                if (content == null) { kbListening = false; return; }
                content.getViewTreeObserver().addOnGlobalLayoutListener(() -> {
                    android.graphics.Rect r = new android.graphics.Rect();
                    content.getWindowVisibleDisplayFrame(r);
                    int[] loc = new int[2];
                    content.getLocationOnScreen(loc);
                    int h = content.getHeight();
                    int covered = loc[1] + h - r.bottom;
                    float f = h > 0 ? (float)covered / h : 0;
                    kbFraction = f > 0.12f ? f : 0;  // (less: only the navigation bar)
                });
            });
        }
        return kbFraction;
    }

    // ---- picture in picture: a movie playing goes on in a small window when the app is left
    private static volatile boolean pipAllowed = false, inPip = false;
    private static volatile int pipW = 16, pipH = 9;

    private static android.app.PictureInPictureParams pipParams() {
        float r = pipH > 0 ? (float)pipW / pipH : 16f / 9f;
        r = Math.max(1f / 2.3f, Math.min(2.3f, r));  // (what Android takes)
        android.app.PictureInPictureParams.Builder b = new android.app.PictureInPictureParams.Builder()
            .setAspectRatio(new android.util.Rational(Math.round(r * 1000), 1000));
        if (Build.VERSION.SDK_INT >= 31) b.setAutoEnterEnabled(pipAllowed).setSeamlessResizeEnabled(true);
        return b.build();
    }

    /** Whether leaving the app now keeps the movie going in a small window, and the movie's shape. */
    public static void moviePip(final boolean allowed, final int w, final int h) {
        pipAllowed = allowed;
        if (w > 0 && h > 0) {
            pipW = w;
            pipH = h;
        }
        if (Build.VERSION.SDK_INT < 26) return;
        ui(() -> {
            LunaShareActivity a = self;
            if (a == null) return;
            try { a.setPictureInPictureParams(pipParams()); } catch (Exception e) { }
        });
    }

    /** The picture-in-picture button. */
    public static void enterPip() {
        if (Build.VERSION.SDK_INT < 26) return;
        ui(() -> {
            LunaShareActivity a = self;
            if (a == null) return;
            try { a.enterPictureInPictureMode(pipParams()); } catch (Exception e) { }
        });
    }

    public static boolean inPictureInPicture() { return inPip; }

    @Override
    protected void onUserLeaveHint() {
        super.onUserLeaveHint();
        // (Android 12+ enters by itself - setAutoEnterEnabled)
        if (pipAllowed && Build.VERSION.SDK_INT >= 26 && Build.VERSION.SDK_INT < 31) {
            try { enterPictureInPictureMode(pipParams()); } catch (Exception e) { }
        }
    }

    @Override
    public void onPictureInPictureModeChanged(boolean in, android.content.res.Configuration c) {
        super.onPictureInPictureModeChanged(in, c);
        inPip = in;
    }

    /** Media volume 0..1 (the movie player's swipe on the right). */
    // app lock: fingerprint / face (Android 10+), a success unlocks
    public static boolean biometricAvailable() {
        LunaShareActivity a = self;
        if (a == null || Build.VERSION.SDK_INT < 29) return false;
        try {
            android.hardware.biometrics.BiometricManager bm = a.getSystemService(android.hardware.biometrics.BiometricManager.class);
            if (bm == null) return false;
            int r = Build.VERSION.SDK_INT >= 30 ? bm.canAuthenticate(android.hardware.biometrics.BiometricManager.Authenticators.BIOMETRIC_WEAK)
                                                : bm.canAuthenticate();
            return r == android.hardware.biometrics.BiometricManager.BIOMETRIC_SUCCESS;
        } catch (Throwable t) {
            return false;
        }
    }
    public static void biometricAsk(final String title, final String cancel) {
        final LunaShareActivity a = self;
        if (a == null || Build.VERSION.SDK_INT < 29) return;
        a.runOnUiThread(() -> {
            try {
                android.hardware.biometrics.BiometricPrompt p = new android.hardware.biometrics.BiometricPrompt.Builder(a)
                    .setTitle(title)
                    .setNegativeButton(cancel, a.getMainExecutor(), (d, w) -> {})
                    .build();
                p.authenticate(new android.os.CancellationSignal(), a.getMainExecutor(),
                    new android.hardware.biometrics.BiometricPrompt.AuthenticationCallback() {
                        @Override public void onAuthenticationSucceeded(android.hardware.biometrics.BiometricPrompt.AuthenticationResult r) {
                            try { nativeOnIntent("UNLOCK", ""); } catch (Throwable ignored) {}
                        }
                    });
            } catch (Throwable ignored) {}
        });
    }
    public static void secureWindow(final boolean on) {
        final LunaShareActivity a = self;
        if (a == null) return;
        a.runOnUiThread(() -> {
            if (on) a.getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_SECURE);
            else a.getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_SECURE);
        });
    }

    // the update check (from a native background thread): the body, or null
    public static String httpsGet(String url) {
        try {
            java.net.HttpURLConnection c = (java.net.HttpURLConnection) new java.net.URL(url).openConnection();
            c.setConnectTimeout(10000);
            c.setReadTimeout(15000);
            c.setRequestProperty("User-Agent", "LunaShare");
            c.setRequestProperty("Accept", "application/vnd.github+json");
            if (c.getResponseCode() != 200) return null;
            try (java.io.InputStream in = c.getInputStream()) {
                java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream();
                byte[] b = new byte[8192];
                int n;
                while ((n = in.read(b)) > 0 && o.size() < (1 << 20)) o.write(b, 0, n);
                return o.toString("UTF-8");
            } finally {
                c.disconnect();
            }
        } catch (Exception e) {
            return null;
        }
    }

    public static float mediaVolume() {
        LunaShareActivity a = self;
        if (a == null) return -1;
        android.media.AudioManager am = (android.media.AudioManager)a.getSystemService(AUDIO_SERVICE);
        int max = am.getStreamMaxVolume(android.media.AudioManager.STREAM_MUSIC);
        return max > 0 ? (float)am.getStreamVolume(android.media.AudioManager.STREAM_MUSIC) / max : -1;
    }
    public static void setMediaVolume(float v) {
        LunaShareActivity a = self;
        if (a == null) return;
        android.media.AudioManager am = (android.media.AudioManager)a.getSystemService(AUDIO_SERVICE);
        int max = am.getStreamMaxVolume(android.media.AudioManager.STREAM_MUSIC);
        am.setStreamVolume(android.media.AudioManager.STREAM_MUSIC, Math.round(Math.max(0f, Math.min(1f, v)) * max), 0);
    }
    /** This window's brightness 0..1 (< 0: back to the system's) - the swipe on the left. */
    public static void setScreenBrightness(final float v) {
        ui(() -> {
            LunaShareActivity a = self;
            if (a == null) return;
            android.view.WindowManager.LayoutParams lp = a.getWindow().getAttributes();
            lp.screenBrightness = v < 0 ? android.view.WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE : Math.max(0.01f, Math.min(1f, v));
            a.getWindow().setAttributes(lp);
        });
    }

    /** While a movie shows: a tall movie upright, a wide one sideways (the "by the movie's shape" option). */
    public static void movieOrientation(final boolean portrait) {
        ui(() -> {
            LunaShareActivity a = self;
            if (a == null) return;
            a.setRequestedOrientation(portrait ? android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
                                               : android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        });
    }

    /** Back on the home screen: keep running in the background instead of closing (like other messengers). */
    public static void moveToBack() { ui(() -> { if (self != null) self.moveTaskToBack(true); }); }

    static Context appContext() { return appCtx != null ? appCtx : self.getApplicationContext(); }

    static Context ctx() { return appContext(); }

    static void ui(Runnable r) { new Handler(Looper.getMainLooper()).post(r); }

    static void toast(final String text) { ui(() -> Toast.makeText(ctx(), text, Toast.LENGTH_LONG).show()); }

    // ------------------------------------------------------------------ misc
    public static String deviceName() {
        try {
            String n = Settings.Global.getString(self.getContentResolver(), "device_name");
            if (n != null && !n.isEmpty()) return n;
        } catch (Exception ignored) {}
        String m = Build.MODEL;
        return m.startsWith(Build.MANUFACTURER) ? m : Build.MANUFACTURER + " " + m;
    }

    public static void acquireMulticastLock() {
        try {
            WifiManager wm = (WifiManager) ctx().getSystemService(Context.WIFI_SERVICE);
            multicastLock = wm.createMulticastLock("LunaShare discovery");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
        } catch (Exception e) {
            nativeOnError("multicast lock: " + e);
        }
    }

    public static void requestPermissions() {
        ui(() -> {
            ArrayList<String> want = new ArrayList<>();
            if (Build.VERSION.SDK_INT >= 31) {
                want.add(Manifest.permission.BLUETOOTH_CONNECT);
                want.add(Manifest.permission.BLUETOOTH_SCAN);
            }
            if (Build.VERSION.SDK_INT >= 33) {
                want.add(Manifest.permission.NEARBY_WIFI_DEVICES);
                want.add(Manifest.permission.POST_NOTIFICATIONS);
            } else {
                want.add(Manifest.permission.ACCESS_FINE_LOCATION);  // hotspot + BT scan on older versions
            }
            if (Build.VERSION.SDK_INT < 30) {
                want.add(Manifest.permission.READ_EXTERNAL_STORAGE);
                want.add(Manifest.permission.WRITE_EXTERNAL_STORAGE);
            }
            ArrayList<String> missing = new ArrayList<>();
            for (String p : want)
                if (self.checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) missing.add(p);
            if (!missing.isEmpty()) self.requestPermissions(missing.toArray(new String[0]), 1);
            // Sending arbitrary files and saving into Download/LunaShare needs "All files access" on Android 11+.
            if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
                try {
                    Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + self.getPackageName()));
                    self.startActivity(i);
                    toast("LunaShare needs \"All files access\" to send and save files");
                } catch (Exception e) {
                    self.startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                }
            }
        });
    }

    public static void openFile(String path) {
        ui(() -> {
            try {
                File f = new File(path);
                Uri uri = FileProvider.getUriForFile(ctx(), ctx().getPackageName() + ".files", f);
                String ext = MimeTypeMap.getFileExtensionFromUrl(f.getName());
                String mime = ext == null ? null : MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext.toLowerCase());
                Intent i = new Intent(Intent.ACTION_VIEW);
                i.setDataAndType(uri, mime != null ? mime : "*/*");
                i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
                self.startActivity(Intent.createChooser(i, f.getName()));
            } catch (Exception e) {
                toast("No app can open this file");
            }
        });
    }

    /** The system's share sheet with a text (an invite code, the link to get the app). */
    // this app's page in the system settings (to allow a permission that was refused for good)
    public static void openAppSettings() {
        ui(() -> {
            try {
                Intent i = new Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                                      android.net.Uri.fromParts("package", self.getPackageName(), null));
                i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                self.startActivity(i);
            } catch (Exception e) {
                toast("Can't open the settings");
            }
        });
    }

    public static void shareText(final String text) {
        ui(() -> {
            try {
                Intent i = new Intent(Intent.ACTION_SEND);
                i.setType("text/plain");
                i.putExtra(Intent.EXTRA_TEXT, text);
                Intent c = Intent.createChooser(i, null);
                c.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                self.startActivity(c);
            } catch (Exception e) {
                toast("No app to share with");
            }
        });
    }

    public static void openFolder(String path) {
        ui(() -> {
            try {
                Intent i = new Intent(Intent.ACTION_VIEW);
                i.setDataAndType(Uri.parse("content://com.android.externalstorage.documents/document/primary%3ADownload%2FLunaShare"),
                        "vnd.android.document/directory");
                i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                self.startActivity(i);
            } catch (Exception e) {
                toast("Saved in: " + path);
            }
        });
    }

    // ------------------------------------------------------------------ calls (used by the optional voice module)
    /** Voice-call audio mode: earpiece routing and the platform's own echo cancellation where available. */
    public static void callAudioMode(final boolean inCall) {
        ui(() -> {
            try {
                android.media.AudioManager am = (android.media.AudioManager) ctx().getSystemService(Context.AUDIO_SERVICE);
                am.setMode(inCall ? android.media.AudioManager.MODE_IN_COMMUNICATION : android.media.AudioManager.MODE_NORMAL);
                if (!inCall) {
                    if (Build.VERSION.SDK_INT >= 31) {
                        am.clearCommunicationDevice();
                    } else {
                        am.setSpeakerphoneOn(false);
                        am.stopBluetoothSco();
                        am.setBluetoothScoOn(false);
                    }
                }
                // SDL plays call audio on the media stream (OpenSL ES default), so the volume buttons must control
                // that one: with STREAM_VOICE_CALL they changed a stream we don't use and calls stayed quiet
                self.setVolumeControlStream(inCall ? android.media.AudioManager.STREAM_MUSIC : android.media.AudioManager.USE_DEFAULT_STREAM_TYPE);
            } catch (Exception e) {
                nativeOnError("audio mode: " + e.getMessage());
            }
        });
    }

    /** Plays audio/video files in chat (all builds). */
    public static Class<?> playerClass() { return PlayerEngine.class; }
    public static Class<?> emojiClass() { return EmojiRenderer.class; }

    /** The video call engine class, present only in the "calls" build. */
    public static Class<?> videoClass() {
        try {
            return Class.forName("org.lunamoo.lunashare.VideoEngine");
        } catch (Throwable t) {
            return null;
        }
    }

    private static String routeKind(int type) {
        switch (type) {
            case android.media.AudioDeviceInfo.TYPE_BUILTIN_EARPIECE: return "earpiece";
            case android.media.AudioDeviceInfo.TYPE_BUILTIN_SPEAKER: return "speaker";
            case android.media.AudioDeviceInfo.TYPE_BLUETOOTH_SCO:
            case android.media.AudioDeviceInfo.TYPE_BLUETOOTH_A2DP:
            case 26: /* TYPE_BLE_HEADSET */
            case 27: /* TYPE_BLE_SPEAKER */
                return "bluetooth";
            case android.media.AudioDeviceInfo.TYPE_WIRED_HEADSET:
            case android.media.AudioDeviceInfo.TYPE_WIRED_HEADPHONES:
                return "wired";
            case android.media.AudioDeviceInfo.TYPE_USB_HEADSET:
            case android.media.AudioDeviceInfo.TYPE_USB_DEVICE:
                return "usb";
            default: return null;
        }
    }

    /**
     * Where call audio can go right now. One line per route: "id\tkind\tname"; the first line is the id of the
     * current route. Android 12+ uses the communication-device API; older versions use speakerphone / Bluetooth SCO.
     */
    public static String callRoutes() {
        StringBuilder sb = new StringBuilder();
        try {
            android.media.AudioManager am = (android.media.AudioManager) ctx().getSystemService(Context.AUDIO_SERVICE);
            if (Build.VERSION.SDK_INT >= 31) {
                android.media.AudioDeviceInfo cur = am.getCommunicationDevice();
                sb.append(cur != null ? cur.getId() : -1).append('\n');
                java.util.HashSet<String> seen = new java.util.HashSet<>();
                for (android.media.AudioDeviceInfo d : am.getAvailableCommunicationDevices()) {
                    String kind = routeKind(d.getType());
                    if (kind == null) continue;
                    String name = d.getProductName() != null ? d.getProductName().toString() : "";
                    if (!seen.add(kind + "/" + name)) continue;
                    sb.append(d.getId()).append('\t').append(kind).append('\t').append(name.replace('\t', ' ').replace('\n', ' ')).append('\n');
                }
            } else {
                boolean bt = false, wired = false;
                String btName = "";
                for (android.media.AudioDeviceInfo d : am.getDevices(android.media.AudioManager.GET_DEVICES_OUTPUTS)) {
                    String k = routeKind(d.getType());
                    if ("bluetooth".equals(k)) { bt = true; if (d.getProductName() != null) btName = d.getProductName().toString(); }
                    if ("wired".equals(k) || "usb".equals(k)) wired = true;
                }
                int cur = am.isBluetoothScoOn() ? 3 : am.isSpeakerphoneOn() ? 2 : wired ? 4 : 1;
                sb.append(cur).append('\n');
                if (wired) sb.append("4\twired\t\n");
                else sb.append("1\tearpiece\t\n");
                sb.append("2\tspeaker\t\n");
                if (bt && am.isBluetoothScoAvailableOffCall()) sb.append("3\tbluetooth\t").append(btName).append('\n');
            }
        } catch (Exception e) {
            return "";
        }
        return sb.toString();
    }

    public static void setCallRoute(final int id) {
        ui(() -> {
            try {
                android.media.AudioManager am = (android.media.AudioManager) ctx().getSystemService(Context.AUDIO_SERVICE);
                if (Build.VERSION.SDK_INT >= 31) {
                    for (android.media.AudioDeviceInfo d : am.getAvailableCommunicationDevices())
                        if (d.getId() == id) { am.setCommunicationDevice(d); return; }
                } else if (id == 3) {
                    am.setSpeakerphoneOn(false);
                    am.startBluetoothSco();
                    am.setBluetoothScoOn(true);
                } else {
                    am.stopBluetoothSco();
                    am.setBluetoothScoOn(false);
                    am.setSpeakerphoneOn(id == 2);
                }
            } catch (Exception e) {
                nativeOnError("audio route: " + e.getMessage());
            }
        });
    }

    // ------------------------------------------------------------------ Bluetooth
    @SuppressLint("MissingPermission")
    public static boolean btAvailable() {
        BluetoothAdapter a = BluetoothAdapter.getDefaultAdapter();
        return a != null;
    }

    @SuppressLint("MissingPermission")
    public static boolean btSetListening(boolean on) {
        BluetoothAdapter a = BluetoothAdapter.getDefaultAdapter();
        if (!on) {
            try { if (btServer != null) btServer.close(); } catch (Exception ignored) {}
            btServer = null;
            return true;
        }
        if (a == null) return false;
        if (!a.isEnabled()) {
            ui(() -> {
                try { self.startActivity(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)); } catch (Exception ignored) {}
            });
        }
        try {
            btServer = a.listenUsingRfcommWithServiceRecord("LunaShare", SERVICE_UUID);
        } catch (Exception e) {
            nativeOnError("Bluetooth listen failed: " + e.getMessage());
            return false;
        }
        final BluetoothServerSocket server = btServer;
        new Thread(() -> {
            while (btServer == server) {
                try {
                    BluetoothSocket s = server.accept();
                    bridge(s, false);
                } catch (Exception e) {
                    break;
                }
            }
        }, "bt-accept").start();
        return true;
    }

    @SuppressLint("MissingPermission")
    public static String[] btDevices(boolean inquiry) {
        BluetoothAdapter a = BluetoothAdapter.getDefaultAdapter();
        if (a == null) return new String[0];
        final Map<String, String> found = new LinkedHashMap<>();
        try {
            for (BluetoothDevice d : a.getBondedDevices()) found.put(d.getAddress(), d.getName() + "|1");
        } catch (Exception ignored) {}
        if (inquiry) {
            BroadcastReceiver r = new BroadcastReceiver() {
                @Override
                public void onReceive(Context c, Intent i) {
                    BluetoothDevice d = i.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
                    if (d != null && !found.containsKey(d.getAddress())) found.put(d.getAddress(), d.getName() + "|0");
                }
            };
            ctx().registerReceiver(r, new IntentFilter(BluetoothDevice.ACTION_FOUND));
            try {
                a.startDiscovery();
                Thread.sleep(9000);
                a.cancelDiscovery();
            } catch (Exception ignored) {
            } finally {
                ctx().unregisterReceiver(r);
            }
        }
        ArrayList<String> out = new ArrayList<>();
        for (Map.Entry<String, String> e : found.entrySet()) out.add(e.getKey() + "|" + e.getValue());
        return out.toArray(new String[0]);
    }

    @SuppressLint("MissingPermission")
    public static void btConnect(final String addr) {
        new Thread(() -> {
            BluetoothAdapter a = BluetoothAdapter.getDefaultAdapter();
            try {
                a.cancelDiscovery();
                BluetoothSocket s = a.getRemoteDevice(addr).createRfcommSocketToServiceRecord(SERVICE_UUID);
                s.connect();
                bridge(s, true);
            } catch (Exception e) {
                nativeOnError("Bluetooth connect to " + addr + " failed - is LunaShare running there with Bluetooth enabled?");
            }
        }, "bt-connect").start();
    }

    /** Pumps bytes between the Bluetooth socket and a native socketpair end owned by the engine. */
    static void bridge(final BluetoothSocket bt, boolean initiator) {
        int fd = nativeAdoptBluetooth(bt.getRemoteDevice().getAddress(), initiator);
        if (fd < 0) {
            try { bt.close(); } catch (Exception ignored) {}
            return;
        }
        final ParcelFileDescriptor pfd = ParcelFileDescriptor.adoptFd(fd);
        final Runnable closeAll = () -> {
            try { bt.close(); } catch (Exception ignored) {}
            try { pfd.close(); } catch (Exception ignored) {}
        };
        try {
            final InputStream btIn = bt.getInputStream();
            final OutputStream btOut = bt.getOutputStream();
            final FileInputStream fdIn = new FileInputStream(pfd.getFileDescriptor());
            final FileOutputStream fdOut = new FileOutputStream(pfd.getFileDescriptor());
            new Thread(() -> pump(btIn, fdOut, closeAll), "bt-in").start();
            new Thread(() -> pump(fdIn, btOut, closeAll), "bt-out").start();
        } catch (Exception e) {
            closeAll.run();
        }
    }

    static void pump(InputStream in, OutputStream out, Runnable onEnd) {
        byte[] buf = new byte[32 * 1024];
        try {
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                out.flush();
            }
        } catch (Exception ignored) {
        } finally {
            onEnd.run();
        }
    }

    // ------------------------------------------------------------------ Wi-Fi hotspot
    @SuppressLint("MissingPermission")
    public static boolean hotspotStart() {
        if (Build.VERSION.SDK_INT < 26) {
            nativeOnHotspot(false, "", "", "needs Android 8 or newer");
            return false;
        }
        ui(() -> {
            try {
                WifiManager wm = (WifiManager) ctx().getSystemService(Context.WIFI_SERVICE);
                wm.startLocalOnlyHotspot(new WifiManager.LocalOnlyHotspotCallback() {
                    @Override
                    public void onStarted(WifiManager.LocalOnlyHotspotReservation r) {
                        hotspot = r;
                        String ssid = "", pass = "";
                        if (Build.VERSION.SDK_INT >= 30) {
                            SoftApConfiguration c = r.getSoftApConfiguration();
                            ssid = c.getSsid();
                            pass = c.getPassphrase();
                        } else {
                            WifiConfiguration c = r.getWifiConfiguration();
                            if (c != null) { ssid = c.SSID; pass = c.preSharedKey; }
                        }
                        if (ssid != null && ssid.startsWith("\"")) ssid = ssid.substring(1, ssid.length() - 1);
                        nativeOnHotspot(true, ssid == null ? "" : ssid, pass == null ? "" : pass, "");
                    }

                    @Override
                    public void onStopped() { nativeOnHotspot(false, "", "", ""); }

                    @Override
                    public void onFailed(int reason) {
                        String why = reason == ERROR_INCOMPATIBLE_MODE ? "turn off the regular hotspot / Wi-Fi tethering first"
                                   : reason == ERROR_NO_CHANNEL ? "no free Wi-Fi channel"
                                   : reason == ERROR_TETHERING_DISALLOWED ? "tethering is not allowed on this device"
                                   : "error " + reason;
                        nativeOnHotspot(false, "", "", why);
                    }
                }, new Handler(Looper.getMainLooper()));
            } catch (SecurityException e) {
                nativeOnHotspot(false, "", "", "permission missing (Location / Nearby devices)");
                requestPermissions();
            } catch (Exception e) {
                nativeOnHotspot(false, "", "", e.getMessage());
            }
        });
        return true;
    }

    public static void hotspotStop() {
        if (hotspot != null) hotspot.close();
        hotspot = null;
        nativeOnHotspot(false, "", "", "");
    }

    @SuppressLint("MissingPermission")
    public static void wifiJoin(final String ssid, final String pass) {
        ui(() -> {
            try {
                if (Build.VERSION.SDK_INT >= 29) {
                    WifiNetworkSpecifier.Builder b = new WifiNetworkSpecifier.Builder().setSsid(ssid);
                    if (pass != null && !pass.isEmpty()) b.setWpa2Passphrase(pass);
                    NetworkRequest req = new NetworkRequest.Builder()
                            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                            .setNetworkSpecifier(b.build())
                            .build();
                    final ConnectivityManager cm = (ConnectivityManager) ctx().getSystemService(Context.CONNECTIVITY_SERVICE);
                    if (joinCallback != null) try { cm.unregisterNetworkCallback(joinCallback); } catch (Exception ignored) {}
                    joinCallback = new ConnectivityManager.NetworkCallback() {
                        @Override
                        public void onAvailable(Network n) {
                            // route our sockets (discovery + transfers) through the hotspot network
                            cm.bindProcessToNetwork(n);
                            toast("Connected to " + ssid);
                        }

                        @Override
                        public void onLost(Network n) { cm.bindProcessToNetwork(null); }

                        @Override
                        public void onUnavailable() { nativeOnError("Could not join " + ssid); }
                    };
                    cm.requestNetwork(req, joinCallback);
                } else {
                    WifiManager wm = (WifiManager) ctx().getSystemService(Context.WIFI_SERVICE);
                    WifiConfiguration c = new WifiConfiguration();
                    c.SSID = "\"" + ssid + "\"";
                    c.preSharedKey = "\"" + pass + "\"";
                    int id = wm.addNetwork(c);
                    wm.disconnect();
                    wm.enableNetwork(id, true);
                    wm.reconnect();
                }
            } catch (Exception e) {
                nativeOnError("Joining Wi-Fi failed: " + e.getMessage());
            }
        });
    }
}
