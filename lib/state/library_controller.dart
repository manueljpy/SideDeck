import 'dart:io';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/foundation.dart';
import 'package:path/path.dart' as p;
import 'package:permission_handler/permission_handler.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:sidedeck/engine/analysis_cache.dart';
import 'package:sidedeck/engine/music_utils.dart';
import 'package:sidedeck/engine/track_analyzer.dart';

class LibraryTrack {
  LibraryTrack({
    required this.path,
    required this.title,
    this.artist = '',
    this.durationMs = 0,
    this.bpm,
    this.key,
  });

  final String path;
  final String title;
  final String artist;
  final int durationMs;
  final double? bpm;
  final int? key;

  bool get hasAnalysis => bpm != null && bpm! > 0;

  LibraryTrack copyWith({double? bpm, int? key}) {
    return LibraryTrack(
      path: path,
      title: title,
      artist: artist,
      durationMs: durationMs,
      bpm: bpm ?? this.bpm,
      key: key ?? this.key,
    );
  }
}

class LibraryController extends ChangeNotifier {
  LibraryController() {
    restore();
  }

  final AnalysisCache _cache = AnalysisCache.instance;
  final List<LibraryTrack> tracks = [];
  String query = '';
  TrackSort sort = TrackSort.name;
  bool loading = false;
  String? error;
  String? folderPath;
  bool _disposed = false;

  bool analyzing = false;
  bool _cancelAnalyze = false;
  int analyzeDone = 0;
  int analyzeTotal = 0;
  String? analyzingPath;

  List<LibraryTrack> get filtered {
    final q = query.trim().toLowerCase();
    if (q.isEmpty) return List.unmodifiable(tracks);
    return tracks
        .where(
          (t) =>
              t.title.toLowerCase().contains(q) ||
              t.artist.toLowerCase().contains(q),
        )
        .toList();
  }

  int get unanalyzedCount => tracks.where((t) => !t.hasAnalysis).length;

  Future<bool> ensurePermission() async {
    if (!Platform.isAndroid) return true;
    final audio = await Permission.audio.request();
    if (audio.isGranted) return true;
    final storage = await Permission.storage.request();
    return storage.isGranted;
  }

  void _emit() {
    if (!_disposed) notifyListeners();
  }

  @override
  void dispose() {
    _disposed = true;
    _cancelAnalyze = true;
    super.dispose();
  }

  Future<void> restore() async {
    loading = true;
    _emit();
    try {
      final prefs = await SharedPreferences.getInstance();
      folderPath = prefs.getString('library_folder');
      final extra = prefs.getStringList('library_paths') ?? [];
      if (folderPath != null && folderPath!.isNotEmpty) {
        await scanDirectory(folderPath!, persist: false);
      }
      for (final path in extra) {
        await _addPath(path);
      }
    } catch (e) {
      error = e.toString();
    }
    loading = false;
    _emit();
  }

  Future<void> _persist() async {
    final prefs = await SharedPreferences.getInstance();
    if (folderPath != null) {
      await prefs.setString('library_folder', folderPath!);
    }
    await prefs.setStringList(
      'library_paths',
      [
        for (final t in tracks)
          if (folderPath == null || !p.isWithin(folderPath!, t.path)) t.path,
      ],
    );
  }

  Future<void> pickFiles() async {
    loading = true;
    error = null;
    _emit();
    try {
      await ensurePermission();
      final result = await FilePicker.platform.pickFiles(
        type: FileType.custom,
        allowedExtensions: const ['mp3', 'wav', 'MP3', 'WAV'],
        allowMultiple: true,
        withData: false,
      );
      if (result == null) {
        loading = false;
        _emit();
        return;
      }
      for (final f in result.files) {
        final path = f.path;
        if (path == null) continue;
        await _addPath(path);
      }
      await _persist();
    } catch (e) {
      error = e.toString();
    }
    loading = false;
    _emit();
  }

  Future<void> pickFolder() async {
    loading = true;
    error = null;
    _emit();
    try {
      await ensurePermission();
      final dir = await FilePicker.platform.getDirectoryPath();
      if (dir == null) {
        loading = false;
        _emit();
        return;
      }
      folderPath = dir;
      await scanDirectory(dir);
    } catch (e) {
      error = e.toString();
    }
    loading = false;
    _emit();
  }

  Future<void> scanDirectory(String dirPath, {bool persist = true}) async {
    final dir = Directory(dirPath);
    if (!await dir.exists()) return;
    await for (final entity in dir.list(recursive: true, followLinks: false)) {
      if (entity is! File) continue;
      final ext = p.extension(entity.path).toLowerCase();
      if (ext == '.mp3' || ext == '.wav') {
        await _addPath(entity.path);
      }
    }
    if (persist) await _persist();
    _emit();
  }

  Future<void> _addPath(String path) async {
    if (tracks.any((t) => t.path == path)) return;
    final name = p.basenameWithoutExtension(path);
    final parsed = splitTitleArtist(name);
    double? bpm;
    int? key;
    try {
      final stat = await File(path).stat();
      final cached = await _cache.get(
        path,
        stat.size,
        stat.modified.millisecondsSinceEpoch,
      );
      if (cached != null && cached.bpm > 0) {
        bpm = cached.bpm;
        key = cached.key;
      }
    } catch (_) {}
    tracks.add(LibraryTrack(
      path: path,
      title: parsed.title,
      artist: parsed.artist,
      durationMs: (fileDurationSec(path) * 1000).round(),
      bpm: bpm,
      key: key,
    ));
  }

  void setQuery(String q) {
    query = q;
    _emit();
  }

  void setSort(TrackSort value) {
    if (sort == value) return;
    sort = value;
    _emit();
  }

  void upsertAnalysis(String path, {double? bpm, int? key}) {
    final i = tracks.indexWhere((t) => t.path == path);
    if (i < 0) return;
    tracks[i] = tracks[i].copyWith(bpm: bpm, key: key);
    _emit();
  }

  void cancelAnalyze() {
    _cancelAnalyze = true;
  }

  Future<void> analyzePath(String path) async {
    if (analyzing) return;
    await _analyzeList([path]);
  }

  Future<void> analyzeMissing() async {
    final pending = tracks.where((t) => !t.hasAnalysis).map((t) => t.path).toList();
    await _analyzeList(pending);
  }

  Future<void> analyzeAll() async {
    await _analyzeList(tracks.map((t) => t.path).toList());
  }

  Future<void> _analyzeList(List<String> paths) async {
    if (paths.isEmpty || analyzing) return;
    analyzing = true;
    _cancelAnalyze = false;
    analyzeDone = 0;
    analyzeTotal = paths.length;
    error = null;
    _emit();
    try {
      for (final path in paths) {
        if (_cancelAnalyze) break;
        analyzingPath = path;
        _emit();
        final result = await analyzeTrackFileOffUi(path);
        analyzeDone++;
        if (result == null) continue;
        try {
          final stat = await File(path).stat();
          await _cache.put(
            path: path,
            size: stat.size,
            mtime: stat.modified.millisecondsSinceEpoch,
            bpm: result.bpm,
            key: result.key,
            beatOffset: result.beatOffset,
          );
        } catch (_) {}
        upsertAnalysis(path, bpm: result.bpm, key: result.key);
      }
    } catch (e) {
      error = e.toString();
    }
    analyzing = false;
    analyzingPath = null;
    analyzeTotal = 0;
    analyzeDone = 0;
    _emit();
  }
}

