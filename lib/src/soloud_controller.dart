import 'dart:ffi' as ffi;
import 'dart:io';

import 'package:flutter_soloud/src/bindings_capture_ffi.dart';

/// Controller that expose method channel and FFI
class SoLoudController {
  ///
  factory SoLoudController() => _instance ??= SoLoudController._();

  SoLoudController._() {
    initialize();
  }

  static SoLoudController? _instance;

  ///
  late ffi.DynamicLibrary nativeLib;

  ///
  late final FlutterCaptureFfi captureFFI;

  bool _isInitialized = false;

  /// Returns true if initialization was successful
  bool get isInitialized => _isInitialized;

  ///
  void initialize() {
    try {
      if (Platform.isMacOS) {
        print('Using process library for macOS');
        nativeLib = ffi.DynamicLibrary.process();
      } else {
        nativeLib = Platform.isLinux
            ? ffi.DynamicLibrary.open('libflutter_soloud.so')
            : (Platform.isAndroid
                ? ffi.DynamicLibrary.open('libflutter_soloud.so')
                : (Platform.isWindows
                    ? ffi.DynamicLibrary.open('flutter_soloud.dll')
                    : ffi.DynamicLibrary.process()));
      }

      print('Initializing SoLoud with library: ${nativeLib.toString()}');

      try {
        captureFFI = FlutterCaptureFfi.fromLookup(nativeLib.lookup);
        print('Successfully created FlutterCaptureFfi instance');

        // Test a simple FFI call
        final devices = captureFFI.listCaptureDevices();
        print('Found ${devices.length} capture devices');

        _isInitialized = true;
        print('SoLoud capture FFI initialized successfully');
      } catch (e) {
        print('Error during FFI initialization: $e');
        rethrow;
      }
    } catch (e, stackTrace) {
      print('Error initializing SoLoud: $e');
      print('Stack trace: $stackTrace');
      _isInitialized = false;
      rethrow;
    }
  }

  /// Ensures the controller is initialized before use
  void ensureInitialized() {
    if (!_isInitialized) {
      throw StateError('SoLoudController not properly initialized');
    }
  }
}
