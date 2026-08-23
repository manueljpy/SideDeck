const keyNames = [
  'C',
  'C#',
  'D',
  'D#',
  'E',
  'F',
  'F#',
  'G',
  'G#',
  'A',
  'A#',
  'B',
];

/// Camelot major (B) for pitch class 0=C .. 11=B.
const camelotMajor = [
  '8B',
  '3B',
  '10B',
  '5B',
  '12B',
  '7B',
  '2B',
  '9B',
  '4B',
  '11B',
  '6B',
  '1B',
];

/// Camelot minor (A): relative to major root+3.
String camelotMinor(int root) {
  final major = camelotMajor[(root + 3) % 12];
  return '${major.substring(0, major.length - 1)}A';
}

/// [key] 0..11 major, 12..23 minor, else unknown.
String formatKey(int key) {
  if (key >= 0 && key <= 11) {
    return '${keyNames[key]} / ${camelotMajor[key]}';
  }
  if (key >= 12 && key <= 23) {
    final root = key - 12;
    return '${keyNames[root]}m / ${camelotMinor(root)}';
  }
  return '—';
}

String formatDuration(double seconds) {
  if (seconds.isNaN || seconds <= 0) return '0:00';
  final m = seconds.floor() ~/ 60;
  final s = seconds.floor() % 60;
  return '$m:${s.toString().padLeft(2, '0')}';
}

/// `Artist - Title` from a file basename. No dash → title only.
({String title, String artist}) splitTitleArtist(String basename) {
  var name = basename.trim();
  name = name.replaceAll(RegExp(r'_spotdown\.org$', caseSensitive: false), '');
  final dash = name.indexOf(' - ');
  if (dash <= 0 || dash >= name.length - 3) {
    return (title: name, artist: '');
  }
  return (
    artist: name.substring(0, dash).trim(),
    title: name.substring(dash + 3).trim(),
  );
}

enum TrackSort { name, bpm, key }

int compareTrackSort(
  TrackSort sort,
  String titleA,
  String artistA,
  double? bpmA,
  int? keyA,
  String titleB,
  String artistB,
  double? bpmB,
  int? keyB,
) {
  int byName() => titleA.toLowerCase().compareTo(titleB.toLowerCase());
  switch (sort) {
    case TrackSort.name:
      final c = byName();
      return c != 0 ? c : artistA.toLowerCase().compareTo(artistB.toLowerCase());
    case TrackSort.bpm:
      final a = bpmA == null || bpmA <= 0 ? 1e9 : bpmA;
      final b = bpmB == null || bpmB <= 0 ? 1e9 : bpmB;
      final c = a.compareTo(b);
      return c != 0 ? c : byName();
    case TrackSort.key:
      final c = _camelot(keyA).compareTo(_camelot(keyB));
      return c != 0 ? c : byName();
  }
}

({int number, bool minor})? camelotWheel(int? key) {
  if (key == null || key < 0 || key > 23) return null;
  final label = key <= 11 ? camelotMajor[key] : camelotMinor(key - 12);
  return (
    number: int.parse(label.substring(0, label.length - 1)),
    minor: label.endsWith('A'),
  );
}

/// Same Camelot number (incl. relative major/minor) or ±1 on the wheel.
bool keysCompatible(int a, int b) {
  final x = camelotWheel(a);
  final y = camelotWheel(b);
  if (x == null || y == null) return false;
  if (x.number == y.number) return true;
  final dn = (x.number - y.number).abs();
  return x.minor == y.minor && (dn == 1 || dn == 11);
}

int _camelot(int? key) {
  final c = camelotWheel(key);
  if (c == null) return 1000;
  return c.number * 2 + (c.minor ? 0 : 1);
}
