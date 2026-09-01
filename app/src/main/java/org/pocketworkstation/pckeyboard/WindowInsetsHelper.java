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

    /**
     * Keeps the keyboard's key rows clear of the navigation bar.
     *
     * The IME window is laid out edge to edge when the app targets API 35+, so
     * the gesture pill, the hide-keyboard affordance and the IME-switcher globe
     * are drawn over the bottom key row unless the keyboard insets itself.
     * Before targeting 35+ the framework reserved that space automatically.
     *
     * LatinKeyboardBaseView.onMeasure() reports its height as
     * keyboard height + paddingTop + paddingBottom, and draws keys offset by
     * paddingTop only, so bottom padding makes the view taller and leaves the
     * extra space below the keys - it does not shrink them. Key hit-testing
     * uses the left/top padding only, so it is unaffected.
     *
     * Three inset types are unioned. navigationBars() covers the bar itself.
     * systemOverlays() (API 35+) covers system elements drawn over app content,
     * which is where the IME-switcher globe and the hide-keyboard chevron live:
     * they are centred in a band slightly taller than the navigation bar, so
     * navigationBars() alone leaves them clipping the bottom key row by a few
     * dp. displayCutout() covers the rare bottom cutout.
     *
     * extraBottomPx is the gap the theme layout itself declares via
     * keyboard_bottom_padding, which KeyboardSwitcher otherwise discards. It
     * keeps the key rows off the very edge of whatever the system reserved.
     *
     * The IME type is deliberately excluded: this view *is* the IME.
     */
    static void padForBottomSystemBars(final View view, final int extraBottomPx) {
        if (view == null) return;
        // Apply the extra gap up front. The listener below refines it once the
        // system reports its insets, but on a path where no inset dispatch ever
        // arrives the user's configured gap would otherwise be silently lost.
        view.setPadding(view.getPaddingLeft(), view.getPaddingTop(),
                view.getPaddingRight(), extraBottomPx);
        ViewCompat.setOnApplyWindowInsetsListener(view, new OnApplyWindowInsetsListener() {
            @Override
            public WindowInsetsCompat onApplyWindowInsets(View v, WindowInsetsCompat insets) {
                Insets bars = insets.getInsets(
                        WindowInsetsCompat.Type.navigationBars()
                                | WindowInsetsCompat.Type.systemOverlays()
                                | WindowInsetsCompat.Type.displayCutout());
                // The bottom value is set absolutely rather than accumulated, so
                // repeated inset dispatches cannot stack up padding. On a window
                // the navigation bar does not overlap - an older platform, or a
                // device with no navigation bar - the inset is 0 and only the
                // theme's own gap remains, which is why this is safe on every
                // API level. Types the running platform does not know report
                // Insets.NONE rather than throwing.
                v.setPadding(v.getPaddingLeft(), v.getPaddingTop(),
                        v.getPaddingRight(), bars.bottom + extraBottomPx);
                return insets;
            }
        });
        ViewCompat.requestApplyInsets(view);
    }

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
