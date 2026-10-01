// Side panel shared between ArcadeHost and EchoCam.dll: ArcadeHost writes the picture, EchoCam writes the finger
// (hover and touches, in panel pixels). Keep identical to E:\echo halloween\echocam\panel_ipc.h.
#pragma once
#include <Windows.h>
#include <cstdint>

namespace panel_ipc
{
	constexpr wchar_t NAME[] = L"Local\\EchoCam.Panel2";
	constexpr uint32_t MAGIC = 0x324C4E50; // "PNL2"
	constexpr int WIDTH = 576, HEIGHT = 768; // portrait 3:4, like a tablet held upright
	constexpr int TOUCHES = 16;

	struct Touch
	{
		volatile LONG seq; // index + 1 once written
		LONG down;         // 1 press, 0 release
		float x, y;        // panel pixels
	};

	struct Shared
	{
		uint32_t magic;
		// ArcadeHost
		volatile LONG visible;  // 1 while the panel should be shown
		volatile LONG serial;   // bumped after each new picture
		volatile LONG front;    // which frame holds the newest picture
		// EchoCam
		volatile LONG hover;    // 1 while a finger is near the panel
		float hoverX, hoverY;   // where it points, panel pixels
		volatile LONG touchWrite;
		Touch touches[TOUCHES];
		// ArcadeHost
		uint32_t frames[2][WIDTH * HEIGHT]; // BGRA, sRGB; alpha 0 = see-through
	};
}
