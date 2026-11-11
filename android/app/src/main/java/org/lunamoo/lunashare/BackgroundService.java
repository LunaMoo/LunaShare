package org.lunamoo.lunashare;

import android.app.Notification;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/**
 * Foreground service: keeps the LunaShare process (and with it the engine's connection to the user's own server)
 * alive while the app is in the background, so messages and calls arrive. It does no work of its own - the native
 * engine runs in the same process. If Android or the user ends the app, it says so and stops.
 */
public class BackgroundService extends Service {
    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Notify.channels(this);
        if (!Notify.nativeRunning) {
            // restarted by the system after the app was gone: the engine isn't running, so nothing to keep alive
            stoppedNotice();
            stopSelf();
            return START_NOT_STICKY;
        }
        Notification n = Notify.builder(this, Notify.CH_SERVICE)
                .setSmallIcon(R.drawable.ic_stat_cow)
                .setContentTitle("LunaShare")
                .setContentText(getString(R.string.bg_ready))
                .setOngoing(true)
                .setShowWhen(false)
                .setContentIntent(Notify.activityIntent(this, Notify.ACTION_OPEN, "", 6))
                .build();
        try {
            if (Build.VERSION.SDK_INT >= 34) startForeground(Notify.ID_SERVICE, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            else startForeground(Notify.ID_SERVICE, n);
        } catch (Throwable t) {
            LunaShareActivity.nativeOnError("background: " + t);
            stopSelf();
        }
        return START_STICKY;
    }

    @Override
    public void onTaskRemoved(Intent rootIntent) {
        // swiped away from the recent apps: the app ends, tell the user how to get messages again
        stoppedNotice();
        stopSelf();
        super.onTaskRemoved(rootIntent);
    }

    void stoppedNotice() {
        try {
            Notification n = Notify.builder(this, Notify.CH_SERVICE)
                    .setSmallIcon(R.drawable.ic_stat_cow)
                    .setContentTitle("LunaShare")
                    .setContentText(getString(R.string.bg_stopped))
                    .setAutoCancel(true)
                    .setContentIntent(Notify.activityIntent(this, Notify.ACTION_OPEN, "", 7))
                    .build();
            Notify.nm(this).notify(Notify.ID_SERVICE + 10, n);
        } catch (Throwable ignored) {}
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
