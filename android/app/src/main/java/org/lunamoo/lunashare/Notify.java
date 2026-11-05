package org.lunamoo.lunashare;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.AudioAttributes;
import android.media.RingtoneManager;
import android.net.Uri;
import android.os.Build;
import android.os.PowerManager;
import android.provider.Settings;

/**
 * System notifications for messages and calls while the app is in the background, and the foreground service that
 * keeps LunaShare connected to the user's server (see BackgroundService). Called from native code (any thread).
 */
public final class Notify {
    static final String CH_MESSAGES = "messages", CH_CALLS = "calls2", CH_SERVICE = "background";
    static final String ACTION_OPEN = "org.lunamoo.lunashare.OPEN", ACTION_ANSWER = "org.lunamoo.lunashare.ANSWER",
                        ACTION_DECLINE = "org.lunamoo.lunashare.DECLINE", ACTION_CALL = "org.lunamoo.lunashare.CALL";
    static final int ID_CALL = 2, ID_SERVICE = 1;
    static volatile boolean nativeRunning = false;

    private Notify() {}

    static Context ctx() { return LunaShareActivity.appContext(); }

    static NotificationManager nm(Context c) { return (NotificationManager) c.getSystemService(Context.NOTIFICATION_SERVICE); }

    static void channels(Context c) {
        if (Build.VERSION.SDK_INT < 26) return;
        NotificationManager m = nm(c);
        NotificationChannel msg = new NotificationChannel(CH_MESSAGES, "Messages", NotificationManager.IMPORTANCE_HIGH);
        msg.setDescription("New messages");
        m.createNotificationChannel(msg);
        NotificationChannel call = new NotificationChannel(CH_CALLS, "Calls", NotificationManager.IMPORTANCE_HIGH);
        call.setDescription("Incoming calls");
        Uri ring = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE);
        call.setSound(ring, new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
                                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build());
        call.enableVibration(true);
        call.setVibrationPattern(new long[] {0, 800, 600, 800, 600});
        call.setLockscreenVisibility(Notification.VISIBILITY_PUBLIC);
        m.createNotificationChannel(call);
        NotificationChannel svc = new NotificationChannel(CH_SERVICE, "Background connection", NotificationManager.IMPORTANCE_MIN);
        svc.setDescription("Keeps LunaShare connected for messages and calls. You can hide this notification.");
        svc.setShowBadge(false);
        m.createNotificationChannel(svc);
    }

    static Notification.Builder builder(Context c, String channel) {
        return Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(c, channel) : new Notification.Builder(c);
    }

    static PendingIntent activityIntent(Context c, String action, String key, int req) {
        Intent i = new Intent(c, LunaShareActivity.class);
        i.setAction(action);
        i.putExtra("key", key);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        return PendingIntent.getActivity(c, req, i, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    }

    static Bitmap avatar(String path) {
        if (path == null || path.isEmpty()) return null;
        try {
            return BitmapFactory.decodeFile(path);
        } catch (Throwable t) {
            return null;
        }
    }

    static int idFor(String key) { return 1000 + (key.hashCode() & 0x7fffff); }

    // ---- called from native code
    public static void message(String key, String title, String text, String avatarPath) {
        try {
            Context c = ctx();
            channels(c);
            Notification.Builder b = builder(c, CH_MESSAGES)
                    .setSmallIcon(R.mipmap.ic_launcher)
                    .setContentTitle(title)
                    .setContentText(text)
                    .setStyle(new Notification.BigTextStyle().bigText(text))
                    .setAutoCancel(true)
                    .setCategory(Notification.CATEGORY_MESSAGE)
                    .setContentIntent(activityIntent(c, ACTION_OPEN, key, idFor(key)));
            Bitmap av = avatar(avatarPath);
            if (av != null) b.setLargeIcon(av);
            if (Build.VERSION.SDK_INT < 26) b.setPriority(Notification.PRIORITY_HIGH).setDefaults(Notification.DEFAULT_ALL);
            nm(c).notify(idFor(key), b.build());
        } catch (Throwable t) {
            LunaShareActivity.nativeOnError("notification: " + t);
        }
    }

    public static void clearChat(String key) {
        try { nm(ctx()).cancel(idFor(key)); } catch (Throwable ignored) {}
    }

    public static void call(String key, String title, boolean video, String avatarPath, String answer, String decline, String sub) {
        try {
            Context c = ctx();
            channels(c);
            Intent d = new Intent(c, CallActionReceiver.class);
            d.setAction(ACTION_DECLINE);
            PendingIntent declinePi = PendingIntent.getBroadcast(c, 3, d, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            PendingIntent answerPi = activityIntent(c, ACTION_ANSWER, key, 4);
            PendingIntent fullPi = activityIntent(c, ACTION_CALL, key, 5);
            Notification.Builder b = builder(c, CH_CALLS)
                    .setSmallIcon(R.mipmap.ic_launcher)
                    .setContentTitle(title)
                    .setContentText(sub)
                    .setCategory(Notification.CATEGORY_CALL)
                    .setOngoing(true)
                    .setAutoCancel(false)
                    .setFullScreenIntent(fullPi, true)
                    .setContentIntent(fullPi)
                    .addAction(new Notification.Action.Builder(null, decline, declinePi).build())
                    .addAction(new Notification.Action.Builder(null, answer, answerPi).build());
            Bitmap av = avatar(avatarPath);
            if (av != null) b.setLargeIcon(av);
            if (Build.VERSION.SDK_INT < 26) b.setPriority(Notification.PRIORITY_MAX).setDefaults(Notification.DEFAULT_ALL);
            Notification n = b.build();
            n.flags |= Notification.FLAG_INSISTENT;  // keep ringing
            nm(c).notify(ID_CALL, n);
        } catch (Throwable t) {
            LunaShareActivity.nativeOnError("call notification: " + t);
        }
    }

    static void cancelCallNotification() {
        try { nm(ctx()).cancel(ID_CALL); } catch (Throwable ignored) {}
    }

    public static void cancelCall() {
        try { nm(ctx()).cancel(ID_CALL); } catch (Throwable ignored) {}
        LunaShareActivity.showOverLockScreen(false);
    }

    // ---- background connection
    public static void background(boolean on) {
        try {
            Context c = ctx();
            Intent i = new Intent(c, BackgroundService.class);
            if (on) {
                if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i);
                else c.startService(i);
            } else {
                c.stopService(i);
            }
        } catch (Throwable t) {
            LunaShareActivity.nativeOnError("background service: " + t);
        }
    }

    /** True when the system may cut the network while the screen is off (battery optimisation is on for us). */
    public static boolean batteryRestricted() {
        if (Build.VERSION.SDK_INT < 23) return false;
        PowerManager pm = (PowerManager) ctx().getSystemService(Context.POWER_SERVICE);
        return pm != null && !pm.isIgnoringBatteryOptimizations(ctx().getPackageName());
    }

    @android.annotation.SuppressLint("BatteryLife")
    public static void requestBatteryExemption() {
        LunaShareActivity.ui(() -> {
            try {
                Intent i = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:" + ctx().getPackageName()));
                i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                ctx().startActivity(i);
            } catch (Throwable t) {
                try {
                    Intent i = new Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS);
                    i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                    ctx().startActivity(i);
                } catch (Throwable ignored) {}
            }
        });
    }

    public static void setNativeRunning(boolean on) { nativeRunning = on; }
}
