# Prebuilt dependencies

Binaries in this directory are linked into an input method — a process that
observes every keystroke on the device — so each one must be identifiable and
verifiable. Nothing is picked up by wildcard: `app/build.gradle` references each
file explicitly.

## voiceimeutils.jar

| | |
|---|---|
| SHA-256 | `dc8de4ded83fbf9c11fc55a6b9b5fcf271ce72982330f00945a735741823194c` |
| Size | 21076 bytes |
| Class file timestamps | 2011-12-08 |
| Package | `com.google.android.voiceime` (21 classes) |
| Origin | The AOSP voice-IME helper of the Gingerbread/Honeycomb era, as shipped with the Android sample IMEs. No upstream coordinate or release tag is recorded in this repository's history. |

Verify with:

    sha256sum app/libs/voiceimeutils.jar

### What it does

Provides `VoiceRecognitionTrigger`, which the keyboard uses for the microphone
key. It dispatches `android.speech.action.RECOGNIZE_SPEECH` and binds to the
resolved recognition service; on most devices that means the spoken audio
leaves the device. The keyboard therefore refuses to offer the microphone in
password fields and in fields flagged as sensitive — see
`LatinIME.shouldShowVoiceButton()`.

### Known limitation

This is a prebuilt binary with no source in this repository and no upstream
artifact coordinate, so a reviewer or downstream packager (F-Droid, for
instance) cannot rebuild it from source. Replacing it with a source dependency,
or with a direct `RecognizerIntent` call, would remove the last unverifiable
component from the build. Tracked as HK-09 in `SECURITY-REVIEW.md`.
