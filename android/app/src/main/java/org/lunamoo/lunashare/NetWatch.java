package org.lunamoo.lunashare;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;

/**
 * Tells the engine the moment the phone's network changes - Wi-Fi to mobile data and back, another Wi-Fi, the
 * network coming back after none - so it reconnects at once (Engine::NetworkChanged) instead of after timeouts.
 * The default network switching is reported as such ("force"): the phone's addresses don't always show it (a Wi-Fi
 * without internet stays connected while Android sends everything over mobile data). Once per process.
 */
final class NetWatch {
    static native void nativeNetworkChanged(boolean force);

    private static boolean started = false;
    private static boolean seen = false;  // (the callback reports the current network once when registered: not a change)
    private static Network current;

    static synchronized void start(Context c) {
        if (started || c == null) return;
        try {
            ConnectivityManager cm = (ConnectivityManager) c.getSystemService(Context.CONNECTIVITY_SERVICE);
            if (cm == null) return;
            cm.registerDefaultNetworkCallback(new ConnectivityManager.NetworkCallback() {
                @Override
                public void onAvailable(Network n) {
                    boolean change;
                    synchronized (NetWatch.class) {
                        change = seen && !n.equals(current);
                        seen = true;
                        current = n;
                    }
                    if (change) tell(true);
                }

                @Override
                public void onLost(Network n) {
                    boolean change;
                    synchronized (NetWatch.class) {
                        change = n.equals(current);
                        if (change) current = null;
                    }
                    if (change) tell(true);
                }

                @Override
                public void onLinkPropertiesChanged(Network n, LinkProperties lp) {
                    boolean mine;
                    synchronized (NetWatch.class) {
                        mine = seen && n.equals(current);
                    }
                    if (mine) tell(false);  // (the engine looks whether its addresses changed)
                }
            });
            started = true;
        } catch (Throwable t) {
            android.util.Log.e("LunaShare", "network watch: " + t);
        }
    }

    private static void tell(boolean force) {
        try {
            nativeNetworkChanged(force);
        } catch (Throwable ignored) {
        }
    }
}
