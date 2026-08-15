# Security Review — Hacker's Keyboard

**Target** `https://github.com/klausw/hackerskeyboard`
**Commit reviewed** `9202d9d8b1d379f1a20edf08ed7e7149038c8e43` (2024-10-09, latest on `master`)
**Application** `org.pocketworkstation.pckeyboard` v1.41.1 (versionCode 1041001)
**Review date** 2026-08-15
**Type** Static source review (manual, whole-codebase)
**Codebase** 44 Java files (~14k LOC), 6 C/C++ files (~2k LOC), 1 prebuilt JAR, Gradle + CMake build

---

## 1. Executive summary

Hacker's Keyboard is an Android input method derived from the Android 2.3 (Gingerbread) AOSP
keyboard. An IME is the single highest-value non-system component on a phone: it observes every
character the user types, in every application, including credentials, messages and recovery codes.
The security bar for such an app is therefore narrow and specific — do not exfiltrate, do not retain
more than necessary, do not accept input from untrusted parties, and do not be memory-unsafe.

**The good news dominates the first two.** The app requests no `INTERNET` permission and contains no
network code, no telemetry, no crash-reporting SDK, no WebView, no dynamic code loading and no
clipboard access. It cannot phone home, and this was verified across the entire source tree rather
than assumed. Its permission set is three entries long. For a keyboard, this is the property that
matters most, and it holds.

**The problems are in the other two.** The review found one exploitable chain and a set of privacy
and IPC weaknesses:

* **A hostile app with zero permissions can supply the keyboard's dictionary.** Any installed
  application that declares an activity with the action `org.pocketworkstation.DICT` is
  automatically discovered and its raw resources are loaded as the main dictionary — no signature
  check, no permission, no user confirmation, no validation (**HK-02**).
* **Those bytes are then parsed by memory-unsafe C++.** The 2009-vintage AOSP dictionary parser has
  unbounded writes into a fixed 128-element buffer, unbounded writes into the caller's output array,
  an unbounded stack VLA write, and out-of-bounds reads in nearly every accessor (**HK-01**).

Together these are a realistic path from "user installs an unrelated app" to "attacker-controlled
code inside the process that sees every keystroke". Neither half is dangerous alone; the
combination is, and it is reachable in the app's *normal* configuration, because Hacker's Keyboard
ships without bundled dictionaries for most locales and deliberately loads them from third-party
dictionary-pack APKs.

Beyond that chain, learned text (words harvested from everything typed) is written to plaintext
databases that are eligible for cloud and `adb` backup (**HK-03**), the standard Android flag by
which an app says "do not learn from this field" is ignored (**HK-04**), the microphone key is
offered in password fields because a suppression check was left as an empty stub (**HK-07**), and
the notification uses mutable implicit `PendingIntent`s delivered to an unprotected exported
receiver (**HK-05**, **HK-06**).

Underlying most of the IPC findings is **HK-08**: the app targets API 26 (Android 8.0, 2017). A
decade of platform hardening — mandatory `PendingIntent` immutability, explicit receiver export,
mandatory `android:exported`, scoped backup rules — is opted out of by that one line. Raising the
target SDK fixes several findings for free.

The project README states plainly that this is an old codebase in maintenance mode. That is
consistent with what the code shows, and the findings below are prioritised accordingly: two are
worth fixing even in a frozen codebase, the rest are ordinary hygiene.

### Risk summary

| ID | Finding | Severity | CWE |
|----|---------|----------|-----|
| HK-01 | Memory-unsafe binary dictionary parser (OOB read/write, stack + heap) | **High** | CWE-787, CWE-125, CWE-121 |
| HK-02 | Any installed app can supply the keyboard's dictionary, unauthenticated | **High** | CWE-829, CWE-284 |
| HK-03 | Learned-text databases are eligible for cloud and `adb` backup | **Medium** | CWE-530, CWE-359 |
| HK-04 | `IME_FLAG_NO_PERSONALIZED_LEARNING` is never honoured | **Medium** | CWE-359 |
| HK-05 | Mutable implicit `PendingIntent`s in the ongoing notification | **Medium** | CWE-927 |
| HK-07 | Voice input offered in password fields (suppression stubbed out) | **Medium** | CWE-200 |
| HK-06 | Unprotected exported broadcast receiver | Low | CWE-926 |
| HK-08 | Severely outdated target SDK and dependency set | Low | CWE-1104, CWE-1035 |
| HK-09 | Unverifiable 2011 prebuilt JAR in the build | Low | CWE-1357 |
| HK-10 | Six implicitly exported activities | Low | CWE-926 |
| HK-11 | Home-grown signing-certificate check (32-bit XOR fold) | Low | CWE-327, CWE-654 |
| HK-13 | No tapjacking protection on any view | Low | CWE-1021 |
| HK-12 | Dormant keystroke-logging code | Info | — |
| HK-14 | Dead contacts-harvesting code retained | Info | — |

Severity reflects impact × exploitability **for the platform this app actually targets** (API 26).
Several Low findings would be non-issues on a modern target SDK; that is the point of HK-08.

---

## 2. Threat model

The relevant adversaries for an IME, in descending order of realism:

1. **A malicious or compromised app already installed on the device**, holding no special
   permissions. It shares the device but not the keyboard's process. This is the adversary that
   HK-01/HK-02, HK-05, HK-06 and HK-10 concern.
2. **Anyone with physical or `adb` access to the device**, or with access to the user's cloud backup
   account. This is HK-03.
3. **An application the user is typing into**, which controls the `EditorInfo` the keyboard receives
   and may reasonably expect the keyboard to honour its sensitivity hints. This is HK-04 and HK-07.
4. **A network adversary** — not applicable. The app has no network access.

The asset in every case is the same: the plaintext of everything the user types.

---

## 3. Findings

### HK-01 — Memory-unsafe binary dictionary parser

**Severity** High **CWE** CWE-787 (OOB write), CWE-125 (OOB read), CWE-121 (stack overflow)
**Location** `app/src/main/cpp/dictionary.cpp`, `app/src/main/cpp/dictionary.h:90`

The native dictionary reader is the 2009 AOSP implementation, essentially unmodified. It treats the
dictionary file as trusted, well-formed input. It is neither, once HK-02 is taken into account. Four
distinct classes of memory error are present.

**(a) Unbounded write into `Dictionary::mWord`.** `mWord` is a fixed `unsigned short[128]`
(`dictionary.h:90`). `getWordsRec` writes `mWord[depth]` at `dictionary.cpp:320`, `:333` and `:342`,
and the only pruning is `if (depth > maxDepth) return;` (`:292`). `maxDepth` is `mInputLength * 3`
(`:68`, `:70`), where `mInputLength` is the length of the word being typed — capped Java-side at 47
(`BinaryDictionary.java:214`). So `maxDepth` reaches 141 while the buffer holds 128.

A dictionary containing a trie chain deeper than 128 nodes therefore writes up to 13 `short`s past
the end of `mWord`, directly into the adjacent members of the same heap object — in declaration
order: `mSkipPos`, `mMaxEditDistance`, `mFullWordMultiplier`, `mTypedLetterMultiplier`, `mDictSize`,
and then the `mNextLettersFrequencies` **pointer**. That pointer is subsequently dereferenced and
written through:

```cpp
// dictionary.cpp:90-96
void Dictionary::registerNextLetter(unsigned short c) {
    if (c < mNextLettersSize) {
        mNextLettersFrequencies[c]++;   // corrupted base, corrupted bound
    }
}
```

Both the pointer and the bound `mNextLettersSize` sit inside the overflow window, giving an
attacker-influenced increment at an attacker-influenced address.

**(b) Heap overflow of the caller's output array.** `addWord` (`:176-213`) copies `length` elements
into a row of `mMaxWordLength` inside `mOutputChars`, with no check that `length <= mMaxWordLength`:

```cpp
// dictionary.cpp:179, :204-208
word[length] = 0;                                  // writes one element past the caller's word
...
unsigned short *dest = mOutputChars + (insertAt) * mMaxWordLength;
while (length--) { *dest++ = *word++; }            // length is depth+1, up to 142
*dest = 0;
```

`length` is `depth + 1` (up to 142); `mMaxWordLength` is 48 (`BinaryDictionary.java:40`);
`mOutputChars` is the pinned Java `char[48 * 18]` obtained via `GetCharArrayElements`
(`BinaryDictionary.java:54`, JNI bridge `:70`). A word deeper than 48 nodes overflows into
subsequent rows, and when it is placed in the final row it runs past the end of the Java array
entirely. The attacker controls the frequency values in the dictionary and therefore controls
`insertAt`. `addWordBigram` (`:215-253`) has the identical defect.

**(c) Stack overflow in `searchForTerminalNode`.** The word buffer is a VLA of 48 elements
(`:440`), while `depth` is incremented once per outer-loop iteration (`:487`, `:530`) with no upper
bound checked before the write at `:454`:

```cpp
// dictionary.cpp:440, :448-454, :530
unsigned short word[mMaxWordLength];        // 48
while (!found) {
    if (depth >= 0) { word[depth] = (unsigned short) followingChar; }   // no depth < 48 check
    ...
    depth++;
}
```

The loop exits only on `found` or when `followDownBranchAddress` becomes 0. A crafted
bigram-enabled dictionary keeps the walk alive and writes arbitrarily far up the stack. Reachable
via `getBigrams`, gated only on header bytes read from the same untrusted file (`:99-111`).

**(d) Out-of-bounds reads in essentially every accessor.** The bounds checks that exist validate the
cursor *before* the read but not the bytes actually touched:

```cpp
// dictionary.cpp:113-124 — validates *pos, then reads *pos and *pos+1 after incrementing
unsigned short Dictionary::getChar(int *pos) {
    if (*pos < 0 || *pos >= mDictSize) return 0;
    unsigned short ch = (unsigned short) (mDict[(*pos)++] & 0xFF);
    if (ch == 0xFF) { ch = ((mDict[*pos] & 0xFF) << 8) | (mDict[*pos + 1] & 0xFF); (*pos) += 2; }
    return ch;
}

// dictionary.cpp:152-160 — no bound at all; walks off the end while the continuation bit is set
int bigramExist = (mDict[*pos] & FLAG_BIGRAM_READ);
if (bigramExist > 0) {
    int nextBigramExist = 1;
    while (nextBigramExist > 0) {
        (*pos) += 3;
        nextBigramExist = (mDict[(*pos)++] & FLAG_BIGRAM_CONTINUED);
    }
}
```

`getAddress` (`:126-141`) reads `*pos+1` and `*pos+2` after validating only `*pos`. The unbounded
bigram-skip loop is repeated verbatim in `searchForTerminalNode` (`:518-527`) and `getBigrams`
(`:418-428`). The file's own first two bytes decide which parsing mode is used (`:99-111`), so the
attacker also chooses which of these paths runs.

**Impact.** Memory corruption inside the process that observes every keystroke in every application.
The primitives available — a controlled increment through a corrupted pointer, a linear heap
overflow with controlled length and contents, and a stack VLA overflow — are the classic ingredients
of a code-execution exploit. No exploit was developed as part of this review, and modern mitigations
(ASLR, stack canaries if enabled, heap hardening) raise the bar; the conservative reading is that
crash-level denial of service is certain and code execution is plausible.

**Prerequisite.** Control over the dictionary bytes — supplied for free by HK-02.

**Recommendation.**
* Clamp `depth` to `min(maxDepth, mMaxWordLength - 1, 127)` before every `mWord[depth]` write, and
  reject `length > mMaxWordLength` at the top of `addWord`/`addWordBigram`.
* Bound the `while (!found)` loop in `searchForTerminalNode` by `mMaxWordLength` and by a maximum
  iteration count.
* Route every `mDict[...]` access through a single checked accessor that validates `pos + n` against
  `mDictSize` — including the `+1`/`+2` follow-on reads and the bigram-skip loops.
* Validate the dictionary structurally once at `open` time (header, node counts, every address
  in range, acyclicity/max depth) rather than trusting it during traversal.
* Build the native library with `-fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wl,-z,relro,-z,now`;
  none of these are currently set in `app/CMakeLists.txt`.
* Highest-value next step: run a libFuzzer/AFL harness over `Dictionary::getSuggestions` and
  `Dictionary::getBigrams` with ASan. Every issue above should surface within minutes.

---

### HK-02 — Any installed app can supply the keyboard's dictionary, unauthenticated

**Severity** High **CWE** CWE-829 (untrusted functionality inclusion), CWE-284 (access control)
**Location** `app/src/main/java/org/pocketworkstation/pckeyboard/PluginManager.java:51-166`, `:272-290`; `Suggest.java:109-116`; `LatinIME.java:408-415`

The keyboard discovers dictionary providers by broadcast-intent query and loads their resources with
no authentication of any kind:

```java
// PluginManager.java:121-137
static void getHKDictionaries(PackageManager packageManager) {
    Intent dictIntent = new Intent(HK_INTENT_DICT);          // "org.pocketworkstation.DICT"
    List<ResolveInfo> dictPacks = packageManager.queryIntentActivities(dictIntent, 0);
    for (ResolveInfo ri : dictPacks) {
        ApplicationInfo appInfo = ri.activityInfo.applicationInfo;
        String pkgName = appInfo.packageName;
        Resources res = packageManager.getResourcesForApplication(appInfo);
        int langId = res.getIdentifier("dict_language", "string", pkgName);
        ...
        int rawId = res.getIdentifier("main", "raw", pkgName);
```

The resulting `InputStream`s are handed straight to `BinaryDictionary` (`:192`) and from there into
the native parser. A second discovery path accepts AnySoftKeyboard-format packs via
`queryBroadcastReceivers` (`:51-119`).

There is **no** signature check, **no** permission on the intent, **no** allowlist, **no** user
confirmation, and **no** validation of the bytes. Any app on the device can register as a dictionary
provider by adding an intent filter to its manifest — a capability available to a zero-permission
app. Discovery runs at IME startup (`LatinIME.java:408`) and again on every `PACKAGE_ADDED`,
`PACKAGE_REPLACED` and `PACKAGE_REMOVED` broadcast (`:410-415`), so installation alone is enough.

Critically, the plugin **replaces** the built-in dictionary rather than supplementing it:

```java
// Suggest.java:109-116
mMainDict = new BinaryDictionary(context, dictionaryResId, DIC_MAIN);
if (!hasMainDictionary()) {                       // built-in below LARGE_DICTIONARY_THRESHOLD
    BinaryDictionary plug = PluginManager.getDictionary(context, locale.getLanguage());
    if (plug != null) { mMainDict.close(); mMainDict = plug; }
}
```

This is not an edge case. Hacker's Keyboard ships without full dictionaries for most locales and
directs users to separate dictionary-pack APKs (see README), so `hasMainDictionary()` returning
false is the *normal* state, and the attacker's pack is loaded on the ordinary path. The attacker
simply declares the victim's language.

**Impact.**
1. **Chained with HK-01**, the attacker chooses the exact bytes fed to a memory-unsafe parser
   running inside the IME process — the highest-value process on the device for keystroke capture.
2. **Even with a perfectly safe parser**, the attacker controls the autocorrect and suggestion
   corpus for the victim's language. Autocorrect silently rewrites typed text; an attacker who owns
   the corpus can make a typed address, URL, username or amount correct into one of their choosing.
   This is a content-integrity attack that needs no memory corruption at all.

**Prerequisite.** One installed app, zero permissions, no user interaction beyond the install.

**Recommendation.**
* Require dictionary packs to be signed by the same certificate as the keyboard, or by an
  explicitly allowlisted certificate — compare SHA-256 digests from `GET_SIGNING_CERTIFICATES`.
* Alternatively/additionally define a `signature`-protection-level permission that packs must hold.
* Prompt the user before a newly discovered pack is used, naming the providing package, and persist
  the decision. Silent adoption on `PACKAGE_ADDED` is the core of the problem.
* Validate the dictionary structurally before use regardless of source (see HK-01).
* Consider parsing plugin dictionaries in an isolated process (`android:isolatedProcess`) so that a
  parser bug cannot reach the IME's input stream.

---

### HK-03 — Learned-text databases are eligible for cloud and `adb` backup

**Severity** Medium **CWE** CWE-530 (backup file exposure), CWE-359 (private information exposure)
**Location** `app/src/main/AndroidManifest.xml:12-16`; `LatinIMEBackupAgent.java`; `AutoDictionary.java:60`; `UserBigramDictionary.java:68`

```xml
<application android:label="@string/english_ime_name"
        android:allowBackup="true"
        android:backupAgent="LatinIMEBackupAgent"
        android:restoreAnyVersion="true"
        ...
        android:killAfterRestore="false"
```

The declared agent registers a shared-preferences helper only:

```java
// LatinIMEBackupAgent.java
addHelper("shared_pref", new SharedPreferencesBackupHelper(this, getPackageName() + "_preferences"));
```

That scopes the *key/value* backup path, but it does not scope full-data Auto Backup, which applies
to the whole application data directory on API 23+ whenever `allowBackup` is true. The manifest
declares neither `android:fullBackupContent` (API 23+) nor `android:dataExtractionRules` (API 31+),
so nothing is excluded — including:

* `auto_dict.db` (`AutoDictionary.java:60`) — words harvested from typing, promoted to the system
  user dictionary after four uses.
* `userbigram_dict.db` (`UserBigramDictionary.java:68`) — word *pairs*, i.e. fragments of actual
  sentences the user typed.

Both are plaintext SQLite. Their contents are drawn from everything typed into non-password fields:
message text, names, addresses, account identifiers, one-time codes, recovery phrases, medical and
financial terms. This is the app's most sensitive data at rest, and it is the data with no
exclusion rule.

`android:restoreAnyVersion="true"` additionally instructs the framework to accept a restore payload
regardless of the version that produced it, which removes a sanity check on attacker-authored
backup data being written into the app's preferences.

**Impact.** Keyboard-learned personal text is copied to Google's backup infrastructure, and is
extractable via `adb backup` on OS versions where that mechanism remains available — a realistic
concern for a device that is lent, seized, resold or serviced.

**Recommendation.**
* Add `android:fullBackupContent` and `android:dataExtractionRules` that exclude `auto_dict.db`,
  `userbigram_dict.db` and any future learned-data store. For a keyboard, `allowBackup="false"` is a
  defensible default; preferences are cheap to re-enter, typed history is not.
* Remove `android:restoreAnyVersion="true"`.
* Consider offering an explicit "clear learned words" control and documenting in-app what the
  keyboard retains.

---

### HK-04 — `IME_FLAG_NO_PERSONALIZED_LEARNING` is never honoured

**Severity** Medium **CWE** CWE-359
**Location** `LatinIME.java:780-905` (`onStartInputView`), `:2807-2836` (`checkAddToDictionary`)

`onStartInputView` inspects `attribute.inputType` in detail and correctly suppresses prediction for
password *variations*:

```java
// LatinIME.java:790-796
if (variation == EditorInfo.TYPE_TEXT_VARIATION_PASSWORD
        || variation == EditorInfo.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
        || variation == 0xe0 /* TYPE_TEXT_VARIATION_WEB_PASSWORD */) {
    if ((attribute.inputType & EditorInfo.TYPE_MASK_CLASS) == EditorInfo.TYPE_CLASS_TEXT) {
        mPasswordText = true;
    }
}
```

But `attribute.imeOptions` is only ever forwarded to the keyboard-mode switcher (`:831`, `:835`,
`:852` …). `EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING` — available since API 26, which is this
app's own target SDK — is never read anywhere in the codebase.

That flag is the mechanism by which an application says "this field is sensitive, do not learn from
it" for fields that are *not* password-typed: two-factor codes, seed and recovery phrases,
incognito browsing input, medical and financial entry, security-question answers. All of these
remain fully subject to learning:

```java
// LatinIME.java:2818-2825 — the only gate is the correction mode, not the field's sensitivity
if (!(mCorrectionMode == Suggest.CORRECTION_FULL || mCorrectionMode == Suggest.CORRECTION_FULL_BIGRAM)) {
    return;
}
...
mAutoDictionary.addWord(suggestion.toString(), frequencyDelta);
```

**Impact.** Text from fields explicitly marked no-learn is written to `auto_dict.db` and
`userbigram_dict.db`, and after four uses is promoted into the **system-wide** user dictionary
(`AutoDictionary.PROMOTION_THRESHOLD` → `LatinIME.java:3325` → `UserDictionary.addWord`), where it
becomes readable by any app holding `READ_USER_DICTIONARY`. Combined with HK-03 it also reaches
backups. The app is silently overriding an explicit, standard, machine-readable privacy request
from the application being typed into.

**Recommendation.** In `onStartInputView`, treat
`(attribute.imeOptions & EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING) != 0` exactly as
`mPasswordText` is treated: disable prediction, and additionally short-circuit
`checkAddToDictionary`, `addToBigramDictionary` and promotion to the user dictionary for the
duration of that input session.

---

### HK-05 — Mutable implicit `PendingIntent`s in the ongoing notification

**Severity** Medium **CWE** CWE-927 (exposed IPC to untrusted component)
**Location** `LatinIME.java:494-500`

```java
Intent notificationIntent = new Intent(NotificationReceiver.ACTION_SHOW);
PendingIntent contentIntent = PendingIntent.getBroadcast(getApplicationContext(), 1, notificationIntent, 0);

Intent configIntent = new Intent(NotificationReceiver.ACTION_SETTINGS);
PendingIntent configPendingIntent = PendingIntent.getBroadcast(getApplicationContext(), 2, configIntent, 0);
```

Both `PendingIntent`s are built from **implicit** intents — an action string with no component and
no package — and with flags `0`, i.e. **mutable**. Because the app targets API 26, the platform does
not enforce `FLAG_IMMUTABLE` (mandatory only from API 31), so this compiles and runs as written.
Both are attached to the persistent notification.

**Impact.** A component that obtains one of these `PendingIntent` objects — most realistically a
`NotificationListenerService` the user has granted notification access to, which receives the
notification's actions — can fill in the unset component, data and extras and cause the broadcast to
be dispatched **with the keyboard's identity and permissions**, which include
`WRITE_USER_DICTIONARY`. This is the standard PendingIntent-hijacking pattern; the mutability and
the missing component are both required for it and both are present.

**Recommendation.** Use explicit intents (`new Intent(this, NotificationReceiver.class)`) and pass
`PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT`. Raising the target SDK (HK-08)
makes the platform enforce this.

---

### HK-06 — Unprotected exported broadcast receiver

**Severity** Low **CWE** CWE-926 (improperly exported component)
**Location** `LatinIME.java:489-492`; `NotificationReceiver.java:24-41`

```java
mNotificationReceiver = new NotificationReceiver(this);
final IntentFilter pFilter = new IntentFilter(NotificationReceiver.ACTION_SHOW);
pFilter.addAction(NotificationReceiver.ACTION_SETTINGS);
registerReceiver(mNotificationReceiver, pFilter);      // no permission, no export flag
```

The receiver is registered with no broadcast permission and no `RECEIVER_NOT_EXPORTED` flag (which
in any case only exists from API 33, above this app's target). Its actions are plain strings that
any app can send:

```java
// NotificationReceiver.java:28-40
if (action.equals(ACTION_SHOW)) {
    imm.showSoftInputFromInputMethod(mIME.mToken, InputMethodManager.SHOW_FORCED);
} else if (action.equals(ACTION_SETTINGS)) {
    context.startActivity(new Intent(mIME, LatinIMESettings.class));
}
```

**Impact.** Any installed app can force the soft keyboard to appear over whatever is currently on
screen (`SHOW_FORCED`), or push the user into the keyboard's settings activity. On its own this is
nuisance-grade — screen-real-estate denial and interaction disruption — but a keyboard that can be
raised on command by a third party is a useful primitive for UI-redress and phishing sequences
(HK-13 notes the absence of tapjacking protection). The receiver is only registered when the
"keyboard notification" preference is enabled, which bounds exposure.

**Recommendation.** Pass a `signature`-level permission as the fourth argument to `registerReceiver`,
or switch to explicit-component `PendingIntent`s (HK-05) so the receiver never needs to be exported
at all, and register with `Context.RECEIVER_NOT_EXPORTED` once targeting API 33+.

---

### HK-07 — Voice input offered in password fields

**Severity** Medium **CWE** CWE-200
**Location** `LatinIME.java:915-918`, `:786-799`, `:2010-2012`

The code goes out of its way to compute the password state early, explaining exactly why:

```java
// LatinIME.java:786-789
// Most such things we decide below in the switch statement, but we need to know
// now whether this is a password text field, because we need to know now (before
// the switch statement) whether we want to enable the voice button.
mPasswordText = false;
```

…and then never uses it for that purpose, because the function that was supposed to make the
decision was left as a generated stub:

```java
// LatinIME.java:915-918
private boolean shouldShowVoiceButton(EditorInfo attribute) {
    // TODO Auto-generated method stub
    return true;
}
```

`mPasswordText` is referenced exactly once thereafter, to disable prediction (`:839`). The
microphone key is therefore offered in password fields, and pressing it starts a recognition session
(`:2010-2012`) handed to whichever component resolves `android.speech.action.RECOGNIZE_SPEECH` via
the bundled `voiceimeutils.jar` — on most devices a cloud recognizer, and in principle any installed
app that registers as the handler. The calling package name is passed along as an extra.

Note also that `mPasswordText` is only set for `TYPE_CLASS_TEXT` (`:793-796`), so a numeric PIN
field (`TYPE_CLASS_NUMBER` + `TYPE_NUMBER_VARIATION_PASSWORD`) is not classified as a password field
at all.

**Impact.** A user who taps the mic in a password or PIN field speaks their credential into an
off-device recognition service, which then returns it as text to be inserted. This is a
user-initiated action, which limits it — but the keyboard is presenting the affordance in precisely
the context where it should not, and the author's own comment shows that was not the intent.

**Recommendation.** Implement `shouldShowVoiceButton` to return false when `mPasswordText` is set,
when the field is `TYPE_CLASS_NUMBER` with `TYPE_NUMBER_VARIATION_PASSWORD`, and when
`IME_FLAG_NO_PERSONALIZED_LEARNING` or `TYPE_TEXT_FLAG_NO_SUGGESTIONS` marks the field sensitive.

---

### HK-08 — Severely outdated target SDK and dependency set

**Severity** Low (but amplifies HK-05, HK-06, HK-10, HK-03) **CWE** CWE-1104, CWE-1035
**Location** `app/build.gradle`, `build.gradle`

| Item | Value | Note |
|------|-------|------|
| `compileSdkVersion` / `targetSdkVersion` | 26 | Android 8.0, 2017 |
| `minSdkVersion` | 14 | Android 4.0, 2011 |
| Android Gradle Plugin | 3.2.1 | 2018 |
| Support library | 26.0.0 / 27.1.1 | pre-AndroidX, unmaintained |
| Repository | `jcenter()` | frozen read-only since 2021, no security updates |
| `minifyEnabled` | false | no R8/ProGuard on release |
| Gradle wrapper | `gradlew` present, `gradle/wrapper/` absent | build not reproducible from a clean clone |

**Impact.** `targetSdkVersion 26` opts the application out of platform mitigations that would
otherwise have neutralised several findings in this report at zero code cost:

* API 30 — package-visibility filtering (would narrow HK-02's discovery surface)
* API 31 — mandatory `FLAG_IMMUTABLE` on `PendingIntent` (HK-05)
* API 31 — mandatory explicit `android:exported` (HK-10)
* API 31 — `dataExtractionRules` for scoped backup (HK-03)
* API 33 — mandatory export flag on runtime-registered receivers (HK-06)

`jcenter()` is a supply-chain concern in its own right: the repository is frozen, so any artifact
resolved from it will never receive a security fix, and dependency resolution silently depends on
infrastructure that is no longer maintained.

**Recommendation.** Raise `compileSdkVersion`/`targetSdkVersion` to a current level and fix the
resulting build breakage; replace `jcenter()` with `mavenCentral()`; migrate to AndroidX; enable R8
for release builds; commit the Gradle wrapper properties (and verify the wrapper JAR's checksum) so
the build is reproducible. The README already acknowledges the target-SDK problem as a distribution
blocker; it is a security matter as well.

---

### HK-09 — Unverifiable prebuilt JAR in the build

**Severity** Low **CWE** CWE-1357 (reliance on insufficiently trustworthy component)
**Location** `app/libs/voiceimeutils.jar`; `app/build.gradle` (`implementation fileTree(include: ['*.jar'], dir: 'libs')`)

A 20 KB prebuilt JAR containing 21 classes under `com.google.android.voiceime`, all with a build
timestamp of **2011-12-08**, is checked into the repository and pulled into the APK by a wildcard
`fileTree` dependency. No source, no checksum, no version, no upstream URL and no provenance note
exist anywhere in the repo. Its classes bind services and dispatch
`android.speech.action.RECOGNIZE_SPEECH` intents (`ActivityHelper`, `ServiceBridge`, `ServiceHelper`,
`IntentApiTrigger`) from inside the IME process.

The contents are consistent with the AOSP voice-IME helper of that era, so this is a provenance and
reproducibility gap rather than evidence of anything malicious. But nothing in the repository lets a
reviewer or downstream packager (e.g. F-Droid) establish that independently, and the wildcard
`fileTree` means any JAR dropped into `libs/` is silently linked in.

**Recommendation.** Replace with a source dependency or a versioned artifact resolved from a
repository with checksum verification; at minimum, record the SHA-256 and upstream origin in the
repo and replace the wildcard with an explicit file reference.

---

### HK-10 — Six implicitly exported activities

**Severity** Low **CWE** CWE-926
**Location** `app/src/main/AndroidManifest.xml:28-88`

`Main`, `LatinIMESettings`, `InputLanguageSelection`, `PrefScreenActions`, `PrefScreenView` and
`PrefScreenFeedback` all declare `<intent-filter>` blocks and none declares `android:exported`.
Under `targetSdkVersion 26` the presence of an intent filter makes each implicitly exported. Each
preference screen is additionally reachable through both `android.intent.action.MAIN` with
`CATEGORY_DEFAULT` and a custom action (`org.pocketworkstation.pckeyboard.PREFS_ACTIONS`, `…PREFS_VIEW`,
`…PREFS_FEEDBACK`, `…SETTINGS`, `…INPUT_LANGUAGE_SELECTION`).

**Impact.** Any app can launch any of the keyboard's settings screens directly. These screens change
keyboard behaviour rather than exposing data, so direct impact is limited — but combined with HK-06
(force the keyboard up) and HK-13 (no obscured-touch filtering), being able to place a specific
settings screen in front of the user on demand is a useful phishing and UI-redress building block.

**Recommendation.** Declare `android:exported` explicitly on every component: `true` for `Main` only,
`false` elsewhere. Remove `android.intent.action.MAIN` from the preference screens; the IME's
`settingsActivity` attribute in `res/xml/method.xml` is the supported entry point.

---

### HK-11 — Home-grown signing-certificate check

**Severity** Low **CWE** CWE-327 (broken algorithm), CWE-654 (insufficient verification)
**Location** `LatinIMESettings.java:117-136`

```java
PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), PackageManager.GET_SIGNATURES);
for (Signature sig : info.signatures) {
    byte[] b = sig.toByteArray();
    int out = 0;
    for (int i = 0; i < b.length; ++i) {
        int pos = i % 4;
        out ^= b[i] << (pos * 4);
    }
    if (out == -466825) { isOfficial = true; }
}
version += isOfficial ? " official" : " custom";
```

A 32-bit XOR fold of the certificate bytes compared against a magic constant. This is not a hash:
collisions are trivial to construct, so any repackager who wants the "official" label can have it.
`GET_SIGNATURES` is also deprecated and, on older releases, susceptible to the Janus signature-
confusion issue (CVE-2017-13156).

**Impact.** Today the result only drives a cosmetic label in the settings screen, so real-world
impact is negligible. It is reported because it *looks* like an integrity check, and code that looks
like an integrity check tends to acquire security-relevant callers over time — for instance as an
allowlist mechanism for HK-02.

**Recommendation.** Remove it, or reimplement with `GET_SIGNING_CERTIFICATES` and a full SHA-256
comparison of the certificate — which is also the primitive needed to fix HK-02 properly.

---

### HK-13 — No tapjacking protection

**Severity** Low **CWE** CWE-1021 (improper restriction of rendered UI layers)
**Location** application-wide — `filterTouchesWhenObscured` / `setFilterTouchesWhenObscured` /
`onFilterTouchEventForSecurity` appear nowhere in the sources or layouts

Neither the preference activities nor the keyboard's own views (including the popup mini-keyboard)
reject touch events delivered while another window overlays them.

**Impact.** An app holding `SYSTEM_ALERT_WINDOW` can overlay the keyboard settings screens with
decoy UI and harvest taps that change keyboard behaviour, or overlay the keyboard itself. Combined
with HK-06 (any app can force the keyboard to appear) and HK-10 (any app can open a chosen settings
screen), the pieces for a full redress sequence are all present.

**Recommendation.** Set `android:filterTouchesWhenObscured="true"` on the root view of each settings
layout and on `LatinKeyboardView`/the popup keyboard container.

### HK-12 — Dormant keystroke-logging code

**Severity** Informational
**Location** `TextEntryState.java:34`, `:83-91`, `:258-270`

```java
private static boolean LOGGING = false;
...
if (LOGGING) {
    sKeyLocationFile = context.openFileOutput("key.txt", Context.MODE_APPEND);
    sUserActionFile  = context.openFileOutput("action.txt", Context.MODE_APPEND);
}
...
public static void keyPressedAt(Key key, int x, int y) {
    if (LOGGING && sKeyLocationFile != null && key.codes[0] >= 32) {
        String out = "KEY: " + (char) key.codes[0] + " X: " + x + " Y: " + y + ...;
        sKeyLocationFile.write(out.getBytes());
    }
}
```

When enabled this appends the literal character and touch coordinates of every printable key press
to a plaintext file in the app's private storage. `LOGGING` is `false` and is never assigned at
runtime, so **this code is unreachable in shipped builds** and is not a live vulnerability.

It is called out because it is precisely the artifact that converts a one-shot IME compromise (see
HK-01/HK-02) into a durable keylog, because the flag is `private static` but **not `final`** — so it
is a one-character edit or a reflective write away from being live — and because the file it writes
is not covered by any backup-exclusion rule (HK-03).

**Recommendation.** Delete the logging paths, or make the flag `static final boolean LOGGING =
BuildConfig.DEBUG` so it cannot survive into a release build.

---

### HK-14 — Dead contacts-harvesting code retained

**Severity** Informational
**Location** `ContactsDictionary.java`; `LatinIME.java:167`, `:618-619`, `:634`, `:651-652`

`ContactsDictionary` queries `ContactsContract.Contacts.CONTENT_URI` for every display name on the
device and feeds them into the suggestion trie, registering a `ContentObserver` to reload on change.
Every call site is commented out, and `READ_CONTACTS` is **not** requested in the manifest — so the
app does not read contacts today, and the class would fail at runtime if it did.

Reported for completeness: the class is still compiled into the APK, its re-enablement is a
three-line uncomment, and it contains no consent flow or runtime-permission handling of its own —
it was written for the pre-Marshmallow install-time permission model.

**Recommendation.** Delete the class. If contact-based suggestions are ever wanted, reintroduce them
with a runtime permission request, an explicit user opt-in, and exclusion from learned-word
persistence.

---

---

## 4. What the code does right

These were verified across the whole source tree, not assumed. For an input method they are the
properties that matter most, and the app holds all of them.

| Property | Evidence |
|----------|----------|
| **No network access whatsoever** | No `INTERNET` permission in the manifest; no `URL`, `HttpURLConnection`, `Socket`, `OkHttp`, or `Retrofit` usage anywhere in the sources. The keyboard is structurally incapable of exfiltrating typed text. |
| **Minimal permission set** | Exactly three: `VIBRATE`, `READ_USER_DICTIONARY`, `WRITE_USER_DICTIONARY`. No contacts, no storage, no location, no `RECORD_AUDIO` (voice input is delegated). |
| **No telemetry or analytics** | No third-party SDKs of any kind; the only non-source dependency is `voiceimeutils.jar` (HK-09). |
| **No dynamic code loading** | No `DexClassLoader`, `PathClassLoader`, `loadClass` or reflective instantiation of external code. HK-02 loads *data*, not code. |
| **No WebView** | No `WebView`, no `loadUrl`, no `addJavascriptInterface`. The About screen renders static `Html.fromHtml` into a `TextView`. |
| **No clipboard access** | No `ClipboardManager` usage — a keyboard reading the clipboard would be a significant privacy surface, and this one does not. |
| **Password fields suppress prediction, candidates and learning** | `LatinIME.java:790-796`, `:839`; the candidate strip is hidden and learning is gated behind full-correction mode, which is off for those fields. |
| **Parameterised SQL throughout** | `AutoDictionary.java:198-210`, `UserBigramDictionary.java:217`, `:243-256` use selection args and `ContentValues`. String-concatenated SQL appears only in `CREATE`/`DROP` DDL built from compile-time constants — no injection path. |
| **No secrets in the repository** | No API keys, credentials, keystores or signing material; `.gitignore` excludes build output. |
| **All data stays in app-private storage** | No external-storage writes, no `MODE_WORLD_READABLE`/`MODE_WORLD_WRITEABLE`, no exported `ContentProvider`. |
| **Contacts harvesting is disabled** | `ContactsDictionary` call sites are commented out and the permission is not requested (HK-14). |

---

## 5. Verified-clean checklist

Checks performed that produced no finding:

| Check | Result |
|-------|--------|
| Hardcoded secrets, tokens, keystores | None |
| Exported `ContentProvider` / `Service` | None (the IME service is correctly guarded by `BIND_INPUT_METHOD`) |
| `android:debuggable` in the manifest | Not set |
| Cleartext network configuration | N/A — no network usage |
| SQL injection | None — parameterised throughout |
| Path traversal / arbitrary file write | None — no filesystem paths derived from input |
| Insecure randomness / home-grown crypto | No crypto used at all (except HK-11's checksum, which is not crypto and is not used as such) |
| Reflection / dynamic dispatch on external input | None |
| Native `system()` / `exec()` | None |
| Intent redirection in exported components | None — no exported component forwards an `Intent` extra to `startActivity` |
| `setResult`/`InputConnection` data leaks to other apps | None found |
| Log statements containing typed text | None — the enabled `Log` calls print lifecycle and configuration only; keystroke logging is dormant (HK-12) |

---

## 6. Prioritised remediation plan

**Tier 1 — the exploitable chain.** These are the only findings that reach code execution, and they
are worth fixing even in a codebase that is otherwise frozen. Fixing **either** breaks the chain;
fixing HK-02 is much cheaper and should come first.

1. **HK-02** — require a signature match (or an explicit, persisted user confirmation naming the
   package) before any third-party dictionary is loaded. ~50 lines in `PluginManager`.
2. **HK-01** — add the depth and length bounds, route all `mDict` reads through a checked accessor,
   and stand up a fuzz harness over `getSuggestions`/`getBigrams` with ASan to confirm coverage.

**Tier 2 — privacy of typed text.** Highest user-visible value per line changed.

3. **HK-03** — add backup exclusion rules for the learned-word databases (manifest-only change).
4. **HK-04** — honour `IME_FLAG_NO_PERSONALIZED_LEARNING` (a handful of lines in `onStartInputView`).
5. **HK-07** — implement `shouldShowVoiceButton` as its own comment describes (three lines).

**Tier 3 — IPC surface.** Mostly fixed for free by the target-SDK bump; do that first, then clean up
what the platform then flags.

6. **HK-08** → then **HK-05**, **HK-06**, **HK-10**, **HK-13**.

**Tier 4 — hygiene.** HK-09 (record JAR provenance), HK-11 (remove or do properly), HK-12 (delete the
dormant logger), HK-14 (delete the dead contacts reader).

---

## 7. Methodology, scope and limitations

**Method.** Manual static review of the complete source tree at commit `9202d9d`: the Android
manifest, Gradle and CMake build configuration, all 44 Java sources, all 6 native sources, the
resource tree, the bundled prebuilt JAR (inspected via class-constant extraction), and the
repository history and CI configuration. Reasoning about the native findings was done by tracing
the data flow from `PluginManager` → `BinaryDictionary` → JNI bridge → `Dictionary` and checking
each buffer write against its allocation site.

**Not performed.** This review did not build the project, run it on a device or emulator, fuzz the
dictionary parser, develop or validate any exploit, reverse-engineer the released Play Store APK,
review the separate dictionary-pack APKs published by the author, or assess the project's wiki and
distribution channels.

**Consequences for the findings.** The HK-01 memory-safety issues are established from the code with
high confidence — the missing bounds checks are unambiguous. The step from "memory corruption" to
"code execution" is reasoned, not demonstrated; a crash-level denial of service is certain, and
exploitability would need a PoC against a real device to state definitively. Everything else in this
report is a direct reading of the code and configuration.

**Severity model.** Severities are assigned for the application as configured — in particular for
`targetSdkVersion 26`, which is what determines whether several platform mitigations apply. A
modern target SDK would reduce HK-05, HK-06 and HK-10 to informational.

**Context.** The project README states that this is a 2011-era codebase in maintenance mode, no
longer receiving significant updates. Nothing in this review contradicts that; the findings should
be read as a description of the risk a user accepts by running it, and as a prioritised list for
anyone maintaining, forking or packaging it.
