#define WIN32_LEAN_AND_MEAN
#include <windows.h>
extern "C" __declspec(dllexport) void PublishFrame(void* data, int index) {
    auto bytes=static_cast<unsigned char*>(data);
    MemoryBarrier();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(bytes+36),index);
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(bytes+40));
}
