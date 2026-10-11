#include "anduefker/ue/StorageLayout.hpp"

#include <algorithm>

namespace anduefker::ue
{
    std::vector<StorageLayoutDescription> ValueStorageCandidates(
        const ir::TypeReferenceIR &reference, int32_t pointerWidth, int32_t nameSize)
    {
        std::vector<StorageLayoutDescription> result;
        if ((pointerWidth != 4 && pointerWidth != 8) || reference.elementSize <= 0 ||
            (nameSize != 4 && nameSize != 8 && nameSize != 12))
            return result;
        const auto align = [](int32_t size, int32_t alignment)
        { return (size + alignment - 1) / alignment * alignment; };
        const auto add = [&](const char *id, const char *range, int32_t size, int32_t alignment,
                             std::vector<std::pair<std::string, int32_t>> offsets)
        {
            if (size == reference.elementSize)
                result.push_back({std::string(id) + "-p" + std::to_string(pointerWidth) + "-n" +
                                      std::to_string(nameSize) + "-s" + std::to_string(size),
                                  range, size, alignment, std::move(offsets)});
        };
        switch (reference.kind)
        {
        case ir::PropertyKind::Text:
            add("text-shared-reference", "4.23-5.3", align(2 * pointerWidth + 4, pointerWidth), pointerWidth,
                {{"TextData", 0}, {"ReferenceController", pointerWidth}, {"Flags", 2 * pointerWidth}});
            add("text-intrusive-reference", "5.4-5.6", align(pointerWidth + 4, pointerWidth), pointerWidth,
                {{"TextData", 0}, {"Flags", pointerWidth}});
            break;
        case ir::PropertyKind::WeakObject:
            add("weak-object-pair", "4.23-5.6", 8, 4, {{"ObjectIndex", 0}, {"ObjectSerialNumber", 4}});
            add("weak-object-remote", "5.6; UE_WITH_REMOTE_OBJECT_HANDLE", 16, 8,
                {{"ObjectIndex", 0}, {"ObjectSerialNumber", 4}, {"RemoteObjectId", 8}});
            break;
        case ir::PropertyKind::Delegate:
            add("script-delegate-unchecked", "4.23-5.6; detector-disabled-or-empty-base", 8 + nameSize, 4,
                {{"ObjectIndex", 0}, {"ObjectSerialNumber", 4}, {"FunctionName", 8}});
            add("script-delegate-tail-checked", "5.2; delegate-race-detector-and-DO_CHECK-enabled",
                8 + nameSize + 12, 4,
                {{"ObjectIndex", 0}, {"ObjectSerialNumber", 4}, {"FunctionName", 8}, {"AccessDetector", 8 + nameSize}});
            // Android ARM ABI 中 uint64/atomic<uint64> 按 8 字节对齐
            add("script-delegate-checked", "5.3-5.6; delegate-race-detector-enabled", align(16 + nameSize, 8), 8,
                {{"AccessDetector", 0}, {"ObjectIndex", 8}, {"ObjectSerialNumber", 12}, {"FunctionName", 16}});
            add("script-delegate-remote-unchecked", "5.6; remote-weak-and-detector-disabled",
                align(16 + nameSize, 8), 8,
                {{"ObjectIndex", 0}, {"ObjectSerialNumber", 4}, {"RemoteObjectId", 8}, {"FunctionName", 16}});
            add("script-delegate-remote-checked", "5.6; remote-weak-and-detector-enabled",
                align(24 + nameSize, 8), 8,
                {{"AccessDetector", 0}, {"ObjectIndex", 8}, {"ObjectSerialNumber", 12}, {"RemoteObjectId", 16}, {"FunctionName", 24}});
            break;
        case ir::PropertyKind::MulticastDelegate:
            if (reference.delegateStorage == ir::DelegateStorageKind::SparseMulticast)
                add("sparse-delegate-bound", "4.23-5.6", 1, 1, {{"Bound", 0}});
            else if (reference.delegateStorage == ir::DelegateStorageKind::InlineMulticast)
            {
                add("inline-delegate-unchecked", "4.23-5.6; detector-disabled-or-empty-base",
                    pointerWidth + 8, pointerWidth, {{"InvocationList", 0}});
                add("inline-delegate-tail-checked", "5.2; delegate-race-detector-and-DO_CHECK-enabled",
                    align(pointerWidth + 8 + 12, pointerWidth), pointerWidth,
                    {{"InvocationList", 0}, {"AccessDetector", pointerWidth + 8}});
                add("inline-delegate-checked", "5.3-5.6; delegate-race-detector-enabled",
                    align(8 + pointerWidth + 8, 8), 8, {{"AccessDetector", 0}, {"InvocationList", 8}});
            }
            break;
        case ir::PropertyKind::LazyObject:
            add("lazy-object-with-tag", "4.23-5.2", 28, 4,
                {{"ObjectIndex", 0}, {"ObjectSerialNumber", 4}, {"TagAtLastTest", 8}, {"ObjectId", 12}});
            add("lazy-object-without-tag", "5.3-5.6", 24, 4,
                {{"ObjectIndex", 0}, {"ObjectSerialNumber", 4}, {"ObjectId", 8}});
            add("lazy-object-remote", "5.6; UE_WITH_REMOTE_OBJECT_HANDLE", 32, 8,
                {{"ObjectIndex", 0}, {"ObjectSerialNumber", 4}, {"RemoteObjectId", 8}, {"ObjectId", 16}});
            break;
        case ir::PropertyKind::SoftObject:
        case ir::PropertyKind::SoftClass:
            for (int family = 0; family < 4; ++family)
            {
                const bool tagged = family < 2;
                const bool topLevel = family != 0;
                const int32_t path = align(tagged ? 12 : 8, pointerWidth);
                const int32_t subPath = align((topLevel ? 2 : 1) * nameSize, pointerWidth);
                std::vector<std::pair<std::string, int32_t>> offsets =
                    {{"ObjectIndex", 0}, {"ObjectSerialNumber", 4}, {"AssetPath", path}, {"SubPath", path + subPath}};
                if (tagged)
                    offsets.emplace_back("TagAtLastTest", 8);
                const char *ids[] = {"soft-object-name-tag", "soft-object-top-level-tag",
                                     "soft-object-top-level", "soft-object-top-level-utf8"};
                const char *ranges[] = {"4.23-5.0; TCHAR", "5.1-5.2; TCHAR",
                                        "5.3-5.5; TCHAR", "5.6; UTF8"};
                add(ids[family], ranges[family], align(path + subPath + pointerWidth + 8, pointerWidth),
                    pointerWidth, std::move(offsets));
            }
            {
                const int32_t pathAlignment = std::max(pointerWidth, 4);
                const int32_t subPath = align(2 * nameSize, pointerWidth);
                add("soft-object-remote-utf8", "5.6; UE_WITH_REMOTE_OBJECT_HANDLE; UTF8",
                    align(16 + subPath + pointerWidth + 8, 8), 8,
                    {{"ObjectIndex", 0}, {"ObjectSerialNumber", 4}, {"RemoteObjectId", 8}, {"AssetPath", align(16, pathAlignment)}, {"SubPath", 16 + subPath}});
            }
            break;
        default:
            break;
        }
        return result;
    }

    std::optional<StorageLayoutDescription> CommonValueStorage(
        const ir::TypeReferenceIR &reference, int32_t pointerWidth, int32_t nameSize)
    {
        const auto candidates = ValueStorageCandidates(reference, pointerWidth, nameSize);
        if (candidates.empty())
            return std::nullopt;
        auto result = candidates.front();
        if (std::any_of(candidates.begin(), candidates.end(), [&](const auto &candidate)
                        { return candidate.alignment != result.alignment; }))
            return std::nullopt;
        if (candidates.size() > 1)
        {
            result.id = "common-" + result.id;
            result.sourceRange = "size-compatible-source-candidate-intersection";
            std::erase_if(result.offsets, [&](const auto &field)
                          { return std::any_of(candidates.begin(), candidates.end(), [&](const auto &candidate)
                                               { return std::find(candidate.offsets.begin(), candidate.offsets.end(), field) == candidate.offsets.end(); }); });
        }
        return result;
    }

    std::optional<MemoryImagePointer> DecodeMemoryImagePointer(
        uint64_t raw, uint64_t fieldAddress, int32_t pointerWidth, MemoryImagePointerEncoding encoding)
    {
        if ((pointerWidth != 4 && pointerWidth != 8) ||
            (encoding != MemoryImagePointerEncoding::RelativeShift1 && encoding != MemoryImagePointerEncoding::PackedOffset40))
            return std::nullopt;
        const uint64_t maximum = pointerWidth == 4 ? UINT32_MAX : UINT64_MAX;
        if (fieldAddress > maximum)
            return std::nullopt;
        MemoryImagePointer result;
        result.frozen = (raw & 1) != 0;
        if (!result.frozen)
        {
            result.address = raw & maximum;
            return result;
        }
        const unsigned shift = encoding == MemoryImagePointerEncoding::RelativeShift1 ? 1 : 24;
        const unsigned bits = 64 - shift;
        const uint64_t payload = raw >> shift;
        const bool negative = (payload & (uint64_t{1} << (bits - 1))) != 0;
        const uint64_t magnitude = negative ? (uint64_t{1} << bits) - payload : payload;
        if (negative ? magnitude > fieldAddress : magnitude > maximum - fieldAddress)
            return std::nullopt;
        result.address = negative ? fieldAddress - magnitude : fieldAddress + magnitude;
        if (encoding == MemoryImagePointerEncoding::PackedOffset40)
            result.typeIndex = static_cast<int32_t>((raw >> 1) & 0x7FFFFFu) - 1;
        return result;
    }
} // namespace anduefker::ue
