## Overview ##

> **This branch:** security-hardened and modernised. The app targets Android 16
> (API 36) and the findings from `SECURITY-REVIEW.md` are fixed — see
> `SECURITY-FIXES.md` for what changed, how it was verified, and what was
> deliberately left alone. See **Building** below for the toolchain.


**WARNING:** *This is a rather ancient project that was originally developed back in 2011 based on the Android 2.3 (Gingerbread) AOSP keyboard. While it still works as-is for many users, it would need some major rewrites to work with newer APIs, and some features such as language switching or popup keys don't work right on modern Android systems. I'm not currently planning on significant updates, and it's possible that it will stop working on modern devices or will no longer be updateable via the Google Play store due to minimum API level requirements. Play Store requires targeting API level 29 (Android 10), while the code was written for API level 9 (Android 2.3) from 2011.*

Are you missing the key layout you're used to from your computer when using an Android device? This software keyboard has separate number keys, punctuation in the usual places, and arrow keys. It is based on the AOSP Gingerbread soft keyboard, so it supports multitouch for the modifier keys.

This keyboard is especially useful if you use ConnectBot for SSH access. It provides working Tab/Ctrl/Esc keys, and the arrow keys are essential for devices such as the Xoom tablet or Nexus S that don't have a trackball or D-Pad.

The supported keyboard layouts include Armenian (Հայերեն), Arabic (العربية),
British (en\_GB), Bulgarian (български език), Czech (Čeština), Danish (dansk),
Carpalx English (language "en-CX"), Dvorak English (language "en-DV"), English
(QWERTY), Finnish (Suomi), French (Français, AZERTY), German (Deutsch, QWERTZ),
German Neo2 (Deutsch, language "de-NE"),
Greek (ελληνικά), Hebrew (עברית), Hungarian (Magyar), Italian (Italiano), Lao
(ພາສາລາວ), Norwegian (Norsk bokmål), Persian (فارسی), Portuguese (Português),
Romanian (Română), Russian (Русский), Russian phonetic (Русский, ru-rPH),
Serbian (Српски), Slovak (Slovenčina), Slovenian
(Slovenščina)/Bosnian/Croatian/Latin Serbian, Spanish (Español, Español
Latinoamérica), Swedish (Svenska), Tamil (தமிழ்), Thai (ไทย), Turkish (Türkçe),
and Ukrainian (українська мова).

To install, get **[Hacker's
Keyboard](https://play.google.com/store/apps/details?id=org.pocketworkstation.pckeyboard)**
from the Play Store, plus optional [dictionary
packs](https://play.google.com/store/apps/developer?id=Klaus+Weidner).

## Building ##

Toolchain used for the released builds of this branch:

| | |
|---|---|
| JDK | 17 (`JAVA_HOME` must point at it) |
| Gradle | 8.11.1, fetched by the wrapper and pinned by `distributionSha256Sum` |
| Android Gradle Plugin | 8.10.0 |
| compileSdk / targetSdk | 36 |
| minSdk | 21 |
| NDK | 28.2.13676358 — pinned because r28+ links with the 16 KB page alignment Android 15+ requires |
| CMake | 3.22.1, pinned in `app/build.gradle` |

```sh
export JAVA_HOME=/path/to/jdk-17
echo "sdk.dir=$HOME/Android/Sdk" > local.properties
./gradlew :app:assembleDebug
```

`assembleRelease` signs the APK if `keystore.properties` (gitignored) or the
matching `HK_STORE_FILE` / `HK_STORE_PASSWORD` / `HK_KEY_ALIAS` /
`HK_KEY_PASSWORD` environment variables are present; without them the release
APK is simply left unsigned, so a clean clone still builds.

### Security checks ###

```sh
tools/dict-fuzz/run.sh           # dictionary parser under ASan/UBSan
tools/lint-security-gate.sh      # runs lint, fails on Security-category findings
```

The build does not abort on lint: upstream carries hundreds of pre-existing
translation and namespace findings, and failing on those would just mean nobody
runs it. The security gate is separate and does fail, which is what CI enforces.

The parser harness runs the regression inputs from the reviews plus several
thousand mutated dictionaries; `run.sh /path/to/other/cpp` points it at another
copy of the parser, which is how the unpatched upstream one was confirmed to
fail. Both run on every push — see `.github/workflows/ci.yml`.

## Additional resources ##

See the **[Release Notes](https://github.com/klausw/hackerskeyboard/wiki/ReleaseNotes)** for changes in the Play Store released versions.

Having problems? See the **[User's Guide](https://github.com/klausw/hackerskeyboard/wiki/UsersGuide)** and **[FAQ](https://github.com/klausw/hackerskeyboard/wiki/FrequentlyAskedQuestions)**, and check the [issue tracker](https://github.com/klausw/hackerskeyboard/issues) for known bugs or filing new ones.

Comments, requests, or contributions? Join the [discussion group](http://groups.google.com/group/hackerskeyboard/).

Application developers: see [the page about keyboard support in applications](https://github.com/klausw/hackerskeyboard/wiki/KeyboardSupportInApplications) if you want to enable the additional keys in your Android application, the same method also works for hardware USB or Bluetooth keyboards.

![hk-5row-en-s.png](hk-5row-en-s.png)
