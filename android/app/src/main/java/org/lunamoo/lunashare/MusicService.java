package org.lunamoo.lunashare;

import android.app.Notification;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;

/**
 * The music player in the background: a "media playback" foreground service with the usual media notification
 * (cover, title, previous / play-pause / next), lock screen controls and headset / Bluetooth buttons through a
 * MediaSession, audio focus (a phone call or another app playing pauses the music) and pausing when headphones
 * come out. The music itself plays in native code (src/app/music.cpp); this only shows it and passes buttons back.
 */
public class MusicService extends Service {
    static final int ID = 7;
    static final String A_TOGGLE = "org.lunamoo.lunashare.MUSIC_TOGGLE", A_NEXT = "org.lunamoo.lunashare.MUSIC_NEXT",
                        A_PREV = "org.lunamoo.lunashare.MUSIC_PREV", A_STOP = "org.lunamoo.lunashare.MUSIC_STOP";
    // native commands (LunaShareActivity.nativeMusic)
    static final int C_TOGGLE = 1, C_NEXT = 2, C_PREV = 3, C_PAUSE = 4, C_PLAY = 5, C_STOP = 6, C_SEEK = 7, C_VOLUME = 8;

    private static final Handler main = new Handler(Looper.getMainLooper());
    private static volatile boolean active, playing;
    private static volatile String title = "", artist = "", album = "", cover = "";
    private static volatile long durMs, posMs;
    private static String shownKey = "";

    private static MediaSession session;
    private static AudioManager am;
    private static AudioFocusRequest focusReq;
    private static boolean hasFocus, resumeOnGain, noisyRegistered;
    private static String coverLoaded = "";
    private static Bitmap coverBmp;
    private static String letterKey = "";
    private static Bitmap letterBmp;

    // no cover (a station without a logo, a song without a picture): the name's initials on a colour of its own -
    // without any picture the phone shows the app's small icon there, blown up, next to the same icon as a badge
    static Bitmap lettersFor(String name) {
        StringBuilder in = new StringBuilder();
        boolean start = true;
        for (int i = 0; i < name.length() && in.length() < 4;) {
            int cp = name.codePointAt(i);
            i += Character.charCount(cp);
            if (Character.isLetterOrDigit(cp)) {
                if (start) in.appendCodePoint(Character.toUpperCase(cp));
                start = false;
            } else start = true;
            if (in.codePointCount(0, in.length()) >= 2) break;
        }
        if (in.length() == 0) in.append('\u266A');  // (a note)
        int[] colours = {0xFF7E57C2, 0xFF5C6BC0, 0xFF1E88E5, 0xFF00897B, 0xFF43A047, 0xFFF4511E, 0xFFD81B60, 0xFF6D4C41, 0xFF546E7A, 0xFF8E24AA};
        int c = colours[(name.hashCode() & 0x7fffffff) % colours.length];
        int sz = 256;
        Bitmap b = Bitmap.createBitmap(sz, sz, Bitmap.Config.ARGB_8888);
        android.graphics.Canvas cv = new android.graphics.Canvas(b);
        cv.drawColor(c);
        android.graphics.Paint p = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
        p.setColor(0xFFFFFFFF);
        p.setTypeface(android.graphics.Typeface.create(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD));
        p.setTextAlign(android.graphics.Paint.Align.CENTER);
        String t = in.toString();
        p.setTextSize(sz * 0.42f);
        float w = p.measureText(t);
        if (w > sz * 0.8f) p.setTextSize(p.getTextSize() * sz * 0.8f / w);
        android.graphics.Paint.FontMetrics fm = p.getFontMetrics();
        cv.drawText(t, sz / 2f, sz / 2f - (fm.ascent + fm.descent) / 2f, p);
        return b;
    }

    private static final BroadcastReceiver noisy = new BroadcastReceiver() {
        @Override public void onReceive(Context c, Intent i) {
            if (AudioManager.ACTION_AUDIO_BECOMING_NOISY.equals(i.getAction())) cmd(C_PAUSE, 0);  // headphones out
        }
    };

    static void cmd(int c, long arg) {
        try { LunaShareActivity.nativeMusic(c, arg); } catch (Throwable ignored) {}
    }

    /** From native code (any thread): what's playing. */
    public static void update(boolean act, boolean play, String t, String a, String al, String c, long dur, long pos) {
        boolean changed = act != active || play != playing || !t.equals(title) || !a.equals(artist) || !c.equals(cover) || dur != durMs;
        long expected = posMs + (playing ? SystemClock.elapsedRealtime() - lastPosAt : 0);
        boolean jumped = Math.abs(pos - expected) > 2500;
        active = act; playing = play; title = t; artist = a; album = al; cover = c; durMs = dur; posMs = pos;
        lastPosAt = SystemClock.elapsedRealtime();
        if (!changed && !jumped) return;
        main.post(() -> {
            Context ctx = Notify.ctx();
            if (ctx == null) return;
            if (!active) {
                ctx.stopService(new Intent(ctx, MusicService.class));
                return;
            }
            Intent i = new Intent(ctx, MusicService.class);
            try {
                if (Build.VERSION.SDK_INT >= 26) ctx.startForegroundService(i);
                else ctx.startService(i);
            } catch (Throwable e) {
                LunaShareActivity.nativeOnError("music: " + e);
            }
        });
    }
    private static volatile long lastPosAt = 0;

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String a = intent != null ? intent.getAction() : null;
        if (A_TOGGLE.equals(a)) cmd(C_TOGGLE, 0);
        else if (A_NEXT.equals(a)) cmd(C_NEXT, 0);
        else if (A_PREV.equals(a)) cmd(C_PREV, 0);
        else if (A_STOP.equals(a)) cmd(C_STOP, 0);
        show();
        if (!active && a == null) stopSelf();
        return START_NOT_STICKY;
    }

    private void ensureSession() {
        if (session != null) return;
        session = new MediaSession(this, "LunaShare music");
        session.setCallback(new MediaSession.Callback() {
            @Override public void onPlay() { cmd(C_PLAY, 0); }
            @Override public void onPause() { cmd(C_PAUSE, 0); }
            @Override public void onSkipToNext() { cmd(C_NEXT, 0); }
            @Override public void onSkipToPrevious() { cmd(C_PREV, 0); }
            @Override public void onStop() { cmd(C_PAUSE, 0); }
            @Override public void onSeekTo(long ms) { cmd(C_SEEK, ms); }
        });
        session.setActive(true);
    }

    private void focus(boolean want) {
        if (am == null) am = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
        if (am == null) return;
        if (want && !hasFocus) {
            AudioManager.OnAudioFocusChangeListener l = change -> {
                switch (change) {
                    case AudioManager.AUDIOFOCUS_LOSS: hasFocus = false; resumeOnGain = false; cmd(C_PAUSE, 0); break;
                    case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT: resumeOnGain = playing; cmd(C_PAUSE, 0); break;
                    case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK: cmd(C_VOLUME, 30); break;
                    case AudioManager.AUDIOFOCUS_GAIN:
                        cmd(C_VOLUME, 100);
                        if (resumeOnGain) cmd(C_PLAY, 0);
                        resumeOnGain = false;
                        break;
                }
            };
            int r;
            if (Build.VERSION.SDK_INT >= 26) {
                focusReq = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                        .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                        .setOnAudioFocusChangeListener(l).build();
                r = am.requestAudioFocus(focusReq);
            } else {
                r = am.requestAudioFocus(l, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN);
            }
            hasFocus = r == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
        } else if (!want && hasFocus && !resumeOnGain) {
            if (Build.VERSION.SDK_INT >= 26 && focusReq != null) am.abandonAudioFocusRequest(focusReq);
            hasFocus = false;
        }
    }

    private PendingIntent action(String a, int req) {
        Intent i = new Intent(this, MusicService.class);
        i.setAction(a);
        return PendingIntent.getService(this, req, i, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    }

    private void show() {
        Notify.channels(this);
        ensureSession();
        focus(playing);
        if (playing && !noisyRegistered) {
            registerReceiver(noisy, new IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY));
            noisyRegistered = true;
        } else if (!playing && noisyRegistered) {
            try { unregisterReceiver(noisy); } catch (Throwable ignored) {}
            noisyRegistered = false;
        }
        if (!cover.equals(coverLoaded)) {
            coverLoaded = cover;
            coverBmp = null;
            try {
                if (!cover.isEmpty()) {
                    BitmapFactory.Options o = new BitmapFactory.Options();
                    o.inSampleSize = 2;
                    coverBmp = BitmapFactory.decodeFile(cover, o);
                }
            } catch (Throwable ignored) {}
        }
        MediaMetadata.Builder md = new MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_TITLE, title)
                .putString(MediaMetadata.METADATA_KEY_ARTIST, artist)
                .putString(MediaMetadata.METADATA_KEY_ALBUM, album)
                .putLong(MediaMetadata.METADATA_KEY_DURATION, durMs);
        Bitmap art = coverBmp;
        if (art == null) {
            String who = !artist.isEmpty() ? artist : title;
            if (!who.equals(letterKey) || letterBmp == null) {
                letterKey = who;
                try { letterBmp = lettersFor(who); } catch (Throwable t) { letterBmp = null; }
            }
            art = letterBmp;
        }
        if (art != null) md.putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, art);
        session.setMetadata(md.build());
        session.setPlaybackState(new PlaybackState.Builder()
                .setActions(PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE | PlaybackState.ACTION_PLAY_PAUSE |
                            PlaybackState.ACTION_SKIP_TO_NEXT | PlaybackState.ACTION_SKIP_TO_PREVIOUS | PlaybackState.ACTION_SEEK_TO |
                            PlaybackState.ACTION_STOP)
                .setState(playing ? PlaybackState.STATE_PLAYING : PlaybackState.STATE_PAUSED, posMs, playing ? 1f : 0f,
                          SystemClock.elapsedRealtime())
                .build());
        Notification.Builder b = Notify.builder(this, Notify.CH_MUSIC)
                .setSmallIcon(R.drawable.ic_stat_cow)
                .setContentTitle(title)
                .setContentText(artist.isEmpty() ? album : artist)
                .setOngoing(playing)
                .setShowWhen(false)
                .setVisibility(Notification.VISIBILITY_PUBLIC)
                .setContentIntent(Notify.activityIntent(this, Notify.ACTION_OPEN, "", 9))
                .setDeleteIntent(action(A_STOP, 24))
                .addAction(new Notification.Action.Builder(android.R.drawable.ic_media_previous, "Previous", action(A_PREV, 21)).build())
                .addAction(new Notification.Action.Builder(playing ? android.R.drawable.ic_media_pause : android.R.drawable.ic_media_play,
                        playing ? "Pause" : "Play", action(A_TOGGLE, 22)).build())
                .addAction(new Notification.Action.Builder(android.R.drawable.ic_media_next, "Next", action(A_NEXT, 23)).build())
                .setStyle(new Notification.MediaStyle().setMediaSession(session.getSessionToken()).setShowActionsInCompactView(0, 1, 2));
        if (art != null) b.setLargeIcon(art);
        Notification n = b.build();
        try {
            if (Build.VERSION.SDK_INT >= 29) startForeground(ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
            else startForeground(ID, n);
            // paused: the notification stays (swipe it away to stop), the service no longer holds the app up
            if (!playing && Build.VERSION.SDK_INT >= 24) stopForeground(Service.STOP_FOREGROUND_DETACH);
        } catch (Throwable t) {
            LunaShareActivity.nativeOnError("music notification: " + t);
        }
    }

    @Override
    public void onDestroy() {
        focus(false);
        hasFocus = false;
        if (noisyRegistered) {
            try { unregisterReceiver(noisy); } catch (Throwable ignored) {}
            noisyRegistered = false;
        }
        if (session != null) {
            session.setActive(false);
            session.release();
            session = null;
        }
        try { Notify.nm(this).cancel(ID); } catch (Throwable ignored) {}
        super.onDestroy();
    }
}
