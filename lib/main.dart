import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:sidedeck/theme/sidedeck_theme.dart';
import 'package:sidedeck/ui/home_page.dart';

/// Last Flutter framework error — shown if the normal UI never mounts.
String? bootFatalError;

void main() {
  WidgetsFlutterBinding.ensureInitialized();

  // Release/TestFlight: surface errors instead of a blank grey screen.
  ErrorWidget.builder = (details) {
    return Material(
      color: SideDeckTheme.bg,
      child: SafeArea(
        child: SingleChildScrollView(
          padding: const EdgeInsets.all(24),
          child: Text(
            'UI error\n\n${details.exceptionAsString()}\n\n${details.stack}',
            style: const TextStyle(
              color: SideDeckTheme.text,
              fontSize: 13,
              fontFamily: 'Courier',
            ),
          ),
        ),
      ),
    );
  };

  FlutterError.onError = (details) {
    FlutterError.presentError(details);
    bootFatalError = '${details.exceptionAsString()}\n${details.stack}';
  };

  PlatformDispatcher.instance.onError = (error, stack) {
    bootFatalError = '$error\n$stack';
    return true;
  };

  runZonedGuarded(
    () => runApp(const SideDeckApp()),
    (error, stack) {
      bootFatalError = '$error\n$stack';
      runApp(_FatalApp(message: bootFatalError!));
    },
  );
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

class _FatalApp extends StatelessWidget {
  const _FatalApp({required this.message});

  final String message;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: SideDeckTheme.dark(),
      home: Scaffold(
        backgroundColor: SideDeckTheme.bg,
        body: SafeArea(
          child: SingleChildScrollView(
            padding: const EdgeInsets.all(24),
            child: Text(
              'SideDeck failed to start\n\n$message',
              style: const TextStyle(
                color: SideDeckTheme.text,
                fontSize: 13,
                fontFamily: 'Courier',
              ),
            ),
          ),
        ),
      ),
    );
  }
}
