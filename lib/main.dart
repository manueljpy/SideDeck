import 'package:flutter/material.dart';
import 'package:sidedeck/theme/sidedeck_theme.dart';
import 'package:sidedeck/ui/home_page.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const SideDeckApp());
}

class SideDeckApp extends StatelessWidget {
  const SideDeckApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'SideDeck',
      debugShowCheckedModeBanner: false,
      theme: SideDeckTheme.dark(),
      home: const HomePage(),
    );
  }
}
