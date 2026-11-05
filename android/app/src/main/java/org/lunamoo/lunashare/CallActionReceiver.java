package org.lunamoo.lunashare;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** "Decline" on the incoming call notification: ends the call without opening the app. */
public class CallActionReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        Notify.cancelCall();
        if (Notify.nativeRunning) LunaShareActivity.nativeOnIntent(Notify.ACTION_DECLINE, "");
    }
}
