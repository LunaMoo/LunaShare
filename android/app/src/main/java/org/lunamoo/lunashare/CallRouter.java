package org.lunamoo.lunashare;

import android.content.Context;
import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;

/**
 * Headphones during a call: when a call starts with a headset there (wired, USB or Bluetooth), the call's sound and
 * microphone go to it; one connected during the call takes them over; when it goes away they come back to where they
 * were before (the earpiece, or the loudspeaker if that was on). Android does the wired part by itself, but not
 * Bluetooth - a call stayed on the phone's earpiece with the earbuds in - and nothing at all mid-call on Android 12+.
 * For a headset, its microphone goes with its sound (the communication device / Bluetooth SCO carry both).
 * All on the UI thread.
 */
final class CallRouter {
    private static final Handler h = new Handler(Looper.getMainLooper());
    private static boolean on = false;
    private static boolean prevSpeaker = false;      // where the sound was before a headset took it
    private static boolean wasOnHeadset = false;     // the call was on a headset when last looked
    private static final HashSet<Integer> known = new HashSet<>();  // headsets seen during this call
    private static AudioDeviceCallback cb;

    private static AudioManager am() {
        Context c = Notify.ctx();
        return c == null ? null : (AudioManager) c.getSystemService(Context.AUDIO_SERVICE);
    }

    private static boolean headset(int type) {
        switch (type) {
            case AudioDeviceInfo.TYPE_WIRED_HEADSET:
            case AudioDeviceInfo.TYPE_WIRED_HEADPHONES:
            case AudioDeviceInfo.TYPE_USB_HEADSET:
            case AudioDeviceInfo.TYPE_BLUETOOTH_SCO:
            case 26: /* TYPE_BLE_HEADSET */
                return true;
            default:
                return false;
        }
    }

    // (wired / USB first: plugged in on purpose; then Bluetooth)
    private static int rank(int type) {
        return type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO || type == 26 ? 1 : 0;
    }

    /** Headsets a call can use now (Android 12+: communication devices; older: outputs). */
    private static List<AudioDeviceInfo> headsets(AudioManager am) {
        List<AudioDeviceInfo> r = new ArrayList<>();
        if (Build.VERSION.SDK_INT >= 31) {
            for (AudioDeviceInfo d : am.getAvailableCommunicationDevices())
                if (headset(d.getType())) r.add(d);
        } else {
            for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
                int t = d.getType();
                if (headset(t) || t == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP) r.add(d);  // (SCO shows only once on)
            }
        }
        r.sort((a, b) -> rank(a.getType()) - rank(b.getType()));
        return r;
    }

    private static boolean bt(int type) {
        return type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO || type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP || type == 26;
    }

    private static void use(AudioManager am, AudioDeviceInfo d) {
        if (Build.VERSION.SDK_INT >= 31) {
            AudioDeviceInfo cur = am.getCommunicationDevice();
            if (cur == null || cur.getId() != d.getId()) am.setCommunicationDevice(d);
        } else if (bt(d.getType())) {
            am.setSpeakerphoneOn(false);
            if (!am.isBluetoothScoOn()) {
                am.startBluetoothSco();
                am.setBluetoothScoOn(true);
            }
        } else {  // wired: Android routes it by itself once the loudspeaker / Bluetooth are off
            if (am.isBluetoothScoOn()) {
                am.stopBluetoothSco();
                am.setBluetoothScoOn(false);
            }
            am.setSpeakerphoneOn(false);
        }
    }

    // back to where the sound was before the headset: the loudspeaker or the earpiece
    private static void restore(AudioManager am) {
        if (Build.VERSION.SDK_INT >= 31) {
            if (prevSpeaker) {
                for (AudioDeviceInfo d : am.getAvailableCommunicationDevices())
                    if (d.getType() == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER) {
                        am.setCommunicationDevice(d);
                        return;
                    }
            }
            for (AudioDeviceInfo d : am.getAvailableCommunicationDevices())
                if (d.getType() == AudioDeviceInfo.TYPE_BUILTIN_EARPIECE) {
                    am.setCommunicationDevice(d);
                    return;
                }
            am.clearCommunicationDevice();
        } else {
            if (am.isBluetoothScoOn()) {
                am.stopBluetoothSco();
                am.setBluetoothScoOn(false);
            }
            am.setSpeakerphoneOn(prevSpeaker);
        }
    }

    private static boolean onHeadset(AudioManager am) {
        if (Build.VERSION.SDK_INT >= 31) {
            AudioDeviceInfo cur = am.getCommunicationDevice();
            return cur != null && headset(cur.getType());
        }
        return am.isBluetoothScoOn() || (!am.isSpeakerphoneOn() && hasWired(am));
    }

    private static boolean hasWired(AudioManager am) {
        for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_OUTPUTS))
            if (headset(d.getType()) && !bt(d.getType())) return true;
        return false;
    }

    private static boolean speakerNow(AudioManager am) {
        if (Build.VERSION.SDK_INT >= 31) {
            AudioDeviceInfo cur = am.getCommunicationDevice();
            return cur != null && cur.getType() == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER;
        }
        return am.isSpeakerphoneOn();
    }

    /**
     * Looks at the headsets there are. added: one came (it takes the call - the newest one); otherwise one may have
     * gone (another one there takes over, or back to before).
     */
    private static void check(boolean added) {
        if (!on) return;
        AudioManager am = am();
        if (am == null) return;
        try {
            List<AudioDeviceInfo> hs = headsets(am);
            AudioDeviceInfo pick = null;
            if (added) {
                for (AudioDeviceInfo d : hs)
                    if (!known.contains(d.getId())) {
                        pick = d;
                        break;
                    }
            }
            boolean cur = onHeadset(am);
            if (pick != null) {
                // a headset came (or was there when the call started): it takes the call
                if (!cur) prevSpeaker = speakerNow(am);
                use(am, pick);
            } else if (wasOnHeadset && !cur) {
                // the one in use went: another one still there takes over, or back to before. (Only then - a
                // loudspeaker or earpiece picked by hand with the headphones on stays as it is.)
                if (!hs.isEmpty()) use(am, hs.get(0));
                else restore(am);
            }
            wasOnHeadset = onHeadset(am) || (pick != null);
            known.clear();
            for (AudioDeviceInfo d : hs) known.add(d.getId());
        } catch (Throwable t) {
            // (a route Android won't take: the call stays where it is)
        }
    }

    // a Bluetooth headset shows as one for calls a moment after it connects: looked at again
    private static void later(final boolean added) {
        h.postDelayed(() -> check(added), 400);
        h.postDelayed(() -> check(added), 1500);
        h.postDelayed(() -> check(added), 3500);
    }

    /**
     * The person picked where the call goes (the call screen's speaker / headset button): that stands - checks still
     * waiting are dropped, and "the headset went" is judged from the new place.
     */
    static void picked() {
        if (!on) return;
        h.removeCallbacksAndMessages(null);
        h.postDelayed(() -> {
            AudioManager am = am();
            if (on && am != null) wasOnHeadset = onHeadset(am);
        }, 300);
    }

    // Wi-Fi without power saving during a call: with it, the phone's Wi-Fi sleeps between beacons and hands packets
    // over in bursts (100 ms and more) - the voice breaks up and the gaps are filled in with guessed, metallic sound.
    private static android.net.wifi.WifiManager.WifiLock wifiLock;

    private static void wifi(boolean on) {
        try {
            if (on) {
                if (wifiLock == null) {
                    Context c = Notify.ctx();
                    if (c == null) return;
                    android.net.wifi.WifiManager wm = (android.net.wifi.WifiManager) c.getApplicationContext().getSystemService(Context.WIFI_SERVICE);
                    if (wm == null) return;
                    int mode = Build.VERSION.SDK_INT >= 29 ? android.net.wifi.WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                                                           : android.net.wifi.WifiManager.WIFI_MODE_FULL_HIGH_PERF;
                    wifiLock = wm.createWifiLock(mode, "LunaShare:call");
                    wifiLock.setReferenceCounted(false);
                }
                if (!wifiLock.isHeld()) wifiLock.acquire();
            } else if (wifiLock != null && wifiLock.isHeld()) {
                wifiLock.release();
            }
        } catch (Throwable t) {
            android.util.Log.e("LunaShare", "wifi lock: " + t);
        }
    }

    /** A call started / ended. */
    static void set(boolean inCall) {
        wifi(inCall);
        AudioManager am = am();
        if (am == null || Build.VERSION.SDK_INT < 23) return;
        if (inCall == on) return;
        on = inCall;
        if (inCall) {
            known.clear();
            prevSpeaker = false;
            wasOnHeadset = false;
            if (cb == null) {
                cb = new AudioDeviceCallback() {
                    @Override
                    public void onAudioDevicesAdded(AudioDeviceInfo[] ds) {
                        for (AudioDeviceInfo d : ds)
                            if (headset(d.getType()) || bt(d.getType())) {
                                later(true);
                                return;
                            }
                    }

                    @Override
                    public void onAudioDevicesRemoved(AudioDeviceInfo[] ds) {
                        for (AudioDeviceInfo d : ds)
                            if (headset(d.getType()) || bt(d.getType())) {
                                later(false);
                                return;
                            }
                    }
                };
            }
            // (the callback reports the devices there now as added at once: the start of the call picks one)
            am.registerAudioDeviceCallback(cb, h);
            check(true);
            later(true);
        } else {
            h.removeCallbacksAndMessages(null);
            if (cb != null) am.unregisterAudioDeviceCallback(cb);
            known.clear();
        }
    }
}
