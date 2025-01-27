import 'dart:async';
import 'dart:ffi' as ffi;

import 'package:flutter_soloud/src/enums.dart';
import 'package:flutter_soloud/src/soloud.dart';
import 'package:flutter_soloud/src/soloud_controller.dart';
import 'package:logging/logging.dart';
import 'package:meta/meta.dart';

/// Use this class to _capture_ audio (such as from a microphone).
///
/// This class is completely independent from [SoLoud]. You can
/// initialize and shut it down regardless of the state of [SoLoud].
///
/// Please note that this class does _not_ currently provide
/// recording capabilities. It allows you to provide stats and visualizations
/// of the audio data coming from a capture device (such as microphone)
/// but it currently does not allow you to save that data to a file.
///
/// This class is marked as [experimental] and therefore may have
/// breaking changes in the future without a major version bump.
@experimental
interface class SoLoudCapture {
  /// A deprecated way to access the singleton [instance] of [SoLoudCapture].
  ///
  /// The reason this is deprecated is that it leads to code that misrepresents
  /// what is actually happening. For example:
  ///
  /// ```dart
  /// // BAD
  /// var sound = await SoLoudCapture().loadFile('path/to/sound.mp3');
  /// await SoLoudCapture().play(sound)
  /// ```
  ///
  /// The code above suggests, on the face of it, that we're _constructing_
  /// two instances of [SoLoudCapture], although we're in fact only accessing
  /// the singleton instance.
  @Deprecated('Use SoLoudCapture.instance instead')
  factory SoLoudCapture() => instance;

  /// The private constructor of [SoLoudCapture]. This prevents developers from
  /// instantiating new instances.
  SoLoudCapture._();

  static final Logger _log = Logger('flutter_soloud.SoLoudCapture');

  /// The singleton instance of [SoLoudCapture]. Only one SoLoud instance
  /// can exist in C++ land, so – for consistency and to avoid confusion
  /// – only one instance can exist in Dart land.
  ///
  /// Using this static field, you can get a hold of the single instance
  /// of this class from anywhere. This ability to access global state
  /// from anywhere can lead to hard-to-debug bugs, though, so it is
  /// preferable to encapsulate this and provide it through a facade.
  /// For example:
  ///
  /// ```dart
  /// final recordingController = MyRecordingController(SoLoudCapture.instance);
  ///
  /// // Now provide the recording controller to parts of the app that need it.
  /// // No other part of the codebase need import `package:flutter_soloud`.
  /// ```
  ///
  /// Alternatively, at least create a field with the single instance
  /// of [SoLoudCapture], and provide that (without the facade, but also without
  /// accessing [SoLoudCapture.instance] from different places of the app).
  /// For example:
  ///
  /// ```dart
  /// class _MyWidgetState extends State<MyWidget> {
  ///   SoLoud? _soloud;
  ///
  ///   void _initializeRecording() async {
  ///     // The only place in the codebase that accesses SoLoudCapture.instance
  ///     // directly.
  ///     final soloud = SoLoudCapture.instance;
  ///     await soloud.initialize();
  ///
  ///     setState(() {
  ///       _soloud = soloud;
  ///     });
  ///   }
  ///
  ///   // ...
  /// }
  /// ```
  static final SoLoudCapture instance = SoLoudCapture._();

  /// status of capture
  bool isCaptureInited = false;

  /// Stream of audio events. Deprecated.
  @Deprecated(
    'Instead of listening to events, just await initialize() and shutdown()',
  )
  StreamController<AudioEvent> get audioEvent => SoLoud.instance.audioEvent;

  // ////////////////////////////////////////////////
  // Below all the methods implemented with FFI for the capture
  // ////////////////////////////////////////////////

  /// Return a floats matrix of 256x512
  /// Every row are composed of 256 FFT values plus 256 of wave data
  /// Every time is called, a new row is stored in the
  /// first row and all the previous rows are shifted
  /// up (the last one will be lost).
  ///
  /// Return [CaptureErrors.captureNoError] if no error.
  ///
  CaptureErrors getCaptureAudioTexture2D(
    ffi.Pointer<ffi.Pointer<ffi.Float>> audioData,
  ) {
    if (!isCaptureInited || audioData == ffi.nullptr) {
      _log.severe(
        () => 'getCaptureAudioTexture2D(): ${CaptureErrors.captureNotInited}',
      );
      return CaptureErrors.captureNotInited;
    }

    final ret =
        SoLoudController().captureFFI.getCaptureAudioTexture2D(audioData);
    _logCaptureError(ret, from: 'getCaptureAudioTexture2D() result');

    if (ret != CaptureErrors.captureNoError) {
      return ret;
    }
    if (audioData.value == ffi.nullptr) {
      _logCaptureError(
        CaptureErrors.nullPointer,
        from: 'getCaptureAudioTexture2D() result',
      );
      return CaptureErrors.nullPointer;
    }
    return CaptureErrors.captureNoError;
  }

  /// Deprecated alias of [initialize].
  @Deprecated('Use initialize() instead')
  CaptureErrors initCapture({int deviceID = -1, required ffi.Pointer<ffi.Float> buffer, required ffi.Pointer<ffi.Int> recordHead}) =>
      initialize(deviceID: deviceID, buffer: buffer, recordHead: recordHead);

  /// Initialize input device with [deviceID].
  ///
  /// Return [CaptureErrors.captureNoError] if no error.
  ///
  CaptureErrors initialize({int deviceID = -1, required ffi.Pointer<ffi.Float> buffer, required ffi.Pointer<ffi.Int> recordHead}) {
    final ret = SoLoudController().captureFFI.initCapture(deviceID, buffer, recordHead);
    _logCaptureError(ret, from: 'initCapture() result');
    if (ret == CaptureErrors.captureNoError) {
      isCaptureInited = true;
    }

    return ret;
  }

  /// Get the status of the device.
  ///
  bool isCaptureInitialized() {
    return SoLoudController().captureFFI.isCaptureInited();
  }

  /// Returns true if the device is capturing audio.
  ///
  bool isCaptureStarted() {
    return SoLoudController().captureFFI.isCaptureStarted();
  }

  /// List available input devices. Useful on desktop to choose
  /// which input device to use.
  ///
  List<CaptureDevice> listCaptureDevices() {
    return SoLoudController().captureFFI.listCaptureDevices();
  }

  /// Smooth FFT data.
  ///
  /// When new data is read and the values are decreasing, the new value will be
  /// decreased with an amplitude between the old and the new value.
  /// This will resul on a less shaky visualization.
  /// [smooth] must be in the [0.0 ~ 1.0] range.
  /// 0 = no smooth
  /// 1 = full smooth
  /// the new value is calculated with:
  /// newFreq = smooth * oldFreq + (1 - smooth) * newFreq
  ///
  /// Return [CaptureErrors.captureNoError] if no error.
  ///
  CaptureErrors setCaptureFftSmoothing(double smooth) {
    final ret = SoLoudController().captureFFI.setCaptureFftSmoothing(smooth);
    _logCaptureError(ret, from: 'setCaptureFftSmoothing() result');
    return ret;
  }

  /// Start capturing audio data.
  ///
  /// Return [CaptureErrors.captureNoError] if no error
  ///
  CaptureErrors startCapture() {
    final ret = SoLoudController().captureFFI.startCapture();
    _logCaptureError(ret, from: 'startCapture() result');
    if (ret == CaptureErrors.captureNoError) {
      // ignore: deprecated_member_use_from_same_package
      audioEvent.add(AudioEvent.captureStarted);
    }
    return ret;
  }

  /// Stop and deinit capture device.
  ///
  /// Return [CaptureErrors.captureNoError] if no error.
  ///
  CaptureErrors stopCapture() {
    final ret = SoLoudController().captureFFI.stopCapture();
    _logCaptureError(ret, from: 'stopCapture() result');
    if (ret == CaptureErrors.captureNoError) {
      isCaptureInited = false;
      // ignore: deprecated_member_use_from_same_package
      SoLoud.instance.audioEvent.add(AudioEvent.captureStopped);
    }
    return ret;
  }

  /// Utility method that logs a [Level.SEVERE] message if [captureError]
  /// is anything other than [CaptureErrors.captureNoError].
  ///
  /// Optionally takes a [from] string, so that it can construct messages
  /// with more context:
  ///
  /// ```dart
  /// _logCaptureError(result, from: 'setCaptureFftSmoothing()');
  /// ```
  ///
  /// The code above may produce a log record such as:
  ///
  /// ```text
  /// [SoLoudCapture] play(): CaptureErrors.captureNotInited,
  /// ```
  void _logCaptureError(CaptureErrors captureError, {String? from}) {
    if (captureError == CaptureErrors.captureNoError) {
      return;
    }

    if (!_log.isLoggable(Level.SEVERE)) {
      // Do not do extra work if the logger isn't listening.
      return;
    }

    final strBuf = StringBuffer();
    if (from != null) {
      strBuf.write('$from: ');
    }
    strBuf.write(captureError.toString());
    _log.severe(strBuf.toString());
  }
}
