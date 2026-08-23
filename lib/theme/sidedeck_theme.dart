import 'package:flutter/material.dart';

class SideDeckTheme {
  static const bg = Color(0xFF0E1116);
  static const panel = Color(0xFF171B22);
  static const panelAlt = Color(0xFF1E2430);
  static const accentA = Color(0xFF4CC9F0);
  static const accentB = Color(0xFFF72585);
  static const text = Color(0xFFE8ECF1);
  static const muted = Color(0xFF8B93A7);

  static ThemeData dark() {
    final base = ThemeData(
      brightness: Brightness.dark,
      useMaterial3: true,
      colorScheme: ColorScheme.fromSeed(
        seedColor: accentA,
        brightness: Brightness.dark,
      ),
    );
    return base.copyWith(
      scaffoldBackgroundColor: bg,
      textTheme: base.textTheme.apply(bodyColor: text, displayColor: text),
    );
  }
}
