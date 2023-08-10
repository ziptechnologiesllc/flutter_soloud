import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_soloud/flutter_soloud.dart';

void main() async {
  WidgetsFlutterBinding.ensureInitialized();

  await runZonedGuarded(
    () async => loadAndPlay(),
    (error, stack) {
      debugPrint('TEST error: $error\nstack: $stack');
      exit(1);
    },
  );

  await runZonedGuarded(
    () async => playTest(),
    (error, stack) {
      debugPrint('TEST error: $error\nstack: $stack');
      exit(1);
    },
  );
  
  debugPrint('TEST passed!');
  exit(0);
}

String output = '';
SoundProps? currentSound;

Future<void> playTest() async {
  /// Start audio isolate
  await startIsolate();

  /// Load sample
  await loadAsset();

  /// pause, seek test
  {
    await SoLoud().play(currentSound!);
    final ret1 = SoLoud().getLength(currentSound!);
    assert(
      ret1.error == PlayerErrors.noError &&
          (ret1.length * 100).ceilToDouble().toInt() == 384,
      'pauseSwitch() failed!',
    );
    await Future.delayed(const Duration(milliseconds: 1000), () {});
    var ret2 = SoLoud().pauseSwitch(currentSound!.handle.first);
    assert(
      ret2 == PlayerErrors.noError,
      'pauseSwitch() failed!',
    );
    final ret3 = SoLoud().getPause(currentSound!.handle.first);
    assert(
      ret3.error == PlayerErrors.noError && ret3.pause,
      'play() failed!',
    );

    /// seek
    ret2 = SoLoud().seek(currentSound!.handle.first, 2);
    assert(
      ret2 == PlayerErrors.noError,
      'seek() failed!',
    );
    final ret4 = SoLoud().getPosition(currentSound!.handle.first);
    assert(
      ret4.error == PlayerErrors.noError && ret4.position == 2,
      'getPosition() failed!',
    );
  }

  await stopIsolate();
}

/// Test start/stop isolate, load, play and events from sound
///
Future<void> loadAndPlay() async {
  /// Start audio isolate
  await startIsolate();

  /// Load sample
  await loadAsset();

  /// Play sample
  {
    final ret = await SoLoud().play(currentSound!);
    assert(
      ret.error == PlayerErrors.noError &&
          currentSound!.soundHash > 0 &&
          currentSound!.handle.length == 1,
      'play() failed!',
    );

    /// Wait for the sample to finish and see in log:
    /// "@@@@@@@@@@@ SOUND EVENT: SoundEvent.soundDisposed .*"
    /// 3798ms explosion.mp3 sample duration
    await Future.delayed(const Duration(milliseconds: 4500), () {});
    assert(
      output == 'SoundEvent.handleIsNoMoreValid',
      'Sound end playback event not triggered!',
    );
  }

  /// Play 4 sample
  {
    await SoLoud().play(currentSound!);
    await SoLoud().play(currentSound!);
    await SoLoud().play(currentSound!);
    await SoLoud().play(currentSound!);
    assert(
      currentSound!.handle.length == 4,
      'loadFromAssets() failed!',
    );

    /// Wait for the sample to finish and see in log:
    /// "@@@@@@@@@@@ SOUND EVENT: SoundEvent.handleIsNoMoreValid .* has [3-2-1-0] active handles"
    /// 3798ms explosion.mp3 sample duration
    await Future.delayed(const Duration(milliseconds: 4500), () {});
    assert(
      currentSound!.handle.isEmpty,
      'Play 4 sample handles failed!',
    );
  }

  /// Stop isolate
  {
    /// Stop player and see in log:
    /// "@@@@@@@@@@@ SOUND EVENT: SoundEvent.soundDisposed .*"
    await stopIsolate();
    assert(
      output == 'SoundEvent.soundDisposed',
      'Sound end playback event not triggered!',
    );
  }
}

/// Common methods
Future<void> startIsolate() async {
  final ret = await SoLoud().startIsolate();
  assert(ret == PlayerErrors.noError, 'startIsolate() failed!');
}

Future<void> stopIsolate() async {
  final ret = await SoLoud().stopIsolate();
  assert(ret, 'stopIsolate() failed!');
}

Future<void> loadAsset() async {
  currentSound = await SoloudTools.loadFromAssets(
    'assets/audio/explosion.mp3',
  );
  assert(currentSound != null, 'loadFromAssets() failed!');

  currentSound!.soundEvents.stream.listen((event) {
    if (event.event == SoundEvent.handleIsNoMoreValid) {
      output = 'SoundEvent.handleIsNoMoreValid';
    }
    if (event.event == SoundEvent.soundDisposed) {
      output = 'SoundEvent.soundDisposed';
    }
  });
}