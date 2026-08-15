package org.pocketworkstation.pckeyboard;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;
import android.view.inputmethod.InputMethodManager;

/**
 * Backs the two actions on the optional "show keyboard" notification.
 *
 * This used to be registered at runtime with registerReceiver() and no
 * permission, which on every API level this app supports means any installed
 * application could send it: force the keyboard up over whatever was on screen
 * (SHOW_FORCED), or push the user into the keyboard's settings
 * (SECURITY-REVIEW.md, HK-06).
 *
 * It is now declared in the manifest with android:exported="false" and reached
 * only through the explicit, immutable PendingIntents attached to the
 * notification, so nothing outside this app can trigger it.
 */
public class NotificationReceiver extends BroadcastReceiver {
    static final String TAG = "PCKeyboard/Notification";
    static public final String ACTION_SHOW = "org.pocketworkstation.pckeyboard.SHOW";
    static public final String ACTION_SETTINGS = "org.pocketworkstation.pckeyboard.SETTINGS";

    /** The system instantiates manifest receivers with no arguments. */
    public NotificationReceiver() {
        super();
    }

    @Override
    public void onReceive(Context context, Intent intent) {
        String action = intent.getAction();
        Log.i(TAG, "NotificationReceiver.onReceive called, action=" + action);
        if (action == null) return;

        if (action.equals(ACTION_SHOW)) {
            LatinIME ime = LatinIME.sInstance;
            if (ime == null || ime.mToken == null) return;
            InputMethodManager imm = (InputMethodManager)
                context.getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null) {
                imm.showSoftInputFromInputMethod(ime.mToken, InputMethodManager.SHOW_FORCED);
            }
        } else if (action.equals(ACTION_SETTINGS)) {
            Intent settings = new Intent(context, LatinIMESettings.class);
            // A receiver has no task of its own to launch the activity into.
            settings.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(settings);
        }
    }
}
