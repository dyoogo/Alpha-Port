// A program that imports tiny_eh_dll.dll through a delay-import descriptor (/DELAYLOAD, PE-G7 of
// Shared/CLAUDE_PE_COVERAGE_REVIEW.md). Natively the delay helper loads the DLL at the first call; under
// `gta6rt --run` rt::Loader binds delay imports eagerly (pe_loader.cpp, "Delay imports are bound eagerly"), so the
// call must work either way. The image checks it really carries a delay-import directory, so a link that dropped
// /DELAYLOAD cannot pass as this test. Ordinary test code; no game data.
#include <windows.h>

#include <cstdio>

extern "C" IMAGE_DOS_HEADER __ImageBase;
extern "C" __declspec(dllimport) int __cdecl tiny_eh_add(int a, int b);

int main() {
    const auto* base = reinterpret_cast<const unsigned char*>(&__ImageBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + __ImageBase.e_lfanew);
    const IMAGE_DATA_DIRECTORY& delay = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!delay.VirtualAddress || !delay.Size) {
        std::printf("tiny_delay: FAIL the image has no delay-import directory\n");
        return 1;
    }
    const int sum = tiny_eh_add(40, 2);
    if (sum != 42) {
        std::printf("tiny_delay: FAIL tiny_eh_add(40, 2) returned %d\n", sum);
        return 1;
    }
    std::printf("tiny_delay: PASS\n");
    return 0;
}
