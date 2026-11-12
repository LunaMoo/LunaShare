package org.lunamoo.lunashare;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;

/** Reply / Mark as read, from under a message's notification: handled without opening the app. */
public class NotifyActionReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        String key = intent.getStringExtra("key");
        if (key == null) return;
        String text = "";
        if (Notify.ACTION_REPLY.equals(intent.getAction())) {
            Bundle r = android.app.RemoteInput.getResultsFromIntent(intent);
            CharSequence cs = r != null ? r.getCharSequence(Notify.EXTRA_REPLY) : null;
            if (cs == null || cs.toString().trim().isEmpty()) return;
            text = cs.toString();
        }
        if (Notify.nativeRunning) {
            try { LunaShareActivity.nativeQuickReply(key, text); } catch (Throwable ignored) {}
        }
        Notify.clearChat(key);  // (answered / read: the notification goes)
    }
}
