import 'dart:convert';

import 'package:crypto/crypto.dart';
import 'package:http/http.dart' as http;
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'dart:io';

class SubsonicTrack {
  SubsonicTrack({
    required this.id,
    required this.title,
    required this.artist,
    this.album = '',
    this.duration = 0,
    this.coverArt,
    this.suffix = '',
  });

  final String id;
  final String title;
  final String artist;
  final String album;
  final int duration;
  final String? coverArt;
  final String suffix;

  static SubsonicTrack fromJson(Map<String, dynamic> m) {
    return SubsonicTrack(
      id: '${m['id']}',
      title: '${m['title'] ?? 'Unknown'}',
      artist: '${m['artist'] ?? ''}',
      album: '${m['album'] ?? ''}',
      duration: (m['duration'] as num?)?.toInt() ?? 0,
      coverArt: m['coverArt']?.toString(),
      suffix: '${m['suffix'] ?? ''}',
    );
  }
}

class SubsonicPlaylist {
  SubsonicPlaylist({
    required this.id,
    required this.name,
    this.owner = '',
    this.songCount = 0,
    this.duration = 0,
  });

  final String id;
  final String name;
  final String owner;
  final int songCount;
  final int duration;

  static SubsonicPlaylist fromJson(Map<String, dynamic> m) {
    return SubsonicPlaylist(
      id: '${m['id']}',
      name: '${m['name'] ?? 'Playlist'}',
      owner: '${m['owner'] ?? ''}',
      songCount: (m['songCount'] as num?)?.toInt() ?? 0,
      duration: (m['duration'] as num?)?.toInt() ?? 0,
    );
  }
}

class SubsonicClient {
  SubsonicClient({
    required this.baseUrl,
    required this.username,
    required this.password,
  });

  final String baseUrl;
  final String username;
  final String password;

  static const _clientName = 'SideDeck';
  static const _apiVersion = '1.16.1';

  Uri _uri(String endpoint, [Map<String, String>? extra]) {
    final salt = DateTime.now().millisecondsSinceEpoch.toRadixString(16);
    final token = md5.convert(utf8.encode('$password$salt')).toString();
    final params = <String, String>{
      'u': username,
      't': token,
      's': salt,
      'v': _apiVersion,
      'c': _clientName,
      'f': 'json',
      ...?extra,
    };
    final root = baseUrl.endsWith('/') ? baseUrl.substring(0, baseUrl.length - 1) : baseUrl;
    return Uri.parse('$root/rest/$endpoint').replace(queryParameters: params);
  }

  /// Binary stream/download — omit `f=json` so the body is audio, not a JSON wrapper.
  Uri _mediaUri(String endpoint, [Map<String, String>? extra]) {
    final salt = DateTime.now().millisecondsSinceEpoch.toRadixString(16);
    final token = md5.convert(utf8.encode('$password$salt')).toString();
    final params = <String, String>{
      'u': username,
      't': token,
      's': salt,
      'v': _apiVersion,
      'c': _clientName,
      ...?extra,
    };
    final root = baseUrl.endsWith('/') ? baseUrl.substring(0, baseUrl.length - 1) : baseUrl;
    return Uri.parse('$root/rest/$endpoint').replace(queryParameters: params);
  }

  static bool _isNativeDecodable(String suffix) {
    final s = suffix.toLowerCase();
    return s == 'mp3' || s == 'wav' || s == 'wave';
  }

  static String? _sniffKind(List<int> bytes) {
    if (bytes.length < 12) return 'empty';
    if (bytes[0] == 0x7B || bytes[0] == 0x3C) return 'error';
    if (bytes[0] == 0x52 && bytes[1] == 0x49 && bytes[2] == 0x46 && bytes[3] == 0x46) {
      return 'wav';
    }
    if (bytes[0] == 0x49 && bytes[1] == 0x44 && bytes[2] == 0x33) return 'mp3';
    if (bytes[0] == 0xFF && (bytes[1] & 0xE0) == 0xE0) return 'mp3';
    if (bytes[0] == 0x66 && bytes[1] == 0x4C && bytes[2] == 0x61 && bytes[3] == 0x43) {
      return 'flac';
    }
    if (bytes[0] == 0x4F && bytes[1] == 0x67 && bytes[2] == 0x67 && bytes[3] == 0x53) {
      return 'ogg';
    }
    if (bytes.length > 8 &&
        bytes[4] == 0x66 &&
        bytes[5] == 0x74 &&
        bytes[6] == 0x79 &&
        bytes[7] == 0x70) {
      return 'm4a';
    }
    return null;
  }

  Future<Map<String, dynamic>> _get(String endpoint, [Map<String, String>? extra]) async {
    final res = await http.get(_uri(endpoint, extra)).timeout(const Duration(seconds: 20));
    if (res.statusCode != 200) {
      throw Exception('HTTP ${res.statusCode}');
    }
    final json = jsonDecode(res.body) as Map<String, dynamic>;
    final sub = json['subsonic-response'] as Map<String, dynamic>?;
    if (sub == null) throw Exception('Invalid Subsonic response');
    if (sub['status'] != 'ok') {
      final error = sub['error'] as Map<String, dynamic>?;
      throw Exception(error?['message'] ?? 'Subsonic error');
    }
    return sub;
  }

  Future<void> ping() async {
    await _get('ping.view');
  }

  /// Subsonic XML→JSON often emits one object instead of a one-element array.
  static List<Map<String, dynamic>> jsonMaps(dynamic value) {
    if (value == null) return const [];
    if (value is List) {
      return value.whereType<Map<String, dynamic>>().toList();
    }
    if (value is Map<String, dynamic>) return [value];
    return const [];
  }

  Future<List<SubsonicTrack>> search(String query) async {
    final sub = await _get('search3.view', {'query': query.isEmpty ? 'a' : query, 'songCount': '100'});
    final search = sub['searchResult3'] as Map<String, dynamic>? ?? {};
    return jsonMaps(search['song']).map(SubsonicTrack.fromJson).toList();
  }

  Future<List<SubsonicPlaylist>> playlists() async {
    final sub = await _get('getPlaylists.view');
    final wrap = sub['playlists'] as Map<String, dynamic>? ?? {};
    return jsonMaps(wrap['playlist']).map(SubsonicPlaylist.fromJson).toList();
  }

  Future<List<SubsonicTrack>> playlistTracks(String id) async {
    final sub = await _get('getPlaylist.view', {'id': id});
    final playlist = sub['playlist'] as Map<String, dynamic>? ?? {};
    return jsonMaps(playlist['entry']).map(SubsonicTrack.fromJson).toList();
  }

  /// Local path used for the download cache (whether or not the file exists).
  Future<String> cacheFilePath(SubsonicTrack track) async {
    final dir = await getTemporaryDirectory();
    final cacheDir = Directory(p.join(dir.path, 'sidedeck_cache'));
    if (!await cacheDir.exists()) {
      await cacheDir.create(recursive: true);
    }
    final safe = track.id.replaceAll(RegExp(r'[^a-zA-Z0-9_-]'), '_');
    final ext = track.suffix.toLowerCase() == 'wav' || track.suffix.toLowerCase() == 'wave'
        ? 'wav'
        : 'mp3';
    return p.join(cacheDir.path, '$safe.$ext');
  }

  Future<String?> cachedPathIfPresent(SubsonicTrack track) async {
    final path = await cacheFilePath(track);
    final file = File(path);
    if (await file.exists() && await file.length() > 1024) {
      return path;
    }
    return null;
  }

  /// Download full track to cache; returns local file path for the DJ engine.
  Future<String> cacheTrack(SubsonicTrack track) async {
    final path = await cacheFilePath(track);
    final file = File(path);
    if (await file.exists() && await file.length() > 1024) {
      return file.path;
    }

    final native = _isNativeDecodable(track.suffix);
    final uri = native
        ? _mediaUri('download.view', {'id': track.id})
        : _mediaUri('stream.view', {
            'id': track.id,
            'format': 'mp3',
            'maxBitRate': '320',
          });
    final res = await http.get(uri).timeout(const Duration(minutes: 5));
    if (res.statusCode != 200) {
      throw Exception('Download failed HTTP ${res.statusCode}');
    }
    final bytes = res.bodyBytes;
    if (bytes.length < 1024) {
      throw Exception('Download failed (empty or error body)');
    }
    final kind = _sniffKind(bytes);
    if (kind == 'error') {
      throw Exception('Server returned an error instead of audio');
    }
    if (kind == 'flac' || kind == 'ogg' || kind == 'm4a') {
      throw Exception(
        'Got $kind audio. SideDeck needs MP3 or WAV — enable MP3 transcoding (ffmpeg) on the server.',
      );
    }
    await file.writeAsBytes(bytes, flush: true);
    return file.path;
  }

  static Future<SubsonicClient?> fromPrefs() async {
    final prefs = await SharedPreferences.getInstance();
    final url = prefs.getString('subsonic_url');
    final user = prefs.getString('subsonic_user');
    final pass = prefs.getString('subsonic_pass');
    if (url == null || user == null || pass == null || url.isEmpty) {
      return null;
    }
    return SubsonicClient(baseUrl: url, username: user, password: pass);
  }

  static Future<void> savePrefs({
    required String url,
    required String user,
    required String pass,
  }) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString('subsonic_url', url);
    await prefs.setString('subsonic_user', user);
    await prefs.setString('subsonic_pass', pass);
  }
}
