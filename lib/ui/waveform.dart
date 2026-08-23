import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter/material.dart';

/// Horizontal scrolling DJ waveform: playhead pinned center, regular beat grid.
class WaveformPainter extends CustomPainter {
  WaveformPainter({
    required this.min,
    required this.max,
    required this.position,
    required this.duration,
    required this.beatOffset,
    required this.bpm,
    required this.color,
    this.loopEnabled = false,
    this.loopStart = 0,
    this.loopEnd = 0,
    this.zoomWindowSec = 8,
    this.showJumpZone = false,
  });

  final Float32List min;
  final Float32List max;
  final double position;
  final double duration;
  final double beatOffset;
  final double bpm;
  final Color color;
  final bool loopEnabled;
  final double loopStart;
  final double loopEnd;
  final double zoomWindowSec;
  final bool showJumpZone;

  @override
  void paint(Canvas canvas, Size size) {
    canvas.drawRect(Offset.zero & size, Paint()..color = const Color(0xFF0A0C10));

    if (min.isEmpty || duration <= 0 || size.width <= 1 || size.height <= 1) {
      return;
    }

    var peak = 0.001;
    final n = math.min(min.length, max.length);
    for (var i = 0; i < n; i++) {
      peak = math.max(peak, math.max(min[i].abs(), max[i].abs()));
    }

    const overviewH = WaveformPainter.overviewH;
    final waveTop = overviewH;
    final waveH = size.height - overviewH;
    final mid = waveTop + waveH / 2;

    final halfWin = zoomWindowSec / 2;
    // Always-centered playhead (empty pad before 0 / after end).
    final viewStart = position - halfWin;
    final viewEnd = position + halfWin;
    final viewDur = zoomWindowSec;

    double xForTime(double t) => ((t - viewStart) / viewDur) * size.width;

    for (var px = 0.0; px < size.width; px++) {
      final t0 = (px / size.width) * duration;
      final t1 = ((px + 1) / size.width) * duration;
      final amp = _peakInRange(t0, t1, n, duration, peak);
      final h = amp * (overviewH - 2);
      final y0 = (overviewH - h) / 2;
      canvas.drawRect(
        Rect.fromLTWH(px, y0, 1.2, h.clamp(1, overviewH)),
        Paint()..color = _ampColor(color, amp).withValues(alpha: 0.95),
      );
    }
    final winL = (viewStart / duration) * size.width;
    final winR = (viewEnd / duration) * size.width;
    canvas.drawRect(
      Rect.fromLTRB(winL.clamp(0, size.width), 0, winR.clamp(0, size.width), overviewH),
      Paint()..color = Colors.white.withValues(alpha: 0.12),
    );
    _drawPlayhead(
      canvas,
      x: (position / duration) * size.width,
      y0: 0,
      y1: overviewH,
      coreWidth: 2.4,
      tick: true,
    );
    _drawLoopOverview(canvas, size, overviewH);

    for (var px = 0.0; px < size.width; px++) {
      final t0 = viewStart + (px / size.width) * viewDur;
      final t1 = viewStart + ((px + 1) / size.width) * viewDur;
      if (t1 <= 0 || t0 >= duration) continue;
      final loHi = _minMaxInRange(t0.clamp(0.0, duration), t1.clamp(0.0, duration), n, duration);
      final amp = math.max(loHi.$1.abs(), loHi.$2.abs()) / peak;
      final yHi = mid - (loHi.$2.abs() / peak) * (waveH * 0.48);
      final yLo = mid + (loHi.$1.abs() / peak) * (waveH * 0.48);
      final h = (yLo - yHi).clamp(1.0, waveH);
      canvas.drawRect(
        Rect.fromLTWH(px, yHi, 1.05, h),
        Paint()..color = _ampColor(color, amp),
      );
    }

    canvas.drawLine(
      Offset(0, mid),
      Offset(size.width, mid),
      Paint()
        ..color = Colors.white.withValues(alpha: 0.12)
        ..strokeWidth = 1,
    );

    // Grid on top of the wave so it stays visible.
    final beatSec = bpm > 1 ? 60.0 / bpm : 0.0;
    if (beatSec > 0.05) {
      var t = beatOffset;
      if (t > viewStart) {
        t -= (((t - viewStart) / beatSec).ceil() * beatSec);
      }
      while (t < viewStart) {
        t += beatSec;
      }
      for (; t <= viewEnd; t += beatSec) {
        if (t < 0 || t > duration) continue;
        final idx = ((t - beatOffset) / beatSec).round();
        final phrase = idx % 32 == 0; // 8 bars
        final bar = idx % 4 == 0;
        final paint = Paint()
          ..strokeWidth = phrase ? 2.0 : (bar ? 1.4 : 1.0)
          ..color = phrase
              ? const Color(0xFFFFF3B0).withValues(alpha: 0.72)
              : Colors.white.withValues(alpha: bar ? 0.42 : 0.20);
        canvas.drawLine(Offset(xForTime(t), waveTop), Offset(xForTime(t), size.height), paint);
      }
    }

    _drawLoopZoomed(canvas, size, waveTop, xForTime);

    if (showJumpZone) {
      _drawJumpZone(canvas, size, waveTop);
    }

    _drawPlayhead(
      canvas,
      x: size.width / 2,
      y0: waveTop,
      y1: size.height,
      coreWidth: 2.6,
      tick: false,
    );
  }

  static const overviewH = 16.0;
  static const _playhead = Color(0xFFF4F7FB);

  void _drawPlayhead(
    Canvas canvas, {
    required double x,
    required double y0,
    required double y1,
    required double coreWidth,
    required bool tick,
  }) {
    final halo = Paint()
      ..color = const Color(0xE6000000)
      ..strokeWidth = coreWidth + 2.4
      ..strokeCap = StrokeCap.square;
    final core = Paint()
      ..color = _playhead
      ..strokeWidth = coreWidth
      ..strokeCap = StrokeCap.square;
    canvas.drawLine(Offset(x, y0), Offset(x, y1), halo);
    canvas.drawLine(Offset(x, y0), Offset(x, y1), core);
    if (!tick) {
      return;
    }
    final tri = Path()
      ..moveTo(x, y1)
      ..lineTo(x - 5.5, y0)
      ..lineTo(x + 5.5, y0)
      ..close();
    canvas.drawPath(tri, Paint()..color = const Color(0xE6000000));
    final inner = Path()
      ..moveTo(x, y1 - 1)
      ..lineTo(x - 4, y0 + 1)
      ..lineTo(x + 4, y0 + 1)
      ..close();
    canvas.drawPath(inner, Paint()..color = _playhead);
  }

  static const jumpHalf = 0.14;

  /// Left edge of the beat-jump hit rect; same math as [_drawJumpZone].
  static double jumpZoneLeft(double width) => width * (0.5 - jumpHalf);

  /// Right edge of the beat-jump hit rect.
  static double jumpZoneRight(double width) => width * (0.5 + jumpHalf);
  static const _loopColor = Color(0xFF1DE9B6);

  void _drawJumpZone(Canvas canvas, Size size, double waveTop) {
    final cx = size.width / 2;
    final half = size.width * jumpHalf;
    final left = cx - half;
    final right = cx + half;
    canvas.drawRect(
      Rect.fromLTRB(left, waveTop, right, size.height),
      Paint()..color = Colors.white.withValues(alpha: 0.06),
    );
    final edge = Paint()
      ..color = Colors.white.withValues(alpha: 0.22)
      ..strokeWidth = 1;
    canvas.drawLine(Offset(left, waveTop), Offset(left, size.height), edge);
    canvas.drawLine(Offset(right, waveTop), Offset(right, size.height), edge);

    final cy = waveTop + (size.height - waveTop) / 2;
    _chevron(canvas, Offset(cx - half * 0.55, cy), left: true);
    _chevron(canvas, Offset(cx + half * 0.55, cy), left: false);
    _jumpLabel(canvas, Offset(cx - half * 0.55 + 10, cy - 7), '4');
    _jumpLabel(canvas, Offset(cx + half * 0.55 - 18, cy - 7), '4');
  }

  void _chevron(Canvas canvas, Offset c, {required bool left}) {
    final paint = Paint()
      ..color = Colors.white.withValues(alpha: 0.55)
      ..style = PaintingStyle.stroke
      ..strokeWidth = 2
      ..strokeJoin = StrokeJoin.round;
    final dx = left ? -6.0 : 6.0;
    final path = Path()
      ..moveTo(c.dx - dx, c.dy - 8)
      ..lineTo(c.dx + dx, c.dy)
      ..lineTo(c.dx - dx, c.dy + 8);
    canvas.drawPath(path, paint);
  }

  void _jumpLabel(Canvas canvas, Offset o, String s) {
    final tp = TextPainter(
      text: TextSpan(
        text: s,
        style: TextStyle(
          color: Colors.white.withValues(alpha: 0.5),
          fontSize: 10,
          fontWeight: FontWeight.w800,
        ),
      ),
      textDirection: TextDirection.ltr,
    )..layout();
    tp.paint(canvas, o);
  }

  void _drawLoopOverview(Canvas canvas, Size size, double overviewH) {
    if (!loopEnabled || loopEnd <= loopStart || duration <= 0) return;
    final x0 = ((loopStart / duration) * size.width).clamp(0.0, size.width);
    final x1 = ((loopEnd / duration) * size.width).clamp(0.0, size.width);
    if (x1 <= x0) return;
    canvas.drawRect(
      Rect.fromLTRB(x0, 0, x1, overviewH),
      Paint()..color = _loopColor.withValues(alpha: 0.45),
    );
  }

  void _drawLoopZoomed(
    Canvas canvas,
    Size size,
    double waveTop,
    double Function(double) xForTime,
  ) {
    if (!loopEnabled || loopEnd <= loopStart) return;
    final x0 = xForTime(loopStart);
    final x1 = xForTime(loopEnd);
    if (x1 < 0 || x0 > size.width) return;
    final left = x0.clamp(0.0, size.width);
    final right = x1.clamp(0.0, size.width);
    canvas.drawRect(
      Rect.fromLTRB(left, waveTop, right, size.height),
      Paint()..color = _loopColor.withValues(alpha: 0.22),
    );
    final edge = Paint()
      ..color = _loopColor
      ..strokeWidth = 2;
    if (x0 >= 0 && x0 <= size.width) {
      canvas.drawLine(Offset(x0, waveTop), Offset(x0, size.height), edge);
    }
    if (x1 >= 0 && x1 <= size.width) {
      canvas.drawLine(Offset(x1, waveTop), Offset(x1, size.height), edge);
    }
  }

  (double, double) _minMaxInRange(double t0, double t1, int n, double duration) {
    var i0 = ((t0 / duration) * n).floor().clamp(0, n - 1);
    var i1 = ((t1 / duration) * n).ceil().clamp(0, n - 1);
    if (i1 < i0) i1 = i0;
    var lo = 0.0;
    var hi = 0.0;
    for (var i = i0; i <= i1; i++) {
      lo = math.min(lo, min[i]);
      hi = math.max(hi, max[i]);
    }
    return (lo, hi);
  }

  double _peakInRange(double t0, double t1, int n, double duration, double peak) {
    final mm = _minMaxInRange(t0, t1, n, duration);
    return math.max(mm.$1.abs(), mm.$2.abs()) / peak;
  }

  Color _ampColor(Color base, double amp) {
    final a = amp.clamp(0.0, 1.0);
    if (a < 0.25) {
      return Color.lerp(base.withValues(alpha: 0.35), base, a / 0.25)!;
    }
    if (a < 0.6) {
      return Color.lerp(base, const Color(0xFFE040FB), (a - 0.25) / 0.35)!;
    }
    if (a < 0.85) {
      return Color.lerp(const Color(0xFFE040FB), const Color(0xFFFF6B35), (a - 0.6) / 0.25)!;
    }
    return Color.lerp(const Color(0xFFFF6B35), const Color(0xFFFFF3B0), (a - 0.85) / 0.15)!;
  }

  @override
  bool shouldRepaint(covariant WaveformPainter oldDelegate) {
    return oldDelegate.position != position ||
        oldDelegate.duration != duration ||
        oldDelegate.min != min ||
        oldDelegate.max != max ||
        oldDelegate.beatOffset != beatOffset ||
        oldDelegate.bpm != bpm ||
        oldDelegate.loopEnabled != loopEnabled ||
        oldDelegate.loopStart != loopStart ||
        oldDelegate.loopEnd != loopEnd ||
        oldDelegate.showJumpZone != showJumpZone;
  }
}

class WaveformView extends StatelessWidget {
  const WaveformView({
    super.key,
    required this.min,
    required this.max,
    required this.position,
    required this.duration,
    required this.color,
    this.beatOffset = 0,
    this.bpm = 0,
    this.loopEnabled = false,
    this.loopStart = 0,
    this.loopEnd = 0,
    this.onSeek,
  });

  final Float32List min;
  final Float32List max;
  final double position;
  final double duration;
  final double beatOffset;
  final double bpm;
  final Color color;
  final bool loopEnabled;
  final double loopStart;
  final double loopEnd;
  final ValueChanged<double>? onSeek;

  static const zoom = 8.0;

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        final w = constraints.maxWidth.isFinite ? constraints.maxWidth : 300.0;
        final h = constraints.maxHeight.isFinite && constraints.maxHeight > 0
            ? constraints.maxHeight
            : 88.0;
        final paint = CustomPaint(
          painter: WaveformPainter(
            min: min,
            max: max,
            position: position,
            duration: duration,
            beatOffset: beatOffset,
            bpm: bpm,
            color: color,
            loopEnabled: loopEnabled,
            loopStart: loopStart,
            loopEnd: loopEnd,
            zoomWindowSec: zoom,
            showJumpZone: onSeek != null,
          ),
          size: Size(w, h),
        );
        if (onSeek == null || duration <= 0) return paint;
        return GestureDetector(
          onTapDown: (d) {
            if (d.localPosition.dy < WaveformPainter.overviewH) return;
            final nx = (d.localPosition.dx / w).clamp(0.0, 1.0);
            if ((nx - 0.5).abs() > WaveformPainter.jumpHalf) return;
            final t = position - zoom / 2 + nx * zoom;
            onSeek!(t.clamp(0.0, duration).toDouble());
          },
          child: paint,
        );
      },
    );
  }
}
