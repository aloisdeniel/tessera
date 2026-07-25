// smoke_test.dart — boots the app, opens a game, and lets the engine attach.
//
// This exercises the full native path (engine create → shader/pipeline load →
// metal view attach → first frames): a stale shader bundle or linkage problem
// surfaces here as a PlatformException from TesseraController.attach.
//
// Run on a simulator/device: flutter test integration_test -d <device>
import 'package:flutter/material.dart';
import 'package:flutter_tessera/flutter_tessera.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

import 'package:flutter_tessera_example/main.dart' as app;

Future<void> _pumpFor(WidgetTester tester, Duration total) async {
  final step = const Duration(milliseconds: 250);
  var elapsed = Duration.zero;
  while (elapsed < total) {
    await tester.pump(step);
    elapsed += step;
  }
}

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('menu shows and Chess boots the engine', (tester) async {
    app.main();
    await tester.pumpAndSettle();
    expect(find.text('Tessera Games'), findsOneWidget);

    await tester.tap(find.textContaining('Chess').first);
    await tester.pump();

    // Give the platform view time to create the engine, load shaders, attach,
    // and render its first frames. attach failures are unhandled async
    // PlatformExceptions and fail the test.
    await _pumpFor(tester, const Duration(seconds: 6));
    expect(find.byType(TesseraView), findsOneWidget);
  });
}
