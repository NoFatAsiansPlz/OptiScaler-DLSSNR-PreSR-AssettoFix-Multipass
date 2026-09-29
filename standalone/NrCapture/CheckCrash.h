#pragma once
#include "Common.h"
#include <dbghelp.h>
#include <fstream>
// Test executable only: records its own exception context, never opens another process.
inline LONG WINAPI CheckCrash(EXCEPTION_POINTERS* exception) {
    auto file = nr::DataDirectory() / L"check-crash.txt";
    std::ofstream out(file);
    out << std::format("Exception {:08X} at {}\n", exception->ExceptionRecord->ExceptionCode,
        exception->ExceptionRecord->ExceptionAddress);
    CONTEXT context = *exception->ContextRecord;
    out << std::format("rax={:x} rcx={:x} rdx={:x} r8={:x} r9={:x}\n", context.Rax, context.Rcx, context.Rdx, context.R8, context.R9);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    STACKFRAME64 stack{}; stack.AddrPC = {context.Rip, 0, AddrModeFlat};
    stack.AddrStack = {context.Rsp, 0, AddrModeFlat}; stack.AddrFrame = {context.Rbp, 0, AddrModeFlat};
    for (unsigned i = 0; i < 24 && stack.AddrPC.Offset; ++i) {
        IMAGEHLP_MODULE64 module{}; module.SizeOfStruct = sizeof(module);
        SymGetModuleInfo64(GetCurrentProcess(), stack.AddrPC.Offset, &module);
        out << std::format("{} + {:x}\n", module.ImageName, stack.AddrPC.Offset - module.BaseOfImage);
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &stack, &context,
            nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
    }
    out.flush(); return EXCEPTION_EXECUTE_HANDLER;
}
