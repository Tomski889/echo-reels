// CAMERA tile: EchoCam (bin\win10\plugins\EchoCam.dll) moves the game's second viewport (-capturevp2) to a
// controller, and the game shows that viewport in its desktop window. The tile shows that window live on the tablet,
// saves photos of it, and switches EchoCam between hands and between the front and selfie views.
#pragma once
#include "host.h"

struct CameraOptions {
    bool rightHand = false;  // default: the left hand (the tablet hand)
    bool selfie = false;     // looks back at you
    float reach = .25f;      // metres out from the controller (a selfie stick)
    bool frozen = false;     // stays where it is in the world (a tripod)
};
constexpr float CAMERA_REACH_MIN = .25f, CAMERA_REACH_MAX = 3.f;

std::wstring echoCamDll(const Config& config);             // plugins\EchoCam.dll beside the EchoArcade folder
CameraOptions loadCameraOptions(const Config& config);     // from EchoCam.ini
void saveCameraOptions(const Config& config, const CameraOptions& options);  // EchoCam re-reads it live
// On: the camera follows the chosen hand; off: the game's own view from the head (outside the CAMERA tile)
void setHandCamera(const Config& config, bool on);
HWND findEchoWindow();                                     // the game's main window (the camera view)
// Saves a BGRA frame as Pictures\Echo\Echo_<date>_<time>.png; returns the file name, empty on failure.
std::wstring savePhoto(const std::vector<uint8_t>& bgra, int w, int h);
