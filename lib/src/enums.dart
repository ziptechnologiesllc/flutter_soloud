import 'package:meta/meta.dart';

/// CaptureDevice exposed to Dart
final class CaptureDevice {
  /// Constructs a new [CaptureDevice].
  // ignore: avoid_positional_boolean_parameters
  const CaptureDevice(this.name, this.isDefault);

  /// The name of the device.
  final String name;

  /// Whether this is the default capture device.
  final bool isDefault;
}

/// Possible capture errors
enum CaptureErrors {
  /// No error
  captureNoError,

  /// Capture failed to initialize
  captureInitFailed,

  /// Capture not yet initialized
  captureNotInited,

  /// null pointer. Could happens when passing a non initialized
  /// pointer (with calloc()) to retrieve FFT or wave data
  nullPointer,

  /// Frames did not write
  captureWriteFailed;

  /// Returns a human-friendly sentence describing the error.
  String get _asSentence {
    switch (this) {
      case CaptureErrors.captureNoError:
        return 'No error';
      case CaptureErrors.captureInitFailed:
        return 'Capture failed to initialize';
      case CaptureErrors.captureNotInited:
        return 'Capture not yet initialized';
      case CaptureErrors.nullPointer:
        return 'Capture null pointer error. Could happens when passing a non '
            'initialized pointer (with calloc()) to retrieve FFT or wave data. '
            'Or, setVisualization has not been enabled.';
      case CaptureErrors.captureWriteFailed:
        return 'Failed to write full number of requested frames to disk.';
    }
  }

  @override
  String toString() => 'CaptureErrors.$name ($_asSentence)';
}

/// Possible player errors.
/// New values must be enumerated at the bottom
///
/// WARNING: Keep these in sync with `src/enums.h`.
@internal
enum PlayerErrors {
  /// No error
  noError(0),

  /// Some parameter is invalid
  invalidParameter(1),

  /// File not found
  fileNotFound(2),

  /// File found, but could not be loaded
  fileLoadFailed(3),

  /// The sound file has already been loaded
  fileAlreadyLoaded(4),

  /// DLL not found, or wrong DLL
  dllNotFound(5),

  /// Out of memory
  outOfMemory(6),

  /// Feature not implemented
  notImplemented(7),

  /// Other error
  unknownError(8),

  /// null pointer. Could happens when passing a non initialized
  /// pointer (with calloc()) to retrieve FFT or wave data
  nullPointer(9),

  /// The sound with specified hash is not found
  soundHashNotFound(10),

  /// Player not initialized
  backendNotInited(11),

  /// Filter not found
  filterNotFound(12),

  /// asking for wave and FFT is not enabled
  visualizationNotEnabled(13),

  /// The maximum number of filters has been reached (default is 8).
  maxNumberOfFiltersReached(14),

  /// The filter has already been added.
  filterAlreadyAdded(15),

  /// Player already inited.
  playerAlreadyInited(16);

  const PlayerErrors(this.value);

  /// The integer value of the error. This is the same number that is returned
  /// from the C++ API.
  final int value;

  /// Returns a human-friendly sentence describing the error.
  String get _asSentence {
    switch (this) {
      case PlayerErrors.noError:
        return 'No error';
      case PlayerErrors.invalidParameter:
        return 'Some parameters are invalid!';
      case PlayerErrors.fileNotFound:
        return 'File not found!';
      case PlayerErrors.fileLoadFailed:
        return 'File found, but could not be loaded!';
      case PlayerErrors.fileAlreadyLoaded:
        return 'The sound file has already been loaded!';
      case PlayerErrors.dllNotFound:
        return 'DLL not found, or wrong DLL!';
      case PlayerErrors.outOfMemory:
        return 'Out of memory!';
      case PlayerErrors.notImplemented:
        return 'Feature not implemented!';
      case PlayerErrors.unknownError:
        return 'Unknown error!';
      case PlayerErrors.nullPointer:
        return 'Capture null pointer error. Could happens when passing a non '
            'initialized pointer (with calloc()) to retrieve FFT or wave data. '
            'Or, setVisualization has not been enabled.';
      case PlayerErrors.soundHashNotFound:
        return 'The sound with specified hash is not found!';
      case PlayerErrors.backendNotInited:
        return 'Player not initialized!';
      case PlayerErrors.filterNotFound:
        return 'Filter not found!';
      case PlayerErrors.visualizationNotEnabled:
        return 'Asking for audio data is not enabled! Please use '
            '`setVisualizationEnabled(true);` to enable!';
      case PlayerErrors.maxNumberOfFiltersReached:
        return 'The maximum number of filters has been reached (default is 8)!';
      case PlayerErrors.filterAlreadyAdded:
        return 'Filter not found!';
      case PlayerErrors.playerAlreadyInited:
        return 'The player has already been inited!';
    }
  }

  @override
  String toString() => 'PlayerErrors.$name ($_asSentence)';
}

/// The types of waveforms.
enum WaveForm {
  /// Raw, harsh square wave.
  square,

  /// Raw, harsh saw wave.
  saw,

  /// Sine wave.
  sin,

  /// Triangle wave.
  triangle,

  /// Bounce, i.e, abs(sin()).
  bounce,

  /// Quarter sine wave, rest of period quiet.
  jaws,

  /// Half sine wave, rest of period quiet.
  humps,

  /// "Fourier" square wave; less noisy.
  fSquare,

  /// "Fourier" saw wave; less noisy.
  fSaw,
}

/// The way an audio file is loaded.
enum LoadMode {
  /// Load and decompress the audio file into RAM.
  /// Less CPU, more memory allocated, low latency.
  memory,

  /// Keep the file on disk and only load chunks as needed.
  /// More CPU, less memory allocated, seeking lags with MP3s.
  disk,
}
