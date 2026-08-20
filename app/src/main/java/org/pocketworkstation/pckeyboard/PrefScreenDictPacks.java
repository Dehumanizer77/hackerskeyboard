package org.pocketworkstation.pckeyboard;

import android.content.pm.PackageManager;
import android.os.Bundle;
import android.preference.CheckBoxPreference;
import android.preference.Preference;
import android.preference.PreferenceCategory;
import android.preference.PreferenceScreen;

import java.util.Map;

/**
 * Lets the user decide which installed packages may supply dictionary data.
 *
 * Dictionary packs are discovered by intent filter, so any app can offer one.
 * Their bytes are parsed by native code in the keyboard's own process, and the
 * dictionary they provide is what autocorrect rewrites typed text with - so
 * this is a trust decision, and it is the user's to make (HK-02).
 *
 * Packs signed with the keyboard's own certificate are trusted implicitly and
 * are shown here as already enabled and not switchable.
 */
public class PrefScreenDictPacks extends PreferenceScreenBase {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setTitle(R.string.dict_packs_title);
        rebuild();
    }

    @Override
    protected void onResume() {
        super.onResume();
        rebuild();
    }

    private void rebuild() {
        // Re-scan so packs installed while this screen was away show up.
        PluginManager.getPluginDictionaries(getApplicationContext());

        PreferenceScreen screen = getPreferenceManager().createPreferenceScreen(this);
        PreferenceCategory category = new PreferenceCategory(this);
        category.setTitle(R.string.dict_packs_title);
        screen.addPreference(category);

        Map<String, String> packs = PluginManager.getDiscoveredPacks();
        if (packs.isEmpty()) {
            Preference none = new Preference(this);
            none.setTitle(R.string.dict_packs_none);
            none.setSummary(R.string.dict_packs_none_summary);
            none.setEnabled(false);
            category.addPreference(none);
        } else {
            PackageManager pm = getPackageManager();
            for (Map.Entry<String, String> entry : packs.entrySet()) {
                final String pkg = entry.getKey();
                String lang = entry.getValue();

                CheckBoxPreference pref = new CheckBoxPreference(this);
                pref.setKey(DictPackTrust.prefKey(pkg));
                pref.setPersistent(false);
                pref.setTitle(labelFor(pm, pkg));

                if (DictPackTrust.isSameSigner(this, pkg)) {
                    pref.setSummary(getString(R.string.dict_packs_official, lang, pkg));
                    pref.setChecked(true);
                    pref.setEnabled(false);
                } else {
                    pref.setSummary(getString(R.string.dict_packs_third_party, lang, pkg));
                    pref.setChecked(DictPackTrust.isTrusted(this, pkg));
                    pref.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                        @Override
                        public boolean onPreferenceChange(Preference preference, Object newValue) {
                            DictPackTrust.setApproved(
                                    PrefScreenDictPacks.this, pkg, Boolean.TRUE.equals(newValue));
                            // Reload dictionaries so the change takes effect now.
                            PluginManager.getPluginDictionaries(getApplicationContext());
                            LatinIME ime = LatinIME.sInstance;
                            if (ime != null) ime.toggleLanguage(true, true);
                            return true;
                        }
                    });
                }
                category.addPreference(pref);
            }
        }

        Preference explanation = new Preference(this);
        explanation.setTitle(R.string.dict_packs_warning_title);
        explanation.setSummary(R.string.dict_packs_warning);
        explanation.setSelectable(false);
        screen.addPreference(explanation);

        setPreferenceScreen(screen);
    }

    private String labelFor(PackageManager pm, String pkg) {
        try {
            return pm.getApplicationLabel(pm.getApplicationInfo(pkg, 0)).toString();
        } catch (PackageManager.NameNotFoundException e) {
            return pkg;
        }
    }
}
