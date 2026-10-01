// CAMERA tile: EchoCam (bin\win10\plugins\EchoCam.dll) moves the game's second viewport (-capturevp2) to a
// controller, and the game shows that viewport in its desktop window. The tile shows that window live on the tablet,
// saves photos of it, and switches EchoCam between hands and between the front and selfie views.
#pragma once
#include "host.h"

struct CameraOptions {
    enum Source { Tablet, LeftHand, RightHand };
    Source source = Tablet;  // the side panel's phone camera (default), or a controller
    bool selfie = false;     // looks back at you
    float reach = .25f;      // metres out from the controller (a selfie stick)
    bool frozen = false;     // stays where it is in the world (a tripod)
    int smoothing = 0;       // index into CAMERA_SMOOTHING (EchoCam eases the camera like a gimbal)
    int resolution = 0;      // index into CAMERA_RESOLUTIONS (the Echo window's size = the camera picture's)
    int fisheye = 0;         // index into CAMERA_FISHEYE
};
constexpr float CAMERA_SMOOTHING[] = {0.f, .25f, .5f, .8f};
constexpr const wchar_t* CAMERA_SMOOTHING_NAMES[] = {L"OFF", L"LOW", L"MED", L"HIGH"};
struct CameraResolution { int w, h; const wchar_t* name; };
// Fisheye: a wider field of view (EchoCam) bent like a fisheye lens (the panel and photos)
struct CameraFisheye { const wchar_t* name; float fov, strength; };
constexpr CameraFisheye CAMERA_FISHEYE[] = {{L"OFF", 0, 0}, {L"LOW", 105, .55f}, {L"MED", 125, .85f}, {L"HIGH", 150, 1.15f}};
constexpr CameraResolution CAMERA_RESOLUTIONS[] = {{0, 0, L"WINDOW"}, {1280, 720, L"720p"}, {1920, 1080, L"1080p"}, {2560, 1440, L"1440p"}, {3840, 2160, L"4K"}};
constexpr float CAMERA_REACH_MIN = .25f, CAMERA_REACH_MAX = 3.f;

std::wstring echoCamDll(const Config& config);             // plugins\EchoCam.dll beside the EchoArcade folder
CameraOptions loadCameraOptions(const Config& config);     // from EchoCam.ini
void saveCameraOptions(const Config& config, const CameraOptions& options);  // EchoCam re-reads it live
// On: the camera follows the chosen hand; off: the game's own view from the head (outside the CAMERA tile)
void setHandCamera(const Config& config, bool on);
HWND findEchoWindow();                                     // the game's main window (the camera view)
// Sizes the window's picture (client area) to the resolution; index 0 puts back the size it had before the first change.
void applyCameraResolution(HWND window, int index);
// Fisheye bend of a BGRA picture (w x h, rows 'pitch' bytes apart) into 'out' (w x h, packed); strength 0 copies.
// The centre is magnified and the edges squeezed: a point at radius r (0..1 of the half diagonal) shows what was at
// tan(r * strength) / tan(strength).
void fisheye(const uint8_t* src, int w, int h, int pitch, std::vector<uint8_t>& out, float strength);
// The same with a precomputed map, for the live panel (nearest pixel)
class FisheyeMap {
public:
    void apply(const uint32_t* src, int w, int h, int pitch, uint32_t* out, int outPitch, float strength);
private:
    int w_ = 0, h_ = 0;
    float strength_ = 0;
    std::vector<int> from_;  // source pixel per output pixel
};
// Saves a BGRA frame as Pictures\Echo\Echo_<date>_<time>.png; returns the file name, empty on failure.
std::wstring savePhoto(const std::vector<uint8_t>& bgra, int w, int h);
