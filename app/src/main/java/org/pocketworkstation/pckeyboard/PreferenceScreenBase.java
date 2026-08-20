package org.pocketworkstation.pckeyboard;

import android.os.Bundle;
import android.preference.PreferenceActivity;
import android.view.View;
import android.view.Window;

/**
 * Shared behaviour for the settings screens:
 *
 *  - keeps content clear of the system bars now that the app is laid out edge
 *    to edge (API 35+);
 *  - refuses touches that arrive while another window is drawn over this one,
 *    so an overlay cannot steer keyboard settings without the user seeing what
 *    they are tapping (SECURITY-REVIEW.md, HK-13).
 */
public class PreferenceScreenBase extends PreferenceActivity {

    @Override
    protected void onPostCreate(Bundle savedInstanceState) {
        super.onPostCreate(savedInstanceState);
        WindowInsetsHelper.fitSystemBars(this);
        filterObscuredTouches();
    }

    protected void filterObscuredTouches() {
        Window window = getWindow();
        if (window == null) return;
        View decor = window.getDecorView();
        if (decor != null) {
            decor.setFilterTouchesWhenObscured(true);
        }
    }
}
