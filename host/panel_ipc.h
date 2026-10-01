// Side panel picture shared between ArcadeHost (writer) and EchoCam.dll (reader).
// Keep identical to EchoCam's copy (E:\echo halloween\echocam\panel_ipc.h).
#pragma once
#include <Windows.h>
#include <cstdint>

namespace panel_ipc
{
	constexpr wchar_t NAME[] = L"Local\\EchoCam.Panel";
	constexpr uint32_t MAGIC = 0x4C4E4150; // "PANL"
	constexpr int WIDTH = 1024, HEIGHT = 574;

	struct Shared
	{
		uint32_t magic;
		volatile LONG visible;  // 1 while the panel should be shown
		volatile LONG serial;   // bumped after each new picture
		volatile LONG front;    // which frame holds the newest picture
		uint32_t frames[2][WIDTH * HEIGHT]; // BGRA, sRGB
	};
}
