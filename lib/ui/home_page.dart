import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:sidedeck/engine/music_utils.dart';
import 'package:sidedeck/engine/usb_output.dart';
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
  DjController? _dj;
  LibraryController? _library;
  String? _bootError;
  int? _gridEditDeck;
  bool _mixerDialogOpen = false;
  BuildContext? _mixerDialogContext;

  /// Only call from widgets built after a successful boot.
  DjController get _djReady => _dj!;

  @override
  void initState() {
    super.initState();
    SystemChrome.setPreferredOrientations(const [
      DeviceOrientation.landscapeLeft,
      DeviceOrientation.landscapeRight,
    ]);
    SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky);
    try {
      final dj = DjController();
      final library = LibraryController();
      dj.addListener(_onDjChanged);
      _dj = dj;
      _library = library;
    } catch (e, st) {
      _bootError = '$e\n$st';
    }
  }

  @override
  void dispose() {
    final dj = _dj;
    if (dj != null) {
      dj.removeListener(_onDjChanged);
      dj.dispose();
    }
    _library?.dispose();
    SystemChrome.setPreferredOrientations(DeviceOrientation.values);
    super.dispose();
  }

  void _onDjChanged() {
    if (!mounted) return;
    final dj = _dj;
    if (dj == null) return;
    final err = dj.engineError;
    // Only when this route is on top — Settings shows its own SnackBar.
    if (err != null && (ModalRoute.of(context)?.isCurrent ?? false)) {
      final msg = err;
      dj.clearError();
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (!mounted) return;
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text(msg)),
        );
      });
    }
    final offer = dj.mixerOffer;
    if (offer == null) {
      if (_mixerDialogOpen) _dismissMixerDialog();
      return;
    }
    if (_mixerDialogOpen) return;
    _mixerDialogOpen = true;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final next = _dj?.mixerOffer;
      if (!mounted || next == null) {
        _mixerDialogOpen = false;
        return;
      }
      _showMixerOffer(next);
    });
  }

  void _dismissMixerDialog() {
    final ctx = _mixerDialogContext;
    if (ctx == null) return;
    _mixerDialogContext = null;
    if (ctx.mounted) Navigator.of(ctx).pop();
  }

  Future<void> _showMixerOffer(UsbMixerOffer offer) async {
    final accepted = await showDialog<bool>(
      context: context,
      barrierDismissible: true,
      builder: (ctx) {
        _mixerDialogContext = ctx;
        return AlertDialog(
          backgroundColor: SideDeckTheme.panel,
          title: Text('${offer.label} detected'),
          content: const Text(
            'Enable external mixer mode? Deck A and Deck B go out as '
            'separate channels so you mix on hardware.',
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.pop(ctx, false),
              child: const Text('Not now'),
            ),
            FilledButton(
              onPressed: () => Navigator.pop(ctx, true),
              child: const Text('Enable'),
            ),
          ],
        );
      },
    );
    _mixerDialogContext = null;
    _mixerDialogOpen = false;
    if (!mounted) return;
    final dj = _dj;
    if (dj == null) return;
    if (accepted == true) {
      await dj.acceptMixerOffer();
    } else if (dj.mixerOffer != null) {
      dj.declineMixerOffer();
    }
  }

  Future<void> _openSettings() async {
    final dj = _dj;
    if (dj == null) return;
    await Navigator.of(
      context,
    ).push(MaterialPageRoute(builder: (_) => SettingsPage(dj: dj)));
  }

  Future<void> _openLibrary() async {
    final dj = _dj;
    final library = _library;
    if (dj == null || library == null) return;
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
              dj: dj,
              library: library,
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
    final bootError = _bootError;
    if (bootError != null) {
      return Scaffold(
        backgroundColor: SideDeckTheme.bg,
        body: SafeArea(
          child: SingleChildScrollView(
            padding: const EdgeInsets.all(24),
            child: SelectableText(
              'SideDeck failed to start\n\n$bootError',
              style: const TextStyle(
                color: SideDeckTheme.text,
                fontSize: 13,
                fontFamily: 'Courier',
              ),
            ),
          ),
        ),
      );
    }

    final dj = _djReady;
    final library = _library!;
    return AnimatedBuilder(
      animation: Listenable.merge([dj, library]),
      builder: (context, _) {
        return Scaffold(
          // Match mixer so L/R (and home-indicator) safe-area gutters aren't black.
          backgroundColor: SideDeckTheme.panel,
          body: SafeArea(
            child: ColoredBox(
              color: Colors.black,
              child: Column(
                children: [
                  Expanded(flex: 5, child: _waveDeck(0, SideDeckTheme.accentA)),
                  Expanded(flex: 5, child: _waveDeck(1, SideDeckTheme.accentB)),
                  if (dj.externalMixer)
                    _mixerBar()
                  else
                    Expanded(flex: 7, child: _mixerBar(expand: true)),
                ],
              ),
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
          controller: _djReady,
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

  Widget _waveDeck(int i, Color accent) {
    final d = _djReady.deck(i);
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
            final w = constraints.maxWidth;
            // Shrink pads so BPM cluster + nudges fit the side strip (no min floor
            // that would reintroduce overlap on very narrow widths).
            final side = w * (0.5 - WaveformPainter.jumpHalf);
            final padSize = ((side - pad - 3 * _wavePadGap) / 5).clamp(
              1.0,
              _wavePadSize,
            );
            final playFoot = pad + play;
            final nudgeFoot = pad + padSize * 2 + _wavePadGap;
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
                  onSeek: d.loaded ? (t) => _djReady.seek(i, t) : null,
                  onOverviewSeek: d.loaded ? (t) => _djReady.seekTo(i, t) : null,
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
                        _holdNudge(
                          i,
                          faster: false,
                          accent: accent,
                          size: padSize,
                        ),
                        SizedBox(width: _wavePadGap),
                        _holdNudge(
                          i,
                          faster: true,
                          accent: accent,
                          size: padSize,
                        ),
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
                    child: Center(
                      child: _waveLoop(i, accent, size: padSize),
                    ),
                  ),
                if (d.loaded && _gridEditDeck != i && bpmGap > 0)
                  Positioned(
                    left: WaveformPainter.jumpZoneRight(w),
                    width: bpmGap,
                    bottom: pad,
                    child: Center(
                      child: _waveBpm(i, accent, size: padSize),
                    ),
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
        onTap: () => _djReady.playPause(i),
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
            style: const TextStyle(
              fontSize: 20,
              fontWeight: FontWeight.w800,
              height: 1,
            ),
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
              step('«', () => _djReady.shiftGridBeats(deck, -1)),
              step('»', () => _djReady.shiftGridBeats(deck, 1)),
            ),
            const SizedBox(width: 16),
            pair(
              '5 ms',
              step('‹', () => _djReady.shiftGridMs(deck, -5)),
              step('›', () => _djReady.shiftGridMs(deck, 5)),
            ),
          ],
        ),
      ),
    );
  }

  static const _overlayPad = 4.0;
  static const _wavePadSize = 56.0;
  static const _wavePadGap = 6.0;

  Widget _waveLoop(int deck, Color accent, {required double size}) {
    final d = _djReady.deck(deck);
    final beats = d.loopBars * 4;
    final beatsLabel = (beats - beats.round()).abs() < 0.05
        ? '${beats.round()}'
        : beats.toStringAsFixed(1);
    final canHalf = beats > DjController.loopBeats.first + 0.05;
    final canDouble = beats < DjController.loopBeats.last - 0.05;
    final labelStyle = _wavePadLabelStyle(size);
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        HoldRepeatButton(
          enabled: canHalf,
          onStep: () => _djReady.halfLoop(deck),
          child: _wavePad(
            accent,
            size: size,
            child: Text('½', style: labelStyle),
          ),
        ),
        SizedBox(width: _wavePadGap),
        GestureDetector(
          onTap: () => _djReady.setLoop(
            deck,
            !d.loopEnabled,
            d.loopBars == 0 ? 1 : d.loopBars,
          ),
          child: _wavePad(
            accent,
            size: size,
            filled: d.loopEnabled,
            child: Column(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                FittedBox(
                  fit: BoxFit.scaleDown,
                  child: Text(
                    beatsLabel,
                    maxLines: 1,
                    softWrap: false,
                    style: TextStyle(
                      fontSize: size * 0.32,
                      fontWeight: FontWeight.w800,
                      height: 1,
                      color: d.loopEnabled ? Colors.black : Colors.white,
                    ),
                  ),
                ),
                Text(
                  'LOOP',
                  style: TextStyle(
                    fontSize: size * 0.16,
                    fontWeight: FontWeight.w800,
                    letterSpacing: 0.6,
                    color: d.loopEnabled ? Colors.black : accent,
                  ),
                ),
              ],
            ),
          ),
        ),
        SizedBox(width: _wavePadGap),
        HoldRepeatButton(
          enabled: canDouble,
          onStep: () => _djReady.doubleLoop(deck),
          child: _wavePad(
            accent,
            size: size,
            child: Text('×2', style: labelStyle),
          ),
        ),
      ],
    );
  }

  Widget _waveBpm(int deck, Color accent, {required double size}) {
    final d = _djReady.deck(deck);
    final heard = d.bpm * d.rate;
    final bpmLabel = d.bpm > 1 ? heard.toStringAsFixed(1) : '—';
    final labelStyle = _wavePadLabelStyle(size);
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        HoldRepeatButton(
          enabled: d.loaded && d.bpm > 1,
          onStep: () => _djReady.nudgeBpm(deck, -0.1),
          child: _wavePad(
            accent,
            size: size,
            child: Text('−', style: labelStyle),
          ),
        ),
        SizedBox(width: _wavePadGap),
        GestureDetector(
          onTap: d.loaded && _djReady.deck(1 - deck).loaded
              ? () => _djReady.sync(deck)
              : null,
          child: _wavePad(
            accent,
            size: size,
            filled: d.synced,
            child: Column(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                FittedBox(
                  fit: BoxFit.scaleDown,
                  child: Text(
                    bpmLabel,
                    maxLines: 1,
                    softWrap: false,
                    style: TextStyle(
                      fontSize: size * 0.26,
                      fontWeight: FontWeight.w800,
                      height: 1,
                      color: d.synced ? Colors.black : Colors.white,
                      fontFeatures: const [FontFeature.tabularFigures()],
                    ),
                  ),
                ),
                Text(
                  'SYNC',
                  style: TextStyle(
                    fontSize: size * 0.16,
                    fontWeight: FontWeight.w800,
                    letterSpacing: 0.6,
                    color: d.synced ? Colors.black : accent,
                  ),
                ),
              ],
            ),
          ),
        ),
        SizedBox(width: _wavePadGap),
        HoldRepeatButton(
          enabled: d.loaded && d.bpm > 1,
          onStep: () => _djReady.nudgeBpm(deck, 0.1),
          child: _wavePad(
            accent,
            size: size,
            child: Text('+', style: labelStyle),
          ),
        ),
      ],
    );
  }

  static TextStyle _wavePadLabelStyle(double size) => TextStyle(
    fontSize: size * 0.32,
    fontWeight: FontWeight.w800,
    color: Colors.white,
  );

  Widget _wavePad(
    Color accent, {
    required Widget child,
    required double size,
    bool filled = false,
  }) {
    return SizedBox(
      width: size,
      height: size,
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: filled
              ? accent.withValues(alpha: 0.9)
              : const Color(0xCC111111),
          borderRadius: BorderRadius.circular(8),
          border: Border.all(color: accent, width: 1.6),
        ),
        child: Center(child: child),
      ),
    );
  }

  Widget _holdNudge(
    int deck, {
    required bool faster,
    required Color accent,
    required double size,
  }) {
    final on = _djReady.deck(deck).loaded;
    final color = on ? Colors.white : SideDeckTheme.muted;
    return Listener(
      onPointerDown: on ? (_) => _djReady.startNudge(deck, faster: faster) : null,
      onPointerUp: on ? (_) => _djReady.endNudge(deck) : null,
      onPointerCancel: on ? (_) => _djReady.endNudge(deck) : null,
      child: _wavePad(
        accent,
        size: size,
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Icon(
              faster ? Icons.arrow_forward : Icons.arrow_back,
              size: size * 0.39,
              color: color,
            ),
            Text(
              'NUDGE',
              style: TextStyle(
                fontSize: size * 0.16,
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
