#pragma once
#include <string>

// Tablet size (SETTINGS slider / echo_tweaks.ini [tablet] scale).
//
// The hand tablet is one actor whose local transform (CTransformCR: rotation quat,
// position, scale) places a 0.3 m canvas beside the wrist; the canvas and every
// touch hitbox hang off that transform. We find the live copy of that transform in
// Echo's memory by its exact stock values and set its scale, re-centring it.
namespace tablet {
void start(const std::wstring& pluginDir);  // reads the saved size; worker thread
void request(float scale);                  // desired size (from the SETTINGS tab)
void tabletPresent(bool present);           // tablet canvases loaded / unloaded
}
