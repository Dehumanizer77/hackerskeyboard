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
 *    they are tapping (SECURITY-REVIEW.md, HK-13);
 *  - refuses fragment injection (SECURITY_REVIEW.md, SR-05 lint gate).
 */
public class PreferenceScreenBase extends PreferenceActivity {

    @Override
    protected void onPostCreate(Bundle savedInstanceState) {
        super.onPostCreate(savedInstanceState);
        WindowInsetsHelper.fitSystemBars(this);
        filterObscuredTouches();
    }

    /**
     * These screens are the classic XML-inflated kind: none of them hosts a
     * PreferenceFragment. LatinIMESettings has to stay exported so the system
     * Settings app can open the IME's settings screen, and an exported
     * PreferenceActivity that accepts :android:show_fragment lets any app load
     * an arbitrary Fragment into this process (fragment injection). Nothing is
     * a valid fragment here.
     */
    @Override
    protected boolean isValidFragment(String fragmentName) {
        return false;
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
