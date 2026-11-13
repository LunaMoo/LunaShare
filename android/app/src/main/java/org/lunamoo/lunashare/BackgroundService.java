package org.lunamoo.lunashare;

import android.app.Notification;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

import org.libsdl.app.SDL;

/**
 * Foreground service: keeps the LunaShare process (and with it the engine's connection to the user's own server)
 * alive while the app is in the background, so messages and calls arrive. The native engine runs in the same
 * process, and outlives the screen (src/app/core.h): when Android ends the screen, the engine goes on.
 * When the process itself was ended and Android starts this service again (or the phone was restarted, or the app
 * updated - StartReceiver), there is no engine: it's started here, without a screen. Only an engine set up before
 * is started (never a new identity), and only with "Keep running in the background" on.
 */
public class BackgroundService extends Service {
    static native boolean nativeStartCore(String filesDir);

    /** Starts the engine without a screen; true when it runs (now or already). */
    static synchronized boolean startCore(Context c) {
        if (Notify.nativeRunning) return true;
        try {
            Context app = c.getApplicationContext();
            if (LunaShareActivity.appCtx == null) LunaShareActivity.appCtx = app;
            System.loadLibrary("SDL2");
            System.loadLibrary("main");
            // (what the screen's start does first: native code may call Java from its own threads after this)
            if (SDL.getContext() == null) SDL.setContext(app);
            SDL.setupJNI();
            return nativeStartCore(c.getFilesDir().getAbsolutePath());
        } catch (Throwable t) {
            android.util.Log.e("LunaShare", "starting without the screen: " + t);
            return false;
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Notify.channels(this);
        // (foreground first: a service started with startForegroundService - after a restart of the phone - must be
        // within seconds, whatever happens next)
        boolean fg = goForeground();
        if (!Notify.nativeRunning && !startCore(this)) {
            // there's no engine and none could be started (background running off, never set up): nothing to keep
            // alive. Said only when the app was running before (restarted by the system after it was ended).
            if (intent == null) stoppedNotice();
            if (fg) stopForegroundCompat();
            stopSelf();
            return START_NOT_STICKY;
        }
        if (!fg) stopSelf();  // (not allowed to run in the foreground: the engine runs while the process lives)
        return START_STICKY;
    }

    private void stopForegroundCompat() {
        try {
            if (Build.VERSION.SDK_INT >= 24) stopForeground(STOP_FOREGROUND_REMOVE);
            else stopForeground(true);
        } catch (Throwable ignored) {}
    }

    private boolean goForeground() {
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
            return true;
        } catch (Throwable t) {
            android.util.Log.e("LunaShare", "background: " + t);
            return false;
        }
    }

    @Override
    public void onTaskRemoved(Intent rootIntent) {
        // Swiped away from the recent apps: only the screen goes - the engine keeps running in this process (unless
        // background running is off: then the app's end stops this service itself).
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
