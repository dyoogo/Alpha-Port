// Exceptions and unwinding in mapped code (PE-G1..G4, Shared/CLAUDE_PE_COVERAGE_REVIEW.md). Each mode runs natively,
// as the control, and under `gta6rt --run`, where this EXE and tiny_eh_dll.dll are mapped by rt::Loader and dispatch
// can only unwind through them if their .pdata was registered:
//   cross      a std::runtime_error thrown in tiny_eh_dll.dll (loaded at run time) is caught by type here, and the
//              DLL frame's destructor ran during the unwind (G1)
//   seh        an access violation handled by __except in this image; a filter that declines, so the frame above
//              handles it; a __finally that runs while a raised exception passes through (G2)
//   thread     a C++ exception and an SEH fault, each caught on a std::thread this image started (G3)
//   stackwalk  RtlCaptureStackBackTrace walks out of this image's frames, and each of the first two return
//              addresses resolves to a RUNTIME_FUNCTION of this image (G4)
// Prints one "ok"/"FAIL" line per check and then "tiny_eh: PASS" or "tiny_eh: FAILED <n>"; exits 0 only on PASS.
// Ordinary test code; no game data.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("tiny_eh: %s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

bool in_this_image(const void* address) {
    const auto base = reinterpret_cast<const std::uint8_t*>(&__ImageBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + __ImageBase.e_lfanew);
    const auto* p = static_cast<const std::uint8_t*>(address);
    return p >= base && p < base + nt->OptionalHeader.SizeOfImage;
}

// ---- G1: C++ exception across mapped modules ----

void run_cross() {
    HMODULE dll = LoadLibraryW(L"tiny_eh_dll.dll");
    check(dll != nullptr, "LoadLibraryW(tiny_eh_dll.dll)");
    if (!dll) return;
    using ThrowFn = void(__cdecl*)(int);
    using CountFn = int(__cdecl*)();
    const auto thrower = reinterpret_cast<ThrowFn>(GetProcAddress(dll, "tiny_eh_throw"));
    const auto unwound = reinterpret_cast<CountFn>(GetProcAddress(dll, "tiny_eh_unwound"));
    check(thrower && unwound, "GetProcAddress(tiny_eh_throw, tiny_eh_unwound)");
    if (thrower && unwound) {
        std::string what;
        int how = 0;
        try {
            thrower(7);
        } catch (const std::runtime_error& e) {
            what = e.what();
            how = 1;
        } catch (...) {
            how = 2;
        }
        check(how == 1 && what == "tiny_eh_dll 7", "a std::runtime_error thrown in the DLL is caught by type here");
        check(unwound() == 1, "the DLL frame's destructor ran during the unwind");
    }
    FreeLibrary(dll);
}

// ---- G2: SEH in this image (functions with __try hold no objects with destructors) ----

__declspec(noinline) int fault_handled_here(volatile int* p) {
    __try {
        *p = 1;
        return 0;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
        return 1;
    }
}

__declspec(noinline) int fault_declined(volatile int* p) {
    __try {
        *p = 1;
        return 0;
    } __except (EXCEPTION_CONTINUE_SEARCH) {
        return 2;  // never: the filter declines
    }
}

__declspec(noinline) int fault_handled_above(volatile int* p) {
    __try {
        return fault_declined(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 3;
    }
}

constexpr DWORD kTinyException = 0xE0000001;
volatile int g_finally_ran = 0;

__declspec(noinline) void raise_through_finally() {
    __try {
        RaiseException(kTinyException, 0, 0, nullptr);
    } __finally {
        g_finally_ran = 1;
    }
}

__declspec(noinline) int raised_handled_above() {
    __try {
        raise_through_finally();
        return 0;
    } __except (GetExceptionCode() == kTinyException ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return g_finally_ran ? 4 : 5;
    }
}

volatile int* volatile g_null = nullptr;  // read at run time, so the compiler cannot see the fault coming

void run_seh() {
    check(fault_handled_here(g_null) == 1, "an access violation is handled by __except in the same frame");
    check(fault_handled_above(g_null) == 3, "a declining filter passes it to the __except of the frame above");
    check(raised_handled_above() == 4, "a __finally runs while a raised exception unwinds through it");
}

// ---- G3: both kinds on a thread this image started ----

void run_thread() {
    int cpp = 0, seh = 0;
    std::thread t([&] {
        try {
            throw std::runtime_error("thread");
        } catch (const std::runtime_error& e) {
            cpp = std::strcmp(e.what(), "thread") == 0 ? 1 : 2;
        }
        seh = fault_handled_here(g_null);
    });
    t.join();
    check(cpp == 1, "a C++ exception is caught on a thread this image started");
    check(seh == 1, "an access violation is handled by __except on that thread");
}

// ---- G4: stack walk through this image's frames ----

__declspec(noinline) USHORT capture(void** frames, ULONG count) {
    return RtlCaptureStackBackTrace(0, count, frames, nullptr);
}

void run_stackwalk() {
    void* frames[32] = {};
    const USHORT n = capture(frames, 32);
    check(n >= 3, "RtlCaptureStackBackTrace walks at least three frames");
    for (USHORT i = 0; i < 2 && i < n; ++i) {
        DWORD64 image = 0;
        const PRUNTIME_FUNCTION fn =
            RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(frames[i]), &image, nullptr);
        const bool here = in_this_image(frames[i]) && fn != nullptr &&
                          image == reinterpret_cast<DWORD64>(&__ImageBase);
        check(here, i == 0 ? "return address 0 resolves to a RUNTIME_FUNCTION of this image"
                           : "return address 1 resolves to a RUNTIME_FUNCTION of this image");
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool ran = false;
    for (int i = 1; i < argc; ++i) {
        const char* mode = argv[i];
        if (!std::strcmp(mode, "cross")) run_cross();
        else if (!std::strcmp(mode, "seh")) run_seh();
        else if (!std::strcmp(mode, "thread")) run_thread();
        else if (!std::strcmp(mode, "stackwalk")) run_stackwalk();
        else {
            std::printf("tiny_eh: unknown mode %s\n", mode);
            ++g_failures;
        }
        ran = true;
    }
    if (!ran) check(false, "a mode (cross, seh, thread, stackwalk) was given");
    if (g_failures) {
        std::printf("tiny_eh: FAILED %d\n", g_failures);
        return 1;
    }
    std::printf("tiny_eh: PASS\n");
    return 0;
}
