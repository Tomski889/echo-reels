#pragma once
#include "../shared/arcade_ipc.h"

// Streams host frames into the tablet's screen texture on the GPU.
//
// The texture is identified when the engine creates it: a 2D B8G8R8A8_UNORM
// resource of exactly ARCADE_TEX_W x ARCADE_TEX_H (a size no stock texture
// uses). Uploads are recorded on the game's own direct queue, just before one
// of its submissions, so they are ordered with the frames that sample it.
namespace stream {
bool install(arcade::Shared* shared);  // hooks D3D12; call from a normal thread (not DllMain)
void setVisible(bool visible);         // upload only while the ARCADE page is on screen
void shutdown();
}
