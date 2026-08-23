import 'dart:io';

import 'package:flutter/material.dart';
import 'package:path/path.dart' as p;
import 'package:sidedeck/engine/analysis_cache.dart';
import 'package:sidedeck/engine/music_utils.dart';
import 'package:sidedeck/engine/track_analyzer.dart';
import 'package:sidedeck/state/dj_controller.dart';
import 'package:sidedeck/state/library_controller.dart';
import 'package:sidedeck/subsonic/subsonic_client.dart';
import 'package:sidedeck/theme/sidedeck_theme.dart';

/// Modal library overlay (opened from the Library button).
class LibraryOverlay extends StatefulWidget {
  const LibraryOverlay({
    super.key,
    required this.dj,
    required this.library,
    this.onOpenSettings,
  });

  final DjController dj;
  final LibraryController library;
  final VoidCallback? onOpenSettings;

  @override
  State<LibraryOverlay> createState() => _LibraryOverlayState();
}

class _LibraryOverlayState extends State<LibraryOverlay> {
  SubsonicClient? _client;
  List<SubsonicTrack> _remote = [];
  List<SubsonicPlaylist> _playlists = [];
  SubsonicPlaylist? _openPlaylist;
  bool _searching = false;
  bool _remoteLoading = false;
  String? _remoteError;
  String? _remoteBusyId;
  String? _remoteBusyLabel;
  final Map<String, TrackAnalysis> _remoteMeta = {};
  final _cache = AnalysisCache.instance;
  final _search = TextEditingController();
  int _tab = 0;

  @override
  void initState() {
    super.initState();
    _loadClient();
    widget.library.addListener(_onLib);
    widget.dj.addListener(_onLib);
  }

  void _onLib() {
    if (mounted) setState(() {});
  }

  Future<void> _loadClient() async {
    _client = await SubsonicClient.fromPrefs();
    if (!mounted) return;
    setState(() {});
    if (_client != null) await _loadPlaylists();
  }

  @override
  void dispose() {
    widget.library.removeListener(_onLib);
    widget.dj.removeListener(_onLib);
    _search.dispose();
    super.dispose();
  }

  Future<void> _hydrateRemoteMeta(List<SubsonicTrack> tracks) async {
    for (final t in tracks) {
      final path = await _client?.cachedPathIfPresent(t);
      if (path == null) continue;
      try {
        final stat = await File(path).stat();
        final cached = await _cache.get(
          path,
          stat.size,
          stat.modified.millisecondsSinceEpoch,
        );
        if (cached != null) {
          _remoteMeta[t.id] = cached;
        }
      } catch (_) {}
    }
  }

  Future<void> _loadPlaylists() async {
    if (_client == null) return;
    setState(() {
      _remoteLoading = true;
      _remoteError = null;
      _searching = false;
      _openPlaylist = null;
      _remote = [];
    });
    try {
      final lists = await _client!.playlists();
      lists.sort((a, b) => a.name.toLowerCase().compareTo(b.name.toLowerCase()));
      if (!mounted) return;
      setState(() => _playlists = lists);
    } catch (e) {
      if (!mounted) return;
      setState(() => _remoteError = e.toString());
    } finally {
      if (mounted) setState(() => _remoteLoading = false);
    }
  }

  Future<void> _openRemotePlaylist(SubsonicPlaylist playlist) async {
    if (_client == null) return;
    setState(() {
      _remoteLoading = true;
      _remoteError = null;
      _searching = false;
      _openPlaylist = playlist;
      _remote = [];
    });
    try {
      final tracks = await _client!.playlistTracks(playlist.id);
      await _hydrateRemoteMeta(tracks);
      if (!mounted) return;
      setState(() => _remote = tracks);
    } catch (e) {
      if (!mounted) return;
      setState(() => _remoteError = e.toString());
    } finally {
      if (mounted) setState(() => _remoteLoading = false);
    }
  }

  Future<void> _searchRemote(String q) async {
    if (_client == null) return;
    final query = q.trim();
    if (query.isEmpty) {
      await _loadPlaylists();
      return;
    }
    setState(() {
      _remoteLoading = true;
      _remoteError = null;
      _searching = true;
      _openPlaylist = null;
    });
    try {
      final tracks = await _client!.search(query);
      await _hydrateRemoteMeta(tracks);
      if (!mounted) return;
      setState(() => _remote = tracks);
    } catch (e) {
      if (!mounted) return;
      setState(() => _remoteError = e.toString());
    } finally {
      if (mounted) setState(() => _remoteLoading = false);
    }
  }

  Future<void> _loadRemote(SubsonicTrack track, int deck) async {
    if (_client == null) return;
    setState(() {
      _remoteBusyId = track.id;
      _remoteBusyLabel = 'Downloading…';
    });
    try {
      final path = await _client!.cacheTrack(track);
      if (mounted) {
        setState(() => _remoteBusyLabel = 'Loading…');
      }
      final ok = await widget.dj.loadFile(
        deck,
        path,
        title: track.title,
        artist: track.artist,
      );
      if (ok) {
        widget.library.upsertAnalysis(
          path,
          bpm: widget.dj.deck(deck).bpm,
          key: widget.dj.deck(deck).key,
        );
        _remoteMeta[track.id] = TrackAnalysis(
          bpm: widget.dj.deck(deck).bpm,
          key: widget.dj.deck(deck).key,
          beatOffset: widget.dj.deck(deck).beatOffset,
        );
        if (mounted) Navigator.pop(context);
      } else if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text(widget.dj.engineError ?? 'Load failed')),
        );
      }
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Load failed: $e')),
      );
    } finally {
      if (mounted) {
        setState(() {
          _remoteBusyId = null;
          _remoteBusyLabel = null;
        });
      }
    }
  }

  Future<void> _analyzeRemote(SubsonicTrack track) async {
    if (_client == null || _remoteBusyId != null) return;
    setState(() {
      _remoteBusyId = track.id;
      _remoteBusyLabel = 'Downloading…';
    });
    try {
      final path = await _client!.cacheTrack(track);
      if (mounted) setState(() => _remoteBusyLabel = 'Analyzing…');
      final result = await analyzeTrackFileOffUi(path);
      if (result == null) {
        throw Exception('Analyze failed');
      }
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
      if (!mounted) return;
      setState(() => _remoteMeta[track.id] = result);
      for (var deck = 0; deck < 2; deck++) {
        final d = widget.dj.deck(deck);
        if (d.path == path) {
          await widget.dj.loadFile(
            deck,
            path,
            title: track.title,
            artist: track.artist,
          );
        }
      }
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Analyze failed: $e')),
      );
    } finally {
      if (mounted) {
        setState(() {
          _remoteBusyId = null;
          _remoteBusyLabel = null;
        });
      }
    }
  }

  Future<void> _analyzeLocal(LibraryTrack t) async {
    await widget.library.analyzePath(t.path);
    for (var deck = 0; deck < 2; deck++) {
      final d = widget.dj.deck(deck);
      if (d.path == t.path) {
        await widget.dj.loadFile(
          deck,
          t.path,
          title: t.title,
          artist: t.artist,
        );
      }
    }
  }

  Future<void> _loadLocal(LibraryTrack t, int deck) async {
    final ok = await widget.dj.loadFile(
      deck,
      t.path,
      title: t.title,
      artist: t.artist,
    );
    if (ok) {
      widget.library.upsertAnalysis(
        t.path,
        bpm: widget.dj.deck(deck).bpm,
        key: widget.dj.deck(deck).key,
      );
      if (mounted) Navigator.pop(context);
    }
  }

  @override
  Widget build(BuildContext context) {
    final tracks = _sorted(
      widget.library.filtered,
      (t) => (title: t.title, artist: t.artist, bpm: t.bpm, key: t.key),
    );
    final lib = widget.library;

    return Material(
      color: SideDeckTheme.panel,
      child: SafeArea(
        left: false,
        right: false,
        child: Column(
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(8, 4, 4, 4),
              child: Row(
                children: [
                  _tabBtn('Local', 0),
                  _tabBtn('Remote', 1),
                  const SizedBox(width: 8),
                  Expanded(
                    child: SizedBox(
                      height: 36,
                      child: TextField(
                        controller: _search,
                        style: const TextStyle(fontSize: 13),
                        decoration: InputDecoration(
                          hintText: _tab == 0 ? 'Filter…' : 'Search server, or browse playlists',
                          isDense: true,
                          contentPadding: const EdgeInsets.symmetric(horizontal: 8, vertical: 8),
                          prefixIcon: const Icon(Icons.search, size: 18),
                          prefixIconConstraints: const BoxConstraints(minWidth: 32, minHeight: 32),
                          border: OutlineInputBorder(
                            borderRadius: BorderRadius.circular(8),
                          ),
                        ),
                        onChanged: (v) {
                          if (_tab == 0) widget.library.setQuery(v);
                          if (_tab == 1 && v.trim().isEmpty && _searching) {
                            _loadPlaylists();
                          }
                        },
                        onSubmitted: (v) {
                          if (_tab == 1) _searchRemote(v);
                        },
                      ),
                    ),
                  ),
                  PopupMenuButton<TrackSort>(
                    tooltip: 'Sort',
                    padding: EdgeInsets.zero,
                    icon: const Icon(Icons.sort, size: 22),
                    onSelected: widget.library.setSort,
                    itemBuilder: (context) => [
                      _sortItem(TrackSort.name, 'Name'),
                      _sortItem(TrackSort.bpm, 'BPM'),
                      _sortItem(TrackSort.key, 'Key'),
                    ],
                  ),
                  if (_tab == 0) ...[
                    if (lib.analyzing)
                      IconButton(
                        tooltip: 'Stop analysis',
                        visualDensity: VisualDensity.compact,
                        onPressed: lib.cancelAnalyze,
                        icon: const Icon(Icons.stop, size: 22),
                      )
                    else
                      PopupMenuButton<String>(
                        tooltip: 'Analyze BPM / grid',
                        enabled: lib.tracks.isNotEmpty,
                        padding: EdgeInsets.zero,
                        icon: const Icon(Icons.graphic_eq, size: 22),
                        onSelected: (v) {
                          if (v == 'missing') {
                            lib.analyzeMissing();
                          } else if (v == 'all') {
                            lib.analyzeAll();
                          }
                        },
                        itemBuilder: (context) => [
                          PopupMenuItem(
                            value: 'missing',
                            enabled: lib.unanalyzedCount > 0,
                            child: Text(
                              lib.unanalyzedCount > 0
                                  ? 'Analyze missing (${lib.unanalyzedCount})'
                                  : 'Analyze missing',
                            ),
                          ),
                          const PopupMenuItem(
                            value: 'all',
                            child: Text('Re-analyze all'),
                          ),
                        ],
                      ),
                    IconButton(
                      tooltip: 'Pick files',
                      visualDensity: VisualDensity.compact,
                      onPressed: lib.loading || lib.analyzing ? null : lib.pickFiles,
                      icon: const Icon(Icons.audio_file, size: 22),
                    ),
                    IconButton(
                      tooltip: 'Pick folder',
                      visualDensity: VisualDensity.compact,
                      onPressed: lib.loading || lib.analyzing ? null : lib.pickFolder,
                      icon: const Icon(Icons.folder_open, size: 22),
                    ),
                  ],
                  IconButton(
                    visualDensity: VisualDensity.compact,
                    onPressed: () => Navigator.pop(context),
                    icon: const Icon(Icons.close, size: 22),
                  ),
                ],
              ),
            ),
            if (lib.analyzing || widget.dj.loadingDeck != null || lib.error != null)
              Padding(
                padding: const EdgeInsets.fromLTRB(12, 0, 12, 4),
                child: _statusLine(lib),
              ),
            if (_tab == 1 && _openPlaylist != null) _playlistHeader(),
            Expanded(
              child: _tab == 0 ? _localList(tracks) : _remoteList(),
            ),
          ],
        ),
      ),
    );
  }

  CheckedPopupMenuItem<TrackSort> _sortItem(TrackSort value, String label) {
    return CheckedPopupMenuItem(
      value: value,
      checked: widget.library.sort == value,
      child: Text(label),
    );
  }

  List<T> _sorted<T>(
    List<T> src,
    ({String title, String artist, double? bpm, int? key}) Function(T) meta,
  ) {
    final list = List<T>.from(src);
    list.sort((a, b) {
      final x = meta(a);
      final y = meta(b);
      return compareTrackSort(
        widget.library.sort,
        x.title,
        x.artist,
        x.bpm,
        x.key,
        y.title,
        y.artist,
        y.bpm,
        y.key,
      );
    });
    return list;
  }

  Widget? _trackArtist(String artist, {String? status}) {
    final line = (status == null || status.isEmpty)
        ? artist
        : (artist.isEmpty ? status : '$artist · $status');
    if (line.isEmpty) return null;
    return Text(
      line,
      maxLines: 1,
      overflow: TextOverflow.ellipsis,
      style: const TextStyle(color: SideDeckTheme.muted, fontSize: 12),
    );
  }

  Color? _compatibleKeyColor(int? key) {
    if (key == null || key < 0) return null;
    Color? color;
    for (var i = 0; i < 2; i++) {
      final d = widget.dj.deck(i);
      if (!d.loaded || d.key < 0 || !keysCompatible(d.key, key)) continue;
      final accent = i == 0 ? SideDeckTheme.accentA : SideDeckTheme.accentB;
      if (color != null && color != accent) return SideDeckTheme.text;
      color = accent;
    }
    return color;
  }

  Widget _trackMeta(double? bpm, int? key, double durationSec) {
    final hasBpm = bpm != null && bpm > 0;
    final hasKey = key != null && key >= 0;
    final keyColor = _compatibleKeyColor(key);
    return Padding(
      padding: const EdgeInsets.only(left: 4, right: 2),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.end,
        children: [
          Text(
            hasBpm ? bpm.toStringAsFixed(1) : '—',
            style: TextStyle(
              fontSize: 13,
              fontWeight: FontWeight.w700,
              color: hasBpm ? SideDeckTheme.text : SideDeckTheme.muted,
            ),
          ),
          if (durationSec > 0 || hasKey)
            Text.rich(
              TextSpan(
                style: const TextStyle(color: SideDeckTheme.muted, fontSize: 11),
                children: [
                  if (durationSec > 0) TextSpan(text: formatDuration(durationSec)),
                  if (durationSec > 0 && hasKey) const TextSpan(text: '  '),
                  if (hasKey)
                    TextSpan(
                      text: formatKey(key),
                      style: keyColor == null
                          ? null
                          : TextStyle(color: keyColor, fontWeight: FontWeight.w700),
                    ),
                ],
              ),
            ),
        ],
      ),
    );
  }

  Widget _tabBtn(String label, int i) {
    final on = _tab == i;
    return Padding(
      padding: const EdgeInsets.only(right: 2),
      child: TextButton(
        style: TextButton.styleFrom(
          minimumSize: const Size(0, 32),
          padding: const EdgeInsets.symmetric(horizontal: 10),
          visualDensity: VisualDensity.compact,
          foregroundColor: on ? Colors.white : SideDeckTheme.muted,
          backgroundColor: on ? SideDeckTheme.panelAlt : Colors.transparent,
        ),
        onPressed: () {
          setState(() => _tab = i);
          if (i == 1 && _client != null && !_searching && _openPlaylist == null && _playlists.isEmpty) {
            _loadPlaylists();
          }
        },
        child: Text(
          label,
          style: TextStyle(
            fontSize: 13,
            fontWeight: on ? FontWeight.w800 : FontWeight.w500,
          ),
        ),
      ),
    );
  }

  Widget _statusLine(LibraryController lib) {
    if (lib.error != null) {
      return Text(
        lib.error!,
        maxLines: 1,
        overflow: TextOverflow.ellipsis,
        style: const TextStyle(color: Colors.redAccent, fontSize: 12),
      );
    }
    if (lib.analyzing) {
      return Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          LinearProgressIndicator(
            minHeight: 2,
            value: lib.analyzeTotal == 0 ? null : lib.analyzeDone / lib.analyzeTotal,
          ),
          const SizedBox(height: 2),
          Text(
            'Analyzing ${lib.analyzeDone + 1}/${lib.analyzeTotal}'
            '${lib.analyzingPath == null ? '' : ' — ${p.basename(lib.analyzingPath!)}'}',
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: const TextStyle(fontSize: 11, color: SideDeckTheme.muted),
          ),
        ],
      );
    }
    return Row(
      children: [
        const SizedBox(
          width: 12,
          height: 12,
          child: CircularProgressIndicator(strokeWidth: 2),
        ),
        const SizedBox(width: 8),
        Expanded(
          child: Text(
            'Loading ${widget.dj.loadingTitle ?? 'track'} → '
            '${widget.dj.loadingDeck == 0 ? 'A' : 'B'}',
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: const TextStyle(fontSize: 11, color: SideDeckTheme.muted),
          ),
        ),
      ],
    );
  }

  Widget _songRow({
    required bool busyThis,
    required String analyzeTooltip,
    required VoidCallback? onAnalyze,
    required String title,
    required String artist,
    String? status,
    required double? bpm,
    required int? key,
    required double durationSec,
    required List<Widget> actions,
  }) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(2, 4, 8, 4),
      child: Row(
        children: [
          SizedBox(
            width: 32,
            height: 32,
            child: busyThis
                ? const Padding(
                    padding: EdgeInsets.all(6),
                    child: CircularProgressIndicator(strokeWidth: 2),
                  )
                : IconButton(
                    tooltip: analyzeTooltip,
                    visualDensity: VisualDensity.compact,
                    padding: EdgeInsets.zero,
                    constraints: const BoxConstraints.tightFor(width: 32, height: 32),
                    onPressed: onAnalyze,
                    icon: const Icon(Icons.graphic_eq, size: 20),
                  ),
          ),
          const SizedBox(width: 4),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: [
                Text(title, maxLines: 1, overflow: TextOverflow.ellipsis),
                ?_trackArtist(artist, status: status),
              ],
            ),
          ),
          _trackMeta(bpm, key, durationSec),
          const SizedBox(width: 6),
          ...actions,
        ],
      ),
    );
  }

  Widget _localList(List<LibraryTrack> tracks) {
    final lib = widget.library;
    if (lib.loading) {
      return const Center(child: CircularProgressIndicator());
    }
    if (tracks.isEmpty) {
      return const Center(
        child: Padding(
          padding: EdgeInsets.all(24),
          child: Text(
            'Tap the folder icon, choose your music folder.\n'
            'Then Analyze (waveform icon) for BPM/key, or tap A / B to load.',
            textAlign: TextAlign.center,
            style: TextStyle(color: SideDeckTheme.muted),
          ),
        ),
      );
    }
    return ListView.builder(
      itemCount: tracks.length,
      itemBuilder: (context, i) {
        final t = tracks[i];
        final analyzingThis = lib.analyzingPath == t.path;
        final loadingThis = widget.dj.loadingPath == t.path;
        final busy = lib.analyzing || widget.dj.loadingDeck != null;
        return _songRow(
          busyThis: analyzingThis,
          analyzeTooltip: t.hasAnalysis
              ? 'Re-analyze BPM / grid'
              : 'Analyze BPM / key',
          onAnalyze: busy ? null : () => _analyzeLocal(t),
          title: t.title,
          artist: t.artist,
          bpm: t.bpm,
          key: t.key,
          durationSec: t.durationMs / 1000.0,
          actions: [
            _deckBtn('A', busy ? null : () => _loadLocal(t, 0), loadingThis && widget.dj.loadingDeck == 0),
            const SizedBox(width: 8),
            _deckBtn('B', busy ? null : () => _loadLocal(t, 1), loadingThis && widget.dj.loadingDeck == 1),
          ],
        );
      },
    );
  }

  Widget _deckBtn(String label, VoidCallback? onPressed, [bool loading = false]) {
    return FilledButton(
      style: FilledButton.styleFrom(
        minimumSize: const Size(36, 32),
        padding: const EdgeInsets.symmetric(horizontal: 10),
        tapTargetSize: MaterialTapTargetSize.shrinkWrap,
        visualDensity: VisualDensity.compact,
      ),
      onPressed: onPressed,
      child: loading
          ? const SizedBox(
              width: 14,
              height: 14,
              child: CircularProgressIndicator(strokeWidth: 2),
            )
          : Text(label),
    );
  }

  Widget _playlistHeader() {
    final pl = _openPlaylist!;
    final count = pl.songCount > 0 ? '${pl.songCount}' : '${_remote.length}';
    return Padding(
      padding: const EdgeInsets.fromLTRB(4, 0, 12, 4),
      child: Row(
        children: [
          IconButton(
            tooltip: 'Playlists',
            visualDensity: VisualDensity.compact,
            onPressed: _loadPlaylists,
            icon: const Icon(Icons.arrow_back, size: 20),
          ),
          Expanded(
            child: Text(
              pl.name,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: const TextStyle(fontWeight: FontWeight.w700),
            ),
          ),
          Text(
            '$count tracks',
            style: const TextStyle(color: SideDeckTheme.muted, fontSize: 12),
          ),
        ],
      ),
    );
  }

  Widget _playlistList() {
    if (_playlists.isEmpty) {
      return const Center(
        child: Text(
          'No playlists on this server yet.\n'
          'Search and press enter to find tracks.',
          textAlign: TextAlign.center,
          style: TextStyle(color: SideDeckTheme.muted),
        ),
      );
    }
    return ListView.builder(
      itemCount: _playlists.length,
      itemBuilder: (context, i) {
        final pl = _playlists[i];
        final subtitle = [
          if (pl.owner.isNotEmpty) pl.owner,
          if (pl.songCount > 0) '${pl.songCount} tracks',
          if (pl.duration > 0) formatDuration(pl.duration.toDouble()),
        ].join(' · ');
        return ListTile(
          dense: true,
          leading: const Icon(Icons.queue_music, size: 22),
          title: Text(pl.name, maxLines: 1, overflow: TextOverflow.ellipsis),
          subtitle: subtitle.isEmpty
              ? null
              : Text(
                  subtitle,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(color: SideDeckTheme.muted, fontSize: 12),
                ),
          trailing: const Icon(Icons.chevron_right, size: 20, color: SideDeckTheme.muted),
          onTap: () => _openRemotePlaylist(pl),
        );
      },
    );
  }

  Widget _remoteList() {
    if (_client == null) {
      return Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            const Text(
              'Set up a Subsonic server in Settings first.',
              style: TextStyle(color: SideDeckTheme.muted),
            ),
            const SizedBox(height: 8),
            FilledButton(
              onPressed: widget.onOpenSettings,
              child: const Text('Open settings'),
            ),
          ],
        ),
      );
    }
    if (_remoteLoading) {
      return const Center(child: CircularProgressIndicator());
    }
    if (_remoteError != null) {
      return Center(
        child: Text(_remoteError!, style: const TextStyle(color: Colors.redAccent)),
      );
    }
    if (!_searching && _openPlaylist == null) {
      return _playlistList();
    }
    if (_remote.isEmpty) {
      return Center(
        child: Text(
          _searching
              ? 'No matching tracks.\nClear the box to go back to playlists.'
              : 'This playlist is empty.',
          textAlign: TextAlign.center,
          style: const TextStyle(color: SideDeckTheme.muted),
        ),
      );
    }
    final remote = _sorted(
      _remote,
      (t) {
        final m = _remoteMeta[t.id];
        return (title: t.title, artist: t.artist, bpm: m?.bpm, key: m?.key);
      },
    );
    return ListView.builder(
      itemCount: remote.length,
      itemBuilder: (context, i) {
        final t = remote[i];
        final meta = _remoteMeta[t.id];
        final busyThis = _remoteBusyId == t.id;
        final busy = _remoteBusyId != null || widget.dj.loadingDeck != null;
        return _songRow(
          busyThis: busyThis,
          analyzeTooltip: meta == null
              ? 'Download & analyze'
              : 'Re-analyze BPM / grid',
          onAnalyze: busy ? null : () => _analyzeRemote(t),
          title: t.title,
          artist: t.artist,
          status: busyThis ? _remoteBusyLabel : null,
          bpm: meta?.bpm,
          key: meta?.key,
          durationSec: t.duration.toDouble(),
          actions: [
            _deckBtn('A', busy ? null : () => _loadRemote(t, 0)),
            const SizedBox(width: 8),
            _deckBtn('B', busy ? null : () => _loadRemote(t, 1)),
          ],
        );
      },
    );
  }
}
