import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:sidedeck/engine/music_utils.dart';
import 'package:sidedeck/state/dj_controller.dart';
import 'package:sidedeck/state/library_controller.dart';
import 'package:sidedeck/theme/sidedeck_theme.dart';
import 'package:sidedeck/ui/library_drawer.dart';
import 'package:sidedeck/ui/mixer_strip.dart';
import 'package:sidedeck/ui/settings_page.dart';
import 'package:sidedeck/ui/waveform.dart';

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  late final DjController _dj;
  late final LibraryController _library;
  int? _gridEditDeck;

  @override
  void initState() {
    super.initState();
    SystemChrome.setPreferredOrientations(const [
      DeviceOrientation.landscapeLeft,
      DeviceOrientation.landscapeRight,
    ]);
    SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky);
    _dj = DjController();
    _library = LibraryController();
  }

  @override
  void dispose() {
    _dj.dispose();
    _library.dispose();
    SystemChrome.setPreferredOrientations(DeviceOrientation.values);
    super.dispose();
  }

  Future<void> _openSettings() async {
    await Navigator.of(context).push(
      MaterialPageRoute(builder: (_) => SettingsPage(dj: _dj)),
    );
  }

  Future<void> _openLibrary() async {
    await showModalBottomSheet<void>(
      context: context,
      isScrollControlled: true,
      useSafeArea: false,
      backgroundColor: Colors.transparent,
      builder: (context) {
        return FractionallySizedBox(
          heightFactor: 1,
          child: MediaQuery.removePadding(
            context: context,
            removeLeft: true,
            removeRight: true,
            child: LibraryOverlay(
              dj: _dj,
              library: _library,
              onOpenSettings: () async {
                Navigator.pop(context);
                await _openSettings();
              },
            ),
          ),
        );
      },
    );
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: Listenable.merge([_dj, _library]),
      builder: (context, _) {
        return Scaffold(
          backgroundColor: Colors.black,
          body: SafeArea(
            child: Column(
              children: [
                if (_dj.engineError != null) _errorBar(),
                Expanded(flex: 5, child: _waveDeck(0, SideDeckTheme.accentA)),
                Expanded(flex: 5, child: _waveDeck(1, SideDeckTheme.accentB)),
                if (_dj.externalMixer)
                  _mixerBar()
                else
                  Expanded(flex: 7, child: _mixerBar(expand: true)),
              ],
            ),
          ),
        );
      },
    );
  }

  Widget _mixerBar({bool expand = false}) {
    return Stack(
      fit: expand ? StackFit.expand : StackFit.loose,
      children: [
        MixerStrip(
          controller: _dj,
          onLibrary: _openLibrary,
          onSettings: _openSettings,
        ),
        if (_gridEditDeck != null)
          Positioned.fill(
            child: GestureDetector(
              behavior: HitTestBehavior.opaque,
              onTap: () => setState(() => _gridEditDeck = null),
            ),
          ),
      ],
    );
  }

  Widget _errorBar() {
    return Material(
      color: const Color(0xFF5C1A1A),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 8),
        child: Row(
          children: [
            const Icon(Icons.error_outline, size: 16, color: Color(0xFFFF8A80)),
            const SizedBox(width: 8),
            Expanded(
              child: Text(
                _dj.engineError!,
                maxLines: 2,
                overflow: TextOverflow.ellipsis,
                style: const TextStyle(fontSize: 12, color: Color(0xFFFFCDD2)),
              ),
            ),
            IconButton(
              visualDensity: VisualDensity.compact,
              tooltip: 'Dismiss',
              onPressed: _dj.clearError,
              icon: const Icon(Icons.close, size: 16),
            ),
          ],
        ),
      ),
    );
  }

  Widget _waveDeck(int i, Color accent) {
    final d = _dj.deck(i);
    final remaining = (d.duration - d.position).clamp(0.0, 99999.0);
    final info = d.loaded
        ? '${d.title}   ${_bpmLabel(d)}   ${formatKey(d.key)}   −${formatDuration(remaining)}'
        : 'Deck ${i == 0 ? 'A' : 'B'} — tap to load a track';

    return Padding(
      padding: const EdgeInsets.fromLTRB(4, 2, 4, 0),
      child: ClipRRect(
        borderRadius: BorderRadius.circular(4),
        child: LayoutBuilder(
          builder: (context, constraints) {
            final overview = WaveformPainter.overviewH;
            const pad = _overlayPad;
            final play = d.loaded
                ? (constraints.maxHeight - overview - 6).clamp(40.0, 120.0)
                : 0.0;
            final playFoot = pad + play;
            final nudgeFoot = pad + _wavePadSize * 2 + _wavePadGap;
            final w = constraints.maxWidth;
            final loopGap = WaveformPainter.jumpZoneLeft(w) - playFoot;
            final bpmGap = w - WaveformPainter.jumpZoneRight(w) - nudgeFoot;
            return Stack(
              fit: StackFit.expand,
              children: [
                WaveformView(
                  min: d.waveMin,
                  max: d.waveMax,
                  position: d.position,
                  duration: d.duration,
                  beatOffset: d.beatOffset,
                  bpm: d.bpm,
                  rate: d.rate,
                  color: accent,
                  loopEnabled: d.loopEnabled,
                  loopStart: d.loopStart,
                  loopEnd: d.loopEnd,
                  onSeek: d.loaded ? (t) => _dj.seek(i, t) : null,
                ),
                if (!d.loaded)
                  Positioned.fill(
                    child: GestureDetector(
                      behavior: HitTestBehavior.opaque,
                      onTap: _openLibrary,
                    ),
                  ),
                Positioned(
                  left: play + 8,
                  right: d.loaded ? 88 : 8,
                  top: overview + 3,
                  child: IgnorePointer(
                    child: Text(
                      info,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: TextStyle(
                        fontWeight: FontWeight.w700,
                        fontSize: 11,
                        color: d.loaded ? Colors.white : SideDeckTheme.muted,
                        shadows: const [
                          Shadow(color: Colors.black, blurRadius: 6),
                          Shadow(color: Colors.black, blurRadius: 12),
                        ],
                      ),
                    ),
                  ),
                ),
                if (d.loaded)
                  Positioned(
                    left: pad,
                    top: overview,
                    bottom: 0,
                    child: Center(child: _play(i, d.playing, accent, play)),
                  ),
                if (d.loaded)
                  Positioned(
                    right: pad,
                    bottom: pad,
                    child: Row(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        _holdNudge(i, faster: false, accent: accent),
                        const SizedBox(width: _wavePadGap),
                        _holdNudge(i, faster: true, accent: accent),
                      ],
                    ),
                  ),
                if (_gridEditDeck != null)
                  Positioned.fill(
                    child: GestureDetector(
                      behavior: HitTestBehavior.opaque,
                      onTap: () => setState(() => _gridEditDeck = null),
                    ),
                  ),
                if (d.loaded)
                  Positioned(
                    right: 4,
                    top: overview + 2,
                    child: _gridButton(i, accent),
                  ),
                if (d.loaded && _gridEditDeck == i)
                  Positioned(
                    left: play + 8,
                    right: nudgeFoot,
                    bottom: 4,
                    child: _gridEditorBar(i, accent),
                  ),
                if (d.loaded && _gridEditDeck != i && loopGap > 0)
                  Positioned(
                    left: playFoot,
                    width: loopGap,
                    bottom: pad,
                    child: Center(child: _waveLoop(i, accent)),
                  ),
                if (d.loaded && _gridEditDeck != i && bpmGap > 0)
                  Positioned(
                    left: WaveformPainter.jumpZoneRight(w),
                    width: bpmGap,
                    bottom: pad,
                    child: Center(child: _waveBpm(i, accent)),
                  ),
              ],
            );
          },
        ),
      ),
    );
  }

  String _bpmLabel(DeckState d) {
    final heard = d.bpm * d.rate;
    if ((d.rate - 1.0).abs() < 0.002) {
      return '${d.bpm.toStringAsFixed(1)} BPM';
    }
    return '${heard.toStringAsFixed(1)} BPM  (${d.bpm.toStringAsFixed(1)})';
  }

  Widget _play(int i, bool playing, Color accent, double size) {
    return Material(
      color: accent.withValues(alpha: 0.55),
      shape: const CircleBorder(),
      elevation: 0,
      child: InkWell(
        customBorder: const CircleBorder(),
        onTap: () => _dj.playPause(i),
        child: SizedBox(
          width: size,
          height: size,
          child: Icon(
            playing ? Icons.pause : Icons.play_arrow,
            color: Colors.white.withValues(alpha: 0.9),
            size: size * 0.55,
          ),
        ),
      ),
    );
  }

  Widget _gridButton(int deck, Color accent) {
    final on = _gridEditDeck == deck;
    return Material(
      color: on ? accent : const Color(0xCC111111),
      borderRadius: BorderRadius.circular(6),
      child: InkWell(
        onTap: () => setState(() => _gridEditDeck = on ? null : deck),
        borderRadius: BorderRadius.circular(6),
        child: Container(
          height: 32,
          padding: const EdgeInsets.symmetric(horizontal: 10),
          alignment: Alignment.center,
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(6),
            border: Border.all(color: accent.withValues(alpha: 0.85)),
          ),
          child: Text(
            'GRID',
            style: TextStyle(
              fontSize: 11,
              fontWeight: FontWeight.w800,
              letterSpacing: 0.6,
              color: on ? Colors.black : accent,
            ),
          ),
        ),
      ),
    );
  }

  Widget _gridEditorBar(int deck, Color accent) {
    Widget step(String label, VoidCallback onStep) {
      return HoldRepeatButton(
        onStep: onStep,
        child: Container(
          width: 48,
          height: 40,
          alignment: Alignment.center,
          decoration: BoxDecoration(
            color: SideDeckTheme.panelAlt,
            borderRadius: BorderRadius.circular(8),
            border: Border.all(color: accent.withValues(alpha: 0.8)),
          ),
          child: Text(
            label,
            style: const TextStyle(fontSize: 20, fontWeight: FontWeight.w800, height: 1),
          ),
        ),
      );
    }

    Widget pair(String caption, Widget left, Widget right) {
      return Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          left,
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 8),
            child: Text(
              caption,
              style: const TextStyle(fontSize: 11, color: SideDeckTheme.muted),
            ),
          ),
          right,
        ],
      );
    }

    return Material(
      color: const Color(0xE0111111),
      borderRadius: BorderRadius.circular(8),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 6),
        child: Row(
          children: [
            pair(
              'beat',
              step('«', () => _dj.shiftGridBeats(deck, -1)),
              step('»', () => _dj.shiftGridBeats(deck, 1)),
            ),
            const SizedBox(width: 16),
            pair(
              '5 ms',
              step('‹', () => _dj.shiftGridMs(deck, -5)),
              step('›', () => _dj.shiftGridMs(deck, 5)),
            ),
          ],
        ),
      ),
    );
  }

  static const _overlayPad = 4.0;
  static const _wavePadSize = 56.0;
  static const _wavePadGap = 6.0;

  Widget _waveLoop(int deck, Color accent) {
    final d = _dj.deck(deck);
    final beats = d.loopBars * 4;
    final beatsLabel = (beats - beats.round()).abs() < 0.05
        ? '${beats.round()}'
        : beats.toStringAsFixed(1);
    final canHalf = beats > DjController.loopBeats.first + 0.05;
    final canDouble = beats < DjController.loopBeats.last - 0.05;
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        HoldRepeatButton(
          enabled: canHalf,
          onStep: () => _dj.halfLoop(deck),
          child: _wavePad(accent, child: const Text('½', style: _wavePadText)),
        ),
        const SizedBox(width: _wavePadGap),
        GestureDetector(
          onTap: () => _dj.setLoop(deck, !d.loopEnabled, d.loopBars == 0 ? 1 : d.loopBars),
          child: _wavePad(
            accent,
            filled: d.loopEnabled,
            child: Column(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                Text(
                  beatsLabel,
                  style: TextStyle(
                    fontSize: 18,
                    fontWeight: FontWeight.w800,
                    height: 1,
                    color: d.loopEnabled ? Colors.black : Colors.white,
                  ),
                ),
                Text(
                  'LOOP',
                  style: TextStyle(
                    fontSize: 9,
                    fontWeight: FontWeight.w800,
                    letterSpacing: 0.6,
                    color: d.loopEnabled ? Colors.black : accent,
                  ),
                ),
              ],
            ),
          ),
        ),
        const SizedBox(width: _wavePadGap),
        HoldRepeatButton(
          enabled: canDouble,
          onStep: () => _dj.doubleLoop(deck),
          child: _wavePad(accent, child: const Text('×2', style: _wavePadText)),
        ),
      ],
    );
  }

  Widget _waveBpm(int deck, Color accent) {
    final d = _dj.deck(deck);
    final heard = d.bpm * d.rate;
    final bpmLabel = d.bpm > 1 ? heard.toStringAsFixed(1) : '—';
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        HoldRepeatButton(
          enabled: d.loaded && d.bpm > 1,
          onStep: () => _dj.nudgeBpm(deck, -0.1),
          child: _wavePad(accent, child: const Text('−', style: _wavePadText)),
        ),
        const SizedBox(width: _wavePadGap),
        GestureDetector(
          onTap: d.loaded && _dj.deck(1 - deck).loaded ? () => _dj.sync(deck) : null,
          child: _wavePad(
            accent,
            filled: d.synced,
            child: Column(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                Text(
                  bpmLabel,
                  style: TextStyle(
                    fontSize: 16,
                    fontWeight: FontWeight.w800,
                    height: 1,
                    color: d.synced ? Colors.black : Colors.white,
                    fontFeatures: const [FontFeature.tabularFigures()],
                  ),
                ),
                Text(
                  'SYNC',
                  style: TextStyle(
                    fontSize: 9,
                    fontWeight: FontWeight.w800,
                    letterSpacing: 0.6,
                    color: d.synced ? Colors.black : accent,
                  ),
                ),
              ],
            ),
          ),
        ),
        const SizedBox(width: _wavePadGap),
        HoldRepeatButton(
          enabled: d.loaded && d.bpm > 1,
          onStep: () => _dj.nudgeBpm(deck, 0.1),
          child: _wavePad(accent, child: const Text('+', style: _wavePadText)),
        ),
      ],
    );
  }

  static const _wavePadText = TextStyle(
    fontSize: 18,
    fontWeight: FontWeight.w800,
    color: Colors.white,
  );

  Widget _wavePad(Color accent, {required Widget child, bool filled = false}) {
    return SizedBox(
      width: _wavePadSize,
      height: _wavePadSize,
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: filled ? accent.withValues(alpha: 0.9) : const Color(0xCC111111),
          borderRadius: BorderRadius.circular(8),
          border: Border.all(color: accent, width: 1.6),
        ),
        child: Center(child: child),
      ),
    );
  }

  Widget _holdNudge(int deck, {required bool faster, required Color accent}) {
    final on = _dj.deck(deck).loaded;
    final color = on ? Colors.white : SideDeckTheme.muted;
    return Listener(
      onPointerDown: on ? (_) => _dj.startNudge(deck, faster: faster) : null,
      onPointerUp: on ? (_) => _dj.endNudge(deck) : null,
      onPointerCancel: on ? (_) => _dj.endNudge(deck) : null,
      child: _wavePad(
        accent,
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Icon(
              faster ? Icons.arrow_forward : Icons.arrow_back,
              size: 22,
              color: color,
            ),
            Text(
              'NUDGE',
              style: TextStyle(
                fontSize: 9,
                fontWeight: FontWeight.w800,
                letterSpacing: 0.6,
                color: on ? accent : SideDeckTheme.muted,
              ),
            ),
          ],
        ),
      ),
    );
  }
}
