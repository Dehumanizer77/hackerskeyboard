# Security remediation and platform modernisation

This branch takes Hacker's Keyboard from the state described in the security
review (commit `9202d9d`, targeting API 26) to a build that targets current
Android and no longer carries the findings that review reported.

Two review documents sit in this repository, and the hyphen is the only thing
separating their names:

* `SECURITY-REVIEW.md` — the first review, findings **HK-01 … HK-14**.
* `SECURITY_REVIEW.md` — a second, independent review of upstream `9202d9d`
  dated 7 September 2026, findings **SR-01 … SR-05**.

Two things happened here, and they are separable:

1. **Modernisation** — the app now builds against and targets API 36 with a
   supported toolchain. Several findings were platform mitigations the app had
   opted out of by targeting API 26, so this half fixes them for free.
2. **Remediation** — the findings that needed actual code changes, the largest
   being the memory-unsafe dictionary parser.

## Status

| ID | Finding | Severity | Status | Commit |
|----|---------|----------|--------|--------|
| HK-01 | Memory-unsafe binary dictionary parser | High | Fixed, verified under ASan/UBSan | `c4dc907` |
| HK-02 | Any installed app can supply the dictionary | High | Fixed — trust required | `24ce894` |
| HK-03 | Learned text eligible for cloud/`adb` backup | Medium | Fixed — backup rules | `45de62a` |
| HK-04 | `IME_FLAG_NO_PERSONALIZED_LEARNING` ignored | Medium | Fixed | `f7bd5ac`, `e539df1` |
| HK-05 | Mutable implicit `PendingIntent`s | Medium | Fixed | `f7bd5ac` |
| HK-06 | Unprotected exported receiver | Low | Fixed — manifest, not exported | `f7bd5ac` |
| HK-07 | Voice input offered in password fields | Medium | Fixed | `f7bd5ac` |
| HK-08 | Outdated target SDK and dependencies | Low | Fixed — API 36 | `45de62a` |
| HK-09 | Unverifiable prebuilt JAR | Low | Documented, pinned; still prebuilt | `bc4f021` |
| HK-10 | Implicitly exported activities | Low | Fixed | `45de62a` |
| HK-11 | Home-grown signing-certificate check | Low | Fixed — real fingerprint | `bc4f021` |
| HK-12 | Dormant keystroke logger | Info | Removed | `f7bd5ac` |
| HK-13 | No tapjacking protection | Low | Fixed for settings screens (see note) | `24ce894` |
| HK-14 | Dead contacts-harvesting code | Info | Removed | `bc4f021` |
| **HK-15** | **Exponential trie traversal (DoS)** | **Medium** | **Fixed** | `c4dc907` |
| **HK-16** | **Fragment injection via exported settings activity** | **Medium** | **Fixed** | this commit |

HK-15 was not in the original review. It was found by the fuzz harness added
here: see below. HK-16 was not in either review — the lint gate added for SR-05
found it.

## Second review, 7 September 2026

A second review (`SECURITY_REVIEW.md`, in Slovak) was run against **upstream
`9202d9d`** — not against this branch — so most of what it reports is the same
ground as the table above, seen independently. Its findings map as follows.

| ID | Finding | Already covered by | Outstanding work, done here |
|----|---------|--------------------|------------------------------|
| SR-01 | Out-of-bounds writes walking the trie | HK-01, HK-15 (`c4dc907`) | Its two inputs added as fixed regression cases |
| SR-02 | Reads past the end of the dictionary | HK-01 native + JNI (`c4dc907`) | The **Java loader** still trusted `available()` — fixed |
| SR-03 | `IME_FLAG_NO_PERSONALIZED_LEARNING` ignored | HK-04 (`f7bd5ac`, `e539df1`) | — |
| SR-04 | Notification receiver callable by any app | HK-05, HK-06 (`f7bd5ac`) | — |
| SR-05 | Non-reproducible build, release lint off | HK-08 (`45de62a`) | Distribution checksum, CI, security lint gate, build docs |
| **HK-16** | **Fragment injection via the exported settings activity** | — | Found by the new lint gate; fixed |

All four of the review's sanitizer inputs (`header`, `truncated`, `bigram`,
`deep`) were run against this branch before anything was changed: all four
already passed clean, and all four still fail on upstream. They are now part of
`tools/dict-fuzz` so they stay checked.

### SR-02, the part that was still open

`BinaryDictionary.loadDictionary()` sized its buffer from
`InputStream.available()`, summed those sizes without a bound, and then did a
single channel read per part. `available()` is not the length of a stream, and
one read is not guaranteed to return everything: a short read left the tail of
the buffer as zeroes while the size handed to the parser still covered it, so
the parser walked bytes that were never in the file. The native side has been
bounds-checked since HK-01, so this was no longer memory-unsafe — it was the
parser being fed data the file never contained. Each part is now read to EOF,
the total is capped at 16 MB (the format can only address 4 MB), and the native
side is told how much was actually read.

### HK-16, fragment injection

`LatinIMESettings` is a `PreferenceActivity` and has to stay exported, because
the system Settings app opens the IME's settings screen by component. An
exported `PreferenceActivity` honours the `:android:show_fragment` extra, which
lets any installed app load an arbitrary `Fragment` into this process. None of
these screens hosts a fragment at all, so `isValidFragment()` now returns false
— on `PreferenceScreenBase` for every settings screen, and on
`LatinIMESettings` itself, which is the class the manifest exports.

This was not in either review's findings list. The lint gate added for SR-05
reported it on its first run, which is the argument for having the gate.

### SR-05, what "release lint" now means

`abortOnError` stays off: upstream carries several hundred pre-existing
`ExtraTranslation`, `NamespaceTypo` and `ResAuto` findings, some of them
fatal-severity, and a build that always fails is a build nobody lints. The
security checks are raised to fatal in `app/build.gradle`, and
`tools/lint-security-gate.sh` fails on any Security-category finding at Error or
Fatal severity. That script and the parser harness both run in CI
(`.github/workflows/ci.yml`). The Gradle distribution is pinned by
`distributionSha256Sum` (verified against the checksum Gradle publishes
alongside the distribution), so the wrapper will not execute a substituted
download.

## The chain that mattered

HK-01 and HK-02 were only dangerous together, and both are closed.

**HK-02.** Dictionary packs were discovered by intent filter and loaded
immediately — any installed app could declare an activity with action
`org.pocketworkstation.DICT` and have its bytes parsed by native code inside
the process that sees every keystroke. No signature check, no permission, no
user confirmation. Since the bundled `main.dict` is a 34-byte placeholder,
`Suggest` falls through to a plugin dictionary on the *normal* path, so this
was not a corner case.

Now: packs signed with the keyboard's own certificate are trusted implicitly
(the official packs keep working untouched); everything else is discovered but
inert until approved in **Settings → Dictionary packs**. Approval stores the
pack's signing-certificate digest, so a package replaced by one signed with a
different key loses trust automatically. Trust is re-checked at load time, not
just at discovery.

**HK-01.** The parser assumed a well-formed file. It is now bounded at every
read and write. See `app/src/main/cpp/dictionary.cpp`.

## Verification

`tools/dict-fuzz` compiles the parser for the host and runs it under
AddressSanitizer and UndefinedBehaviorSanitizer, against crafted dictionaries
(trie chains deeper than every output buffer, runaway bigram lists, truncated
files) and random/mutated buffers.

    ./tools/dict-fuzz/run.sh                       # this working tree
    ./tools/dict-fuzz/run.sh /path/to/other/cpp    # e.g. an unpatched copy

Against the **unpatched** parser it reproduces the review's findings in
seconds:

    dictionary.cpp:320  runtime error: index 128 out of bounds for type
                        'short unsigned int [128]'      (also index 136)
    dictionary.cpp:156  AddressSanitizer: heap-buffer-overflow READ in
                        Dictionary::getFreq

and one crafted input kept it at 100% CPU for over ten minutes — HK-15, an
exponential traversal. Child addresses are read from the file and may point
backwards, so the "tree" can contain cycles, and a node group can declare 255
children. On a phone that is an unresponsive keyboard on every keystroke.

Against this branch the same corpus completes in about four seconds with no
sanitizer report, and the functional checks confirm lookups still work:

    typed "key" -> [keyboard keys keyed]
    typed "the" -> [there their]
    typed "thi" -> [this]
    46- and 47-character words returned intact

A parser that returns nothing would also be "safe", so those functional checks
run first and fail the harness if suggestions stop coming back.

## What was NOT done

Stating these plainly, because they are the gaps in this work:

- **Almost no device testing.** Everything here is verified by compilation, by
  the host-side sanitizer harness, and by inspecting the built APK.

  The one exception is edge-to-edge layout, which *was* checked on a device and
  did fail: targeting API 35+ makes the IME window edge-to-edge, so the
  navigation bar - gesture pill, hide-keyboard affordance and IME-switcher globe
  - was drawn straight over the keyboard's bottom key row. Confirmed by building
  an otherwise identical APK at `targetSdk 34`, where the overlap disappears.
  `KeyboardSwitcher` also calls `setPadding(0, 0, 0, 0)` right after inflating
  the theme layout, discarding `keyboard_bottom_padding`, so nothing reserved
  that space. Fixed by applying the reported navigation-bar and display-cutout
  insets as bottom padding on the input view.

  Clearing the system controls turned out not to be the same as being far enough
  from them to aim at Ctrl, Alt or the arrow keys: taps that fell slightly low
  still landed on the hide-keyboard and switch-keyboard affordances. The gap
  below the bottom key row is now an adjustable setting (Settings > Bottom gap,
  default 16 dp on top of the system insets) rather than a fixed constant,
  because how much room that needs depends on the device.

  Still unverified on a device: the size of that default, the notification
  permission flow on API 33+, the new dictionary-pack settings screen, and the
  inset padding on the settings screens.
- **R8 is newly enabled.** Release builds are verified to keep the JNI entry
  point (`BinaryDictionary` and its native method names — renaming them breaks
  every dictionary lookup silently) and every XML-inflated View and Preference
  subclass. That is checked by inspecting the dex, not by running the APK.
- **HK-13 is only fixed for the settings screens.** `filterTouchesWhenObscured`
  is deliberately *not* set on the keyboard view itself: screen-filter and
  blue-light apps legitimately overlay the keyboard, and enabling it there
  would make the keyboard silently stop accepting touches for those users. The
  trade-off went the other way for settings, where overlays have no legitimate
  reason to be.
- **HK-09 is documented, not resolved.** `voiceimeutils.jar` is still a 2011
  prebuilt with no upstream coordinate; its SHA-256 and provenance gap are
  recorded in `app/libs/README.md`.
- **`minSdk` moved 14 → 21.** AndroidX requires it. Devices on Android 4.0–4.4
  can no longer install this build.
- **No fuzzing of the Java layer**, and no review of the separate
  dictionary-pack APKs.

## Building

Requires **JDK 17** (AGP 8.10). The Debian `openjdk-17-jre` package is not
enough — `javac` must be present.

    export JAVA_HOME=/path/to/jdk-17
    ./gradlew assembleDebug
    ./gradlew assembleRelease      # shrunk with R8

The build needs an Android SDK with platform 36 and NDK r28 (pinned in
`app/build.gradle` for 16 KB page alignment; AGP's default r27 emits 4 KB and
would not run correctly on 16 KB-page devices).
