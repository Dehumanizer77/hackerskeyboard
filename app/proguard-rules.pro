# R8 configuration. Release builds shrink and obfuscate (HK-08), so everything
# reached by name rather than from a Java call site has to be kept explicitly.

# --- JNI -------------------------------------------------------------------
# JNI_OnLoad in org_pocketworkstation_pckeyboard_BinaryDictionary.cpp resolves
# this class by its exact path and registers the native methods by name via
# RegisterNatives. Renaming either the class or the methods breaks every
# dictionary lookup at runtime, with no build-time error.
-keep class org.pocketworkstation.pckeyboard.BinaryDictionary {
    native <methods>;
    private long mNativeDict;
}
-keepclasseswithmembernames class * {
    native <methods>;
}

# --- Instantiated from XML -------------------------------------------------
# Custom views used in res/layout.
-keep public class * extends android.view.View {
    public <init>(android.content.Context);
    public <init>(android.content.Context, android.util.AttributeSet);
    public <init>(android.content.Context, android.util.AttributeSet, int);
}

# Custom Preference subclasses referenced by fully-qualified name in
# res/xml/prefs*.xml (SeekBarPreference, AutoSummaryListPreference, ...).
# The default Android rules do not cover these.
-keep public class * extends android.preference.Preference {
    public <init>(android.content.Context);
    public <init>(android.content.Context, android.util.AttributeSet);
    public <init>(android.content.Context, android.util.AttributeSet, int);
}

# --- Manifest components ---------------------------------------------------
-keep public class * extends android.inputmethodservice.InputMethodService
-keep public class * extends android.app.backup.BackupAgentHelper

# --- Diagnostics -----------------------------------------------------------
# Keep line numbers so release crash reports stay actionable, but hide the
# original source file names.
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile
