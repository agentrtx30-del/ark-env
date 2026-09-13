#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdint>
#include <cstddef>

class WinMemory
{
public:
    ~WinMemory();

    bool open(DWORD pid);
    void close();

    bool isOpen() const;
    DWORD lastOpenError() const;
    HANDLE nativeHandle() const;

    bool readBytes(uint64_t address, void* out, size_t size) const;

    template <typename T>
    bool read(uint64_t address, T& out) const
    {
        return readBytes(address, &out, sizeof(T));
    }

private:
    HANDLE processHandle_ = nullptr;
    mutable DWORD lastError_ = 0;
};

bool isSaneUserPointer(uint64_t p);

bool readPtr(
    const WinMemory& mem,
    uint64_t address,
    uint64_t& outPtr
);