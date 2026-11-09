# Contributing

```bash
flutter pub get
flutter run -d <android-or-ios-device>
```

The audio engine is C++. Android uses the NDK that ships with Flutter /
Android Studio. iOS builds the same engine with CMake from Xcode (`brew
install cmake`). arm64 devices are the intended target.

SideDeck is GNU GPL v3 or later. By sending a patch you license it under the
same terms.
