#include "memory.h"

WinMemory::~WinMemory()
{
    close();
}

bool WinMemory::open(DWORD pid)
{
    close();

    processHandle_ = OpenProcess(
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE,
        pid
    );

    lastError_ = GetLastError();
    return processHandle_ != nullptr;
}

void WinMemory::close()
{
    if (processHandle_)
    {
        CloseHandle(processHandle_);
        processHandle_ = nullptr;
    }

    lastError_ = 0;
}

bool WinMemory::isOpen() const
{
    return processHandle_ != nullptr;
}

DWORD WinMemory::lastOpenError() const
{
    return lastError_;
}

HANDLE WinMemory::nativeHandle() const
{
    return processHandle_;
}

bool WinMemory::readBytes(uint64_t address, void* out, size_t size) const
{
    if (!processHandle_)
        return false;

    if (!out || size == 0)
        return false;

    SIZE_T bytesRead = 0;

    BOOL ok = ReadProcessMemory(
        processHandle_,
        reinterpret_cast<LPCVOID>(address),
        out,
        size,
        &bytesRead
    );

    lastError_ = GetLastError();

    return ok && bytesRead == size;
}

bool isSaneUserPointer(uint64_t p)
{
    if (p < 0x10000ULL)
        return false;

    if (p > 0x7FFFFFFFFFFFULL)
        return false;

    if ((p & 7ULL) != 0ULL)
        return false;

    return true;
}

bool readPtr(
    const WinMemory& mem,
    uint64_t address,
    uint64_t& outPtr
)
{
    outPtr = 0;

    if (!mem.read(address, outPtr))
        return false;

    if (!isSaneUserPointer(outPtr))
    {
        outPtr = 0;
        return false;
    }

    return true;
}