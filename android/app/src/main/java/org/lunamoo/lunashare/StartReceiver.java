package org.lunamoo.lunashare;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Build;

/**
 * The phone was restarted, or LunaShare was updated: the background service is started, and with it the engine
 * (without a screen) - messages and calls arrive without opening the app first. The service itself decides whether
 * to stay: only with "Keep running in the background" on, and only for an app that was set up.
 */
public class StartReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context c, Intent intent) {
        String a = intent == null ? null : intent.getAction();
        if (!Intent.ACTION_BOOT_COMPLETED.equals(a) && !Intent.ACTION_MY_PACKAGE_REPLACED.equals(a)) return;
        if (Notify.nativeRunning) return;
        try {
            Intent i = new Intent(c, BackgroundService.class);
            if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i);
            else c.startService(i);
        } catch (Throwable t) {
            android.util.Log.e("LunaShare", "start after boot / update: " + t);
        }
    }
}
