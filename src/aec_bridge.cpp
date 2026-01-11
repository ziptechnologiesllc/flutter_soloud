#include "aec_bridge.h"
#include <stdio.h>

// Global callback pointer - initialized to nullptr
AECOutputCallback g_aecOutputCallback = nullptr;

void aec_setOutputCallback(AECOutputCallback callback) {
  fprintf(stderr, "[Soloud Bridge] Setting AEC output callback to %p\n",
          callback);
  fflush(stderr);
  g_aecOutputCallback = callback;
}

void aec_clearOutputCallback() {
  fprintf(stderr, "[Soloud Bridge] Clearing AEC output callback\n");
  fflush(stderr);
  g_aecOutputCallback = nullptr;
}
