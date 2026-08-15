package org.pocketworkstation.pckeyboard;

import android.app.Activity;
import android.view.View;

import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.OnApplyWindowInsetsListener;
import androidx.core.view.WindowInsetsCompat;

/**
 * Keeps activity content clear of the system bars.
 *
 * From Android 15 (API 35) an app targeting that level or higher is laid out
 * edge to edge, and from Android 16 it cannot opt out. Without this, the
 * settings screens draw underneath the status and navigation bars.
 */
final class WindowInsetsHelper {

    private WindowInsetsHelper() {}

    static void fitSystemBars(Activity activity) {
        final View content = activity.findViewById(android.R.id.content);
        if (content == null) return;
        ViewCompat.setOnApplyWindowInsetsListener(content, new OnApplyWindowInsetsListener() {
            @Override
            public WindowInsetsCompat onApplyWindowInsets(View v, WindowInsetsCompat insets) {
                Insets bars = insets.getInsets(
                        WindowInsetsCompat.Type.systemBars()
                                | WindowInsetsCompat.Type.displayCutout()
                                | WindowInsetsCompat.Type.ime());
                v.setPadding(bars.left, bars.top, bars.right, bars.bottom);
                return WindowInsetsCompat.CONSUMED;
            }
        });
        ViewCompat.requestApplyInsets(content);
    }
}
