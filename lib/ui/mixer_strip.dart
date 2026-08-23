import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:sidedeck/state/dj_controller.dart';
import 'package:sidedeck/theme/sidedeck_theme.dart';

/// Mixer under the waveforms: taller EQ/filter faders, xfader, library/settings.
class MixerStrip extends StatelessWidget {
  const MixerStrip({
    super.key,
    required this.controller,
    required this.onLibrary,
    required this.onSettings,
  });

  final DjController controller;
  final VoidCallback onLibrary;
  final VoidCallback onSettings;

  @override
  Widget build(BuildContext context) {
    if (controller.externalMixer) {
      return ColoredBox(
        color: SideDeckTheme.panel,
        child: Padding(
          padding: const EdgeInsets.fromLTRB(6, 4, 6, 4),
          child: _externalCenter(),
        ),
      );
    }

    final a = controller.deckA;
    final b = controller.deckB;

    return ColoredBox(
      color: SideDeckTheme.panel,
      child: Padding(
        padding: const EdgeInsets.fromLTRB(4, 6, 4, 4),
        child: Row(
          children: [
            Expanded(flex: 5, child: _eqBlock(0, a, SideDeckTheme.accentA)),
            Expanded(flex: 6, child: _center()),
            Expanded(flex: 5, child: _eqBlock(1, b, SideDeckTheme.accentB)),
          ],
        ),
      ),
    );
  }

  Widget _center() {
    return Column(
      children: [
        Row(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            _iconBtn(Icons.library_music, 'Library', onLibrary),
            _iconBtn(Icons.settings_outlined, 'Settings', onSettings),
          ],
        ),
        const Text(
          'CROSSFADER',
          style: TextStyle(
            fontSize: 9,
            letterSpacing: 1.2,
            color: SideDeckTheme.muted,
          ),
        ),
        Expanded(
          child: SliderTheme(
            data: const SliderThemeData(
              trackHeight: 8,
              thumbShape: RoundSliderThumbShape(enabledThumbRadius: 11),
            ),
            child: CenterDetentSlider(
              value: controller.xfader,
              onChanged: controller.setXfader,
            ),
          ),
        ),
      ],
    );
  }

  Widget _externalCenter() {
    final name = controller.usbDeviceName.split(' - ').last.trim();
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 4),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Row(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              _iconBtn(Icons.library_music, 'Library', onLibrary),
              _iconBtn(Icons.settings_outlined, 'Settings', onSettings),
            ],
          ),
          const Text(
            'EXTERNAL MIXER',
            textAlign: TextAlign.center,
            style: TextStyle(
              fontSize: 9,
              letterSpacing: 1.0,
              color: SideDeckTheme.muted,
            ),
          ),
          if (name.isNotEmpty)
            Text(
              name,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              textAlign: TextAlign.center,
              style: const TextStyle(fontSize: 10, color: SideDeckTheme.text),
            ),
        ],
      ),
    );
  }

  Widget _iconBtn(IconData icon, String tooltip, VoidCallback onTap) {
    return IconButton(
      tooltip: tooltip,
      visualDensity: VisualDensity.compact,
      padding: EdgeInsets.zero,
      constraints: const BoxConstraints(minWidth: 36, minHeight: 32),
      onPressed: onTap,
      icon: Icon(icon, size: 22),
    );
  }

  Widget _eqBlock(int deck, DeckState d, Color accent) {
    return Row(
      children: [
        Expanded(
          child: _vSlider('LOW', d.eqLow, -12, 12, accent, (v) => controller.setEq(deck, low: v)),
        ),
        Expanded(
          child: _vSlider('MID', d.eqMid, -12, 12, accent, (v) => controller.setEq(deck, mid: v)),
        ),
        Expanded(
          child: _vSlider('HIGH', d.eqHigh, -12, 12, accent, (v) => controller.setEq(deck, high: v)),
        ),
        Expanded(
          child: _vSlider('FILT', d.filter, -1, 1, accent, (v) => controller.setFilter(deck, v)),
        ),
      ],
    );
  }

  Widget _vSlider(
    String label,
    double value,
    double min,
    double max,
    Color accent,
    ValueChanged<double> onChanged,
  ) {
    return Column(
      children: [
        Expanded(
          child: RotatedBox(
            quarterTurns: 3,
            child: SliderTheme(
              data: SliderThemeData(
                trackHeight: 5,
                thumbColor: accent,
                activeTrackColor: accent,
                inactiveTrackColor: SideDeckTheme.panelAlt,
                thumbShape: const RoundSliderThumbShape(enabledThumbRadius: 9),
                overlayShape: SliderComponentShape.noOverlay,
              ),
              child: CenterDetentSlider(
                value: value.clamp(min, max),
                min: min,
                max: max,
                onChanged: onChanged,
              ),
            ),
          ),
        ),
        Text(
          label,
          style: const TextStyle(fontSize: 9, color: SideDeckTheme.muted, letterSpacing: 0.3),
        ),
      ],
    );
  }
}

/// Follows the finger. A center notch only appears after you leave the
/// middle and come back in the same drag.
class CenterDetentSlider extends StatefulWidget {
  const CenterDetentSlider({
    super.key,
    required this.value,
    required this.onChanged,
    this.min = 0,
    this.max = 1,
  });

  final double value;
  final double min;
  final double max;
  final ValueChanged<double> onChanged;

  @override
  State<CenterDetentSlider> createState() => _CenterDetentSliderState();
}

class _CenterDetentSliderState extends State<CenterDetentSlider> {
  static const _snapIn = 0.02;
  static const _breakOut = 0.11;

  bool _latched = false;
  bool _leftCenter = false;

  double get _mid => (widget.min + widget.max) / 2;
  double get _range => widget.max - widget.min;

  bool _near(double v, double threshold) => (v - _mid).abs() <= threshold * _range;

  void _onChangeStart(double v) {
    _latched = false;
    _leftCenter = !_near(v, _snapIn);
  }

  void _onChanged(double v) {
    final dist = (v - _mid).abs();

    if (_latched) {
      if (dist >= _breakOut * _range) {
        _latched = false;
        HapticFeedback.lightImpact();
        widget.onChanged(v);
      } else if (widget.value != _mid) {
        widget.onChanged(_mid);
      }
      return;
    }

    if (!_near(v, _snapIn)) {
      _leftCenter = true;
      widget.onChanged(v);
      return;
    }

    if (_leftCenter) {
      _latched = true;
      HapticFeedback.selectionClick();
      widget.onChanged(_mid);
    } else {
      widget.onChanged(v);
    }
  }

  void _onChangeEnd(double v) {
    _latched = false;
    _leftCenter = false;
  }

  @override
  Widget build(BuildContext context) {
    return Slider(
      value: widget.value.clamp(widget.min, widget.max),
      min: widget.min,
      max: widget.max,
      onChangeStart: _onChangeStart,
      onChanged: _onChanged,
      onChangeEnd: _onChangeEnd,
    );
  }
}

/// Tap once, or hold to repeat after a short delay.
class HoldRepeatButton extends StatefulWidget {
  const HoldRepeatButton({
    super.key,
    required this.onStep,
    required this.child,
    this.enabled = true,
    this.delay = const Duration(milliseconds: 380),
    this.interval = const Duration(milliseconds: 70),
  });

  final VoidCallback onStep;
  final Widget child;
  final bool enabled;
  final Duration delay;
  final Duration interval;

  @override
  State<HoldRepeatButton> createState() => _HoldRepeatButtonState();
}

class _HoldRepeatButtonState extends State<HoldRepeatButton> {
  Timer? _delay;
  Timer? _repeat;

  void _start() {
    if (!widget.enabled) return;
    HapticFeedback.selectionClick();
    widget.onStep();
    _delay = Timer(widget.delay, () {
      _repeat = Timer.periodic(widget.interval, (_) => widget.onStep());
    });
  }

  void _stop() {
    _delay?.cancel();
    _repeat?.cancel();
    _delay = null;
    _repeat = null;
  }

  @override
  void didUpdateWidget(HoldRepeatButton oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (!widget.enabled) _stop();
  }

  @override
  void dispose() {
    _stop();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Listener(
      onPointerDown: widget.enabled ? (_) => _start() : null,
      onPointerUp: (_) => _stop(),
      onPointerCancel: (_) => _stop(),
      child: widget.child,
    );
  }
}
