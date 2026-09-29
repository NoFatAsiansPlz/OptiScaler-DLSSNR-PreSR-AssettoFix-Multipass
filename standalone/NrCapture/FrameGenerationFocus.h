#pragma once
#include "Common.h"
namespace nr {
// Redirect only sl.common's imported focus query, in this process's private IAT.
// Windows focus, application focus checks, other processes and DLL files are untouched.
class FrameGenerationFocus {
public:
    explicit FrameGenerationFocus(void* commonFunction);
    ~FrameGenerationFocus();
    FrameGenerationFocus(const FrameGenerationFocus&) = delete;
    void Enable(HWND window);
    void Disable();
private:
    HMODULE common = nullptr;
    void** slot = nullptr;
    void* original = nullptr;
};
}
