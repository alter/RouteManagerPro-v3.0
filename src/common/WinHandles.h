// src/common/WinHandles.h
#pragma once
#include <windows.h>
#include <memory>

struct HandleDeleter {
    void operator()(HANDLE h) {
        if (h && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
};

using UniqueHandle = std::unique_ptr<void, HandleDeleter>;