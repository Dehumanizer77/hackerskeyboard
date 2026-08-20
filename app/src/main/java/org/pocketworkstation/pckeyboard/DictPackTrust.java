package org.pocketworkstation.pckeyboard;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.content.pm.SigningInfo;
import android.os.Build;
import android.preference.PreferenceManager;
import android.util.Log;

import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

/**
 * Decides which packages may supply dictionary data.
 *
 * A dictionary pack is not a passive data file: its bytes are parsed by native
 * code inside the process that observes every keystroke. Discovery is by
 * intent filter, so any installed application can offer one without holding a
 * permission and without the user being asked (SECURITY-REVIEW.md, HK-02).
 *
 * Policy:
 *   - a pack signed with the same certificate as the keyboard is trusted
 *     automatically (this covers the official dictionary packs);
 *   - anything else must be approved by the user once, in Settings. The
 *     approval records the pack's signing-certificate digest, so a package
 *     that is later replaced by one signed with a different key loses trust
 *     and has to be approved again.
 */
public class DictPackTrust {
    private static final String TAG = "HK/DictPackTrust";
    private static final String PREF_PREFIX = "dict_pack_allow_";

    private DictPackTrust() {}

    static String prefKey(String packageName) {
        return PREF_PREFIX + packageName;
    }

    /**
     * SHA-256 over the package's signing certificates, or null if it cannot be
     * determined. Multiple signers are folded in declaration order.
     */
    @SuppressWarnings("deprecation")
    public static String certDigest(PackageManager pm, String packageName) {
        try {
            Signature[] signatures;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                PackageInfo info = pm.getPackageInfo(
                        packageName, PackageManager.GET_SIGNING_CERTIFICATES);
                SigningInfo signingInfo = info.signingInfo;
                if (signingInfo == null) return null;
                signatures = signingInfo.hasMultipleSigners()
                        ? signingInfo.getApkContentsSigners()
                        : signingInfo.getSigningCertificateHistory();
            } else {
                PackageInfo info = pm.getPackageInfo(
                        packageName, PackageManager.GET_SIGNATURES);
                signatures = info.signatures;
            }
            if (signatures == null || signatures.length == 0) return null;

            MessageDigest md = MessageDigest.getInstance("SHA-256");
            for (Signature signature : signatures) {
                md.update(signature.toByteArray());
            }
            byte[] digest = md.digest();
            StringBuilder sb = new StringBuilder(digest.length * 2);
            for (byte b : digest) {
                sb.append(Character.forDigit((b >> 4) & 0xF, 16));
                sb.append(Character.forDigit(b & 0xF, 16));
            }
            return sb.toString();
        } catch (PackageManager.NameNotFoundException e) {
            return null;
        } catch (NoSuchAlgorithmException e) {
            // SHA-256 is mandatory on every Android release; if it is missing,
            // fail closed rather than trusting the package.
            Log.e(TAG, "SHA-256 unavailable", e);
            return null;
        }
    }

    /** True if the package is signed with the same certificate as this app. */
    public static boolean isSameSigner(Context context, String packageName) {
        PackageManager pm = context.getPackageManager();
        try {
            return pm.checkSignatures(context.getPackageName(), packageName)
                    == PackageManager.SIGNATURE_MATCH;
        } catch (RuntimeException e) {
            return false;
        }
    }

    /**
     * True if this package may be used as a dictionary source right now.
     */
    public static boolean isTrusted(Context context, String packageName) {
        if (packageName == null) return false;
        if (packageName.equals(context.getPackageName())) return true;
        if (isSameSigner(context, packageName)) return true;

        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        String approved = prefs.getString(prefKey(packageName), null);
        if (approved == null) return false;

        // The approval is bound to the certificate that was present when the
        // user granted it, not to the package name alone.
        String current = certDigest(context.getPackageManager(), packageName);
        if (current == null || !approved.equals(current)) {
            Log.w(TAG, "signing certificate changed for " + packageName + ", revoking trust");
            prefs.edit().remove(prefKey(packageName)).apply();
            return false;
        }
        return true;
    }

    /** Records or withdraws the user's approval for a dictionary pack. */
    public static void setApproved(Context context, String packageName, boolean approved) {
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        if (approved) {
            String digest = certDigest(context.getPackageManager(), packageName);
            if (digest == null) {
                Log.w(TAG, "refusing to approve " + packageName + ": no certificate");
                return;
            }
            prefs.edit().putString(prefKey(packageName), digest).apply();
            Log.i(TAG, "dictionary pack approved: " + packageName);
        } else {
            prefs.edit().remove(prefKey(packageName)).apply();
            Log.i(TAG, "dictionary pack approval withdrawn: " + packageName);
        }
    }
}
