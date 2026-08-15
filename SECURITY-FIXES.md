# Security remediation and platform modernisation

This branch takes Hacker's Keyboard from the state described in the security
review (commit `9202d9d`, targeting API 26) to a build that targets current
Android and no longer carries the findings that review reported.

Two things happened here, and they are separable:

1. **Modernisation** — the app now builds against and targets API 36 with a
   supported toolchain. Several findings were platform mitigations the app had
   opted out of by targeting API 26, so this half fixes them for free.
2. **Remediation** — the findings that needed actual code changes, the largest
   being the memory-unsafe dictionary parser.

## Status

| ID | Finding | Severity | Status | Commit |
|----|---------|----------|--------|--------|
| HK-01 | Memory-unsafe binary dictionary parser | High | Fixed, verified under ASan/UBSan | `af5bea8` |
| HK-02 | Any installed app can supply the dictionary | High | Fixed — trust required | `db16aef` |
| HK-03 | Learned text eligible for cloud/`adb` backup | Medium | Fixed — backup rules | `7168904` |
| HK-04 | `IME_FLAG_NO_PERSONALIZED_LEARNING` ignored | Medium | Fixed | `25768d8`, `a954f32` |
| HK-05 | Mutable implicit `PendingIntent`s | Medium | Fixed | `25768d8` |
| HK-06 | Unprotected exported receiver | Low | Fixed — manifest, not exported | `25768d8` |
| HK-07 | Voice input offered in password fields | Medium | Fixed | `25768d8` |
| HK-08 | Outdated target SDK and dependencies | Low | Fixed — API 36 | `7168904` |
| HK-09 | Unverifiable prebuilt JAR | Low | Documented, pinned; still prebuilt | `07935d9` |
| HK-10 | Implicitly exported activities | Low | Fixed | `7168904` |
| HK-11 | Home-grown signing-certificate check | Low | Fixed — real fingerprint | `07935d9` |
| HK-12 | Dormant keystroke logger | Info | Removed | `25768d8` |
| HK-13 | No tapjacking protection | Low | Fixed for settings screens (see note) | `db16aef` |
| HK-14 | Dead contacts-harvesting code | Info | Removed | `07935d9` |
| **HK-15** | **Exponential trie traversal (DoS)** | **Medium** | **Fixed** | `af5bea8` |

HK-15 was not in the original review. It was found by the fuzz harness added
here: see below.

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

- **No device or emulator testing.** Everything here is verified by compilation,
  by the host-side sanitizer harness, and by inspecting the built APK. The
  keyboard has not been typed on. The changes most likely to need real-device
  checking are the edge-to-edge inset padding on the settings screens, the
  notification permission flow on API 33+, and the new dictionary-pack screen.
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
