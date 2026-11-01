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

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        self = this;
        super.onCreate(savedInstanceState);
    }

    static Context ctx() { return self.getApplicationContext(); }

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
                if (!inCall) am.setSpeakerphoneOn(false);
                self.setVolumeControlStream(inCall ? android.media.AudioManager.STREAM_VOICE_CALL : android.media.AudioManager.USE_DEFAULT_STREAM_TYPE);
            } catch (Exception e) {
                nativeOnError("audio mode: " + e.getMessage());
            }
        });
    }

    public static void setSpeakerphone(final boolean on) {
        ui(() -> {
            try {
                android.media.AudioManager am = (android.media.AudioManager) ctx().getSystemService(Context.AUDIO_SERVICE);
                am.setSpeakerphoneOn(on);
            } catch (Exception ignored) {}
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
