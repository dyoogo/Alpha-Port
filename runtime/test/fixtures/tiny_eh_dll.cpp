// A DLL that throws: tiny_eh.exe catches its exception by type across the module boundary (PE-G1), and tiny_delay.exe
// imports tiny_eh_add through a delay-import descriptor (PE-G7). Under `gta6rt --run` it sits in the game folder, so
// rt::Loader maps it and registers its .pdata. Ordinary test code; no game data.
#include <windows.h>

#include <stdexcept>
#include <string>

namespace {

int g_unwound = 0;

struct Guard {
    ~Guard() { ++g_unwound; }
};

}  // namespace

// Throws std::runtime_error("tiny_eh_dll <value>") from a frame that owns a destructor, so the catching module can
// tell both that the exception reached it by type and that this frame was unwound.
extern "C" __declspec(dllexport) void __cdecl tiny_eh_throw(int value) {
    Guard guard;
    throw std::runtime_error("tiny_eh_dll " + std::to_string(value));
}

// How many Guard frames have been unwound so far.
extern "C" __declspec(dllexport) int __cdecl tiny_eh_unwound() {
    return g_unwound;
}

extern "C" __declspec(dllexport) int __cdecl tiny_eh_add(int a, int b) {
    return a + b;
}
