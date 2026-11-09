<p align="center">
  <img src="fastlane/metadata/android/en-US/images/icon.png" alt="SideDeck" width="128">
</p>
<h2 align="center"><b>SideDeck</b></h2>
<h4 align="center">Free DJ app for Android and iPhone. Mix on your phone, or on hardware.</h4>

<p align="center">
  <a href="https://f-droid.org/packages/es.manifold.sidedeck"><img src="https://fdroid.gitlab.io/artwork/badge/get-it-on.svg" alt="Get it on F-Droid" height="80"></a>
  <a href="https://play.google.com/store/apps/details?id=es.manifold.sidedeck"><img src="https://play.google.com/intl/en_us/badges/static/images/badges/en_badge_web_generic.png" alt="Get it on Google Play" height="80"></a>
  <a href="https://apps.apple.com/app/id6827595789"><img src="https://tools.applemediaservices.com/api/badges/download-on-the-app-store/black/en-us?size=250x83" alt="Download on the App Store" height="54"></a>
</p>

Android and iPhone DJ deck for local files and a Subsonic music server.
Landscape dual decks, waveforms, and an internal mixer, or four-channel USB
out to an external mixer such as the Teenage Engineering EP-136 K.O. Sidekick.

![SideDeck](screenshots/mix.png)

## Requirements

- Flutter stable (SDK ^3.12) - see [install Flutter](https://docs.flutter.dev/get-started/install)
- Android: NDK (ships with Flutter / Android Studio); Android 8.0+ (API 26), arm64 recommended
- iPhone: macOS with Xcode 15+, CMake 3.22+ (`brew install cmake`); iOS 15+
- USB host / USB-C (or a Lightning camera adapter) if you use an external mixer

## Build

```bash
flutter pub get
flutter run -d <android-or-ios-device>
```

Debug APK:

```bash
flutter build apk --debug
```

Output: `build/app/outputs/flutter-apk/app-debug.apk`

iOS (macOS + Xcode + CMake):

```bash
flutter build ios --debug --no-codesign
```

Or open `ios/Runner.xcworkspace` in Xcode, pick your iPhone, and Run. The C++
engine is compiled by CMake during the Xcode build.

## What works today

- Dual-deck landscape UI (waveforms, no platters)
- Native engine (Oboe on Android, Core Audio on iOS), MP3 / WAV / FLAC / Opus / AAC (M4A); ALAC when the device has a decoder
- Internal mixer: 3-band EQ, filter, crossfader
- BPM / key analysis on load (cached in SQLite); Camelot labels
- Beat detector: Queen Mary TempoTrackV2 + Mixxx BeatUtils; key: Queen Mary GetKeyMode
- Sync, loops (½ / ×2 on the waveform), BPM ±0.1, temporary pitch bend
- Waveform tap jumps ±4 beats; GRID edits the beatgrid (1 beat or 5 ms)
- Local folder/file picker; sort; analyze missing or all (iPhone: Files app / Finder sharing)
- Library highlights keys compatible with a loaded deck
- Subsonic browse and cache-to-file
- External USB mixer: Deck A → channels 1–2, Deck B → channels 3–4

Keylock (pitch-held tempo) is on by default. Cue points and hot cues exist
in the engine; those controls are not on the live UI yet.

## External mixer

Turn on **Use external mixer** in Settings with a USB mixer plugged in. Deck A
goes out on channels 1–2 and Deck B on channels 3–4 so you mix and cue on the
hardware. The on-screen EQ and crossfader hide in this mode.

Works with USB mixers that take two stereo decks this way, including the
Teenage Engineering EP-136 K.O. Sidekick (set USB mode to **Multi**, not
Controller). On iPhone, use USB-C or a Lightning camera adapter; the mixer
must be class-compliant USB audio.

## Subsonic

[Subsonic](http://www.subsonic.org/pages/api.jsp) is an API for a music
library you host yourself. Servers such as [Navidrome](https://www.navidrome.org/)
speak it. In Settings you can point SideDeck at that server, browse albums
and tracks, and download them to the phone to mix like local files.

## Flutter

SideDeck is a Flutter application (Dart UI + FFI into C++). Docs:
[flutter.dev](https://docs.flutter.dev/).

## Project layout

```
lib/                 Dart UI, controllers, Subsonic client, FFI
native/engine/       C++ engine, EQ, analysis, iOS Core Audio
android/app/         Android host, USB AudioTrack output
ios/                 iPhone host, CMake engine build
third_party/         Oboe, Signalsmith, qm-dsp, dr_libs, ogg/opus/opusfile
test/                Dart tests
```

## License

SideDeck is free software under the [GNU GPL v3 or later](LICENSE).
Third-party licenses are listed in [NOTICE](NOTICE) and
[third_party/README.md](third_party/README.md).
