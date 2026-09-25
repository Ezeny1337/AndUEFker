#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/types.h>

namespace anduefker::memory
{
    enum class ReadError
    {
        None,
        NotInitialized,
        InvalidArgument,
        UnreadableRange,
        PartialRead,
        BackendFailure,
    };

    struct ReadResult
    {
        ReadError error = ReadError::None;
        uintptr_t address = 0;
        size_t requested = 0;
        size_t transferred = 0;

        [[nodiscard]] bool Ok() const { return error == ReadError::None && transferred == requested; }
    };

    struct ReadStats
    {
        uint64_t operations = 0;
        uint64_t requestedBytes = 0;
        uint64_t transferredBytes = 0;
        uint64_t failures = 0;
    };

    class IMemorySource
    {
    public:
        virtual ~IMemorySource() = default;

        [[nodiscard]] virtual bool IsInitialized() const = 0;
        [[nodiscard]] virtual pid_t ProcessId() const = 0;
        [[nodiscard]] virtual bool RefreshAddressSpace() = 0;
        [[nodiscard]] virtual bool IsReadable(uintptr_t address, size_t size) const = 0;
        [[nodiscard]] virtual bool IsExecutable(uintptr_t address, size_t size) const = 0;
        [[nodiscard]] virtual ReadResult ReadBytes(uintptr_t address, void *buffer, size_t size) const = 0;
        [[nodiscard]] virtual const ReadStats &Stats() const = 0;

        template <typename T>
        [[nodiscard]] bool Read(uintptr_t address, T &value) const
        {
            const ReadResult result = ReadBytes(address, &value, sizeof(T));
            return result.Ok();
        }
    };
} // namespace anduefker::memory
