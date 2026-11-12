package org.lunamoo.lunashare;

import android.app.Notification;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/**
 * Foreground service for the time of a call. It does no work: it is what lets the call keep the microphone.
 * Since Android 11 an app whose window isn't on the screen - the screen went off or was locked, another app was
 * opened - gets silence instead of the microphone, unless it runs a foreground service of the "microphone" kind
 * started while the app was still in front. Without it the other side stopped hearing mid-call.
 */
public class CallService extends Service {
    static final int ID = 3;
    static volatile boolean wanted = false;   // a call is on
    static volatile boolean running = false;  // ... and the service has the microphone

    /** A call started / ended (UI thread). */
    static void set(boolean on) {
        wanted = on;
        apply();
    }

    /** Also when the app comes to the front: a service that couldn't start from the background can now. */
    static void apply() {
        try {
            Context c = Notify.ctx();
            if (c == null) return;
            Intent i = new Intent(c, CallService.class);
            if (!wanted) {
                if (running) c.stopService(i);
                running = false;
                return;
            }
            if (running) return;
            // (not yet allowed: asked for when the call opens the microphone, and tried again then)
            if (Build.VERSION.SDK_INT >= 23 && c.checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) return;
            // (startService, not startForegroundService: that one must end in a foreground service or the app is
            // closed - and Android may refuse this kind; from the background this simply fails, and is tried again)
            c.startService(i);
        } catch (Throwable t) {
            // not allowed right now (the app is in the background): tried again when it's in front
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (!wanted) {
            stopSelf();
            return START_NOT_STICKY;
        }
        try {
            Notify.channels(this);
            Notification.Builder b = Notify.builder(this, Notify.CH_ONGOING)
                .setSmallIcon(R.drawable.ic_stat_cow)
                .setContentTitle("LunaShare")
                .setContentText(getString(R.string.call_ongoing))
                .setOngoing(true)
                .setShowWhen(false)
                .setCategory(Notification.CATEGORY_CALL)
                .setContentIntent(Notify.activityIntent(this, Notify.ACTION_OPEN, "", 3));
            if (Build.VERSION.SDK_INT >= 30) startForeground(ID, b.build(), ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE);
            else startForeground(ID, b.build());
            running = true;
        } catch (Throwable t) {
            // (refused: the call goes on as before - with the microphone only while the app is on the screen)
            running = false;
            stopSelf();
        }
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        running = false;
        super.onDestroy();
    }

    @Override
    public void onTaskRemoved(Intent rootIntent) {
        stopSelf();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
