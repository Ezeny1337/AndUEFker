#include "anduefker/reflection/RuntimeInspector.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <unordered_map>

#include "anduefker/memory/CaptureMemorySource.hpp"
#include "anduefker/ue/ContainerReader.hpp"
#include "anduefker/ue/ObjectModelReader.hpp"
#include "anduefker/ue/StorageLayout.hpp"

namespace anduefker::reflection
{
    namespace
    {
        constexpr int32_t kMaxObjectSlots = 262144;
        constexpr size_t kMaxValues = 256;
        constexpr size_t kMaxFieldsPerClass = 256;

        std::optional<uintptr_t> Add(uintptr_t address, uint64_t offset)
        {
            if (!address || offset > UINTPTR_MAX || address > UINTPTR_MAX - static_cast<uintptr_t>(offset))
                return std::nullopt;
            return address + static_cast<uintptr_t>(offset);
        }

        std::optional<uint64_t> Word(const std::vector<uint8_t> &bytes, int32_t offset, int32_t width)
        {
            if (offset < 0 || width <= 0 || width > 8 || static_cast<size_t>(offset) > bytes.size() ||
                static_cast<size_t>(width) > bytes.size() - static_cast<size_t>(offset))
                return std::nullopt;
            uint64_t result = 0;
            std::memcpy(&result, bytes.data() + offset, static_cast<size_t>(width));
            return result;
        }

        int32_t Offset(const ir::StorageLayoutIR &layout, const char *name)
        {
            const auto field = std::find_if(layout.offsets.begin(), layout.offsets.end(),
                                            [name](const auto &value)
                                            { return value.first == name; });
            return field == layout.offsets.end() ? -1 : field->second;
        }

        ir::ValueCapture Capture(const memory::CaptureValidation &value)
        {
            ir::ValueCapture result;
            result.observationsStable = value.Stable();
            result.observedRanges = value.observedRanges;
            result.observedBytes = value.observedBytes;
            result.changedRanges = value.changedRanges;
            result.unreadableRanges = value.unreadableRanges;
            result.readFailures = value.readFailures;
            result.limitExceeded = value.limitExceeded;
            result.generationChanged = value.generationChanged;
            for (size_t index = 0; index < std::min<size_t>(value.readFailureSamples.size(), 8); ++index)
            {
                const auto &read = value.readFailureSamples[index];
                result.failures.push_back({read.address, static_cast<int32_t>(read.error), read.requested, read.transferred});
            }
            for (const auto &change : value.changes)
                result.changes.push_back({change.address, change.size, static_cast<int32_t>(change.read.error),
                                          change.read.transferred, change.before, change.after});
            return result;
        }

        const char *ValueKind(ir::PropertyKind kind)
        {
            switch (kind)
            {
            case ir::PropertyKind::Array:
                return "Array";
            case ir::PropertyKind::Set:
                return "Set";
            case ir::PropertyKind::Map:
                return "Map";
            case ir::PropertyKind::Text:
                return "Text";
            case ir::PropertyKind::WeakObject:
                return "WeakObject";
            case ir::PropertyKind::Delegate:
                return "Delegate";
            case ir::PropertyKind::MulticastDelegate:
                return "MulticastDelegate";
            default:
                return nullptr;
            }
        }

        std::optional<ir::ContainerStorageIR> HeapStorage(ir::PropertyKind kind, int32_t pointerWidth,
                                                          int32_t keySize = 0, int32_t valueSize = 0,
                                                          int32_t keyAlignment = 0, int32_t valueAlignment = 0)
        {
            const auto layout = ue::DescribeHeapContainer(kind, pointerWidth);
            if (!layout)
                return std::nullopt;
            auto result = kind == ir::PropertyKind::Array
                              ? std::optional<ir::ContainerStorageIR>(std::in_place)
                              : ue::DescribeSparseElement(kind, keySize, valueSize, keyAlignment, valueAlignment);
            if (!result)
                return std::nullopt;
            result->allocator = ir::ContainerAllocatorKind::Heap;
            result->allocatorStatus = "source-defined-native";
            result->headerStatus = "source-model";
            result->layoutId = layout->id;
            return result;
        }

        class Inspector
        {
        public:
            Inspector(memory::IMemorySource &memory, const binding::RuntimeBinding &binding,
                      const ue::EngineSchema &schema, const app::ModuleImage &module, const ir::ReflectionIR &reflection)
                : capture_(memory), objects_(capture_, binding, schema), schema_(schema),
                  pointerWidth_(module.pointerWidth), module_(module)
            {
                for (const auto &type : reflection.types)
                    types_.emplace(type.address, &type);
                for (const auto &[id, layout] : reflection.valueStorageLayouts)
                    if (id.starts_with("inline-delegate-"))
                        inlineLayouts_.push_back(layout);
                    else if (id.starts_with("weak-object-"))
                        weakSizes_.insert(layout.size);
            }

            ir::RuntimeEvidence Run(ir::SparseRootEvidence root)
            {
                ir::RuntimeEvidence result;
                result.sparse = std::move(root);
                if (pointerWidth_ != static_cast<int32_t>(sizeof(uintptr_t)) || !objects_.Initialize())
                {
                    result.status = "object-model-unavailable-or-pointer-width-mismatch";
                    return result;
                }
                result.objectSlots = objects_.Count();
                const int32_t limit = std::min(result.objectSlots, kMaxObjectSlots);
                std::map<std::pair<uintptr_t, std::string>, std::pair<size_t, size_t>> samples;
                std::map<std::string, size_t> kinds;
                std::unordered_map<uintptr_t, std::vector<Field>> fields;
                size_t fieldBudget = 8192;
                for (int32_t index = 0; index < limit; ++index)
                {
                    if (result.values.size() >= kMaxValues)
                    {
                        ++result.skipped["global-value-budget"];
                        break;
                    }
                    ++result.scannedSlots;
                    const auto object = objects_.Objects().ReadObject(index, true);
                    if (!object.IsValid())
                        continue;
                    const auto classAddress = objects_.Class(object.address, true);
                    const auto found = classAddress ? types_.find(*classAddress) : types_.end();
                    if (found == types_.end() || found->second->kind != ir::TypeKind::Class)
                        continue;
                    const auto flags = objects_.Flags(object.address);
                    if (!flags || (*flags & ue::kRFUnavailableDefinition) != 0)
                    {
                        ++result.skipped["object-not-ready"];
                        continue;
                    }
                    const bool cdo = (*flags & ue::kRFClassDefaultObject) != 0;
                    auto at = fields.find(*classAddress);
                    if (at == fields.end())
                    {
                        if (!fieldBudget)
                        {
                            ++result.skipped["global-field-expansion-budget"];
                            continue;
                        }
                        std::vector<Field> definitions;
                        std::set<uintptr_t> path;
                        const size_t budget = std::min(kMaxFieldsPerClass, fieldBudget);
                        size_t remaining = budget;
                        CollectFields(*found->second, 0, found->second->name, 0, remaining, path, definitions);
                        fieldBudget -= budget - remaining;
                        if (!remaining)
                            ++result.skipped["class-field-expansion-budget"];
                        at = fields.emplace(*classAddress, std::move(definitions)).first;
                    }
                    for (const auto &field : at->second)
                    {
                        if (result.values.size() >= kMaxValues)
                            break;
                        const char *kind = ValueKind(field.property->type.kind);
                        auto &count = samples[{field.property->address, field.path}];
                        auto &sampled = cdo ? count.second : count.first;
                        if (sampled >= (cdo ? 1u : 2u) ||
                            kinds[kind] >= (field.property->type.kind == ir::PropertyKind::Array ? 64u : 32u))
                        {
                            ++result.skipped["property-or-kind-sample-budget"];
                            continue;
                        }
                        ++sampled;
                        ++kinds[kind];
                        capture_.Reset();
                        ir::ValueObservation value;
                        value.objectAddress = object.address;
                        value.classAddress = *classAddress;
                        value.ownerAddress = field.owner;
                        value.propertyAddress = field.property->address;
                        value.path = field.path;
                        value.kind = kind;
                        value.classDefaultObject = cdo;
                        {
                            memory::CaptureObservationScope observing(capture_);
                            const auto identity = objects_.Objects().ReadObject(index);
                            const auto actualClass = objects_.Class(object.address);
                            const auto actualIndex = objects_.InternalIndex(object.address);
                            const auto actualFlags = objects_.Flags(object.address);
                            const auto address = Add(object.address, field.offset);
                            const auto actualSerial = objects_.Objects().ReadSerialNumber(identity);
                            if (!identity.IsValid() || identity.address != object.address || !actualClass ||
                                *actualClass != *classAddress || !actualIndex || *actualIndex != index ||
                                !actualFlags || (*actualFlags & ue::kRFUnavailableDefinition) != 0 ||
                                ((*actualFlags & ue::kRFClassDefaultObject) != 0) != cdo || !address)
                                value.status = "object-identity-changed-or-invalid";
                            else
                            {
                                value.address = *address;
                                if (actualSerial)
                                    value.numbers["OwnerObjectSerial"] = *actualSerial;
                                ReadValue(value, field.property->type);
                            }
                        }
                        value.capture = Capture(capture_.Validate());
                        ++result.statuses[(value.capture.observationsStable ? "" : "unstable:") + value.kind + ":" + value.status];
                        result.values.push_back(std::move(value));
                    }
                }
                result.scanComplete = result.scannedSlots == result.objectSlots;
                if (result.objectSlots > limit)
                    result.skipped["object-slot-budget"] = static_cast<size_t>(result.objectSlots - limit);
                InspectSparse(result.sparse);
                result.status = "bounded-observations-collected";
                return result;
            }

        private:
            struct Field
            {
                const ir::PropertyIR *property = nullptr;
                uintptr_t owner = 0;
                uint64_t offset = 0;
                std::string path;
            };

            void CollectFields(const ir::TypeIR &type, uint64_t base, const std::string &prefix, size_t depth,
                               size_t &remaining, std::set<uintptr_t> &path, std::vector<Field> &out) const
            {
                if (depth >= 16 || !remaining || type.size <= 0 || !path.insert(type.address).second)
                    return;
                for (const auto &property : type.properties)
                {
                    if (!remaining)
                        break;
                    --remaining;
                    const uint64_t extent = property.elementSize > 0 && property.arrayDim > 0
                                                ? uint64_t{static_cast<uint32_t>(property.elementSize)} *
                                                      static_cast<uint32_t>(property.arrayDim)
                                                : 0;
                    if (!extent || property.offset < 0 || static_cast<uint64_t>(property.offset) > static_cast<uint64_t>(type.size) ||
                        extent > static_cast<uint64_t>(type.size) - static_cast<uint32_t>(property.offset) ||
                        property.status != ir::ParseStatus::Complete || !property.type.nodeDetailsResolved)
                        continue;
                    const size_t elements = std::min<size_t>(property.arrayDim, 2);
                    for (size_t element = 0; element < elements; ++element)
                    {
                        const uint64_t offset = base + static_cast<uint32_t>(property.offset) +
                                                element * static_cast<uint32_t>(property.elementSize);
                        const std::string name = prefix + "." + property.name +
                                                 (property.arrayDim > 1 ? "[" + std::to_string(element) + "]" : "");
                        if (ValueKind(property.type.kind))
                            out.push_back({&property, type.address, offset, name});
                        else if (property.type.kind == ir::PropertyKind::Struct)
                        {
                            const auto child = types_.find(property.type.referencedObject);
                            if (child != types_.end() && child->second->size == property.elementSize)
                                CollectFields(*child->second, offset, name, depth + 1, remaining, path, out);
                        }
                    }
                }
                const auto super = types_.find(type.superAddress);
                if (super != types_.end() && super->second->size <= type.size)
                    CollectFields(*super->second, base, prefix, depth + 1, remaining, path, out);
                path.erase(type.address);
            }

            bool ReadHeader(ir::ValueObservation &value, int32_t size)
            {
                if (size <= 0 || size > 128)
                {
                    value.status = "value-header-size-unsupported";
                    return false;
                }
                value.bytes.resize(static_cast<size_t>(size));
                if (!capture_.IsReadable(value.address, value.bytes.size()) ||
                    !capture_.ReadBytes(value.address, value.bytes.data(), value.bytes.size()).Ok())
                {
                    value.bytes.clear();
                    value.status = "value-header-unreadable";
                    return false;
                }
                return true;
            }

            ir::DelegateBinding ReadBinding(uintptr_t address, const std::vector<uint8_t> &bytes,
                                            int32_t weakOffset, int32_t nameOffset, bool remote) const
            {
                ir::DelegateBinding result;
                result.address = address;
                const auto index = Word(bytes, weakOffset, 4);
                const auto serial = Word(bytes, weakOffset + 4, 4);
                if (!index || !serial)
                {
                    result.targetStatus = "weak-pair-unreadable";
                    return result;
                }
                const uint32_t indexBits = static_cast<uint32_t>(*index);
                const uint32_t serialBits = static_cast<uint32_t>(*serial);
                std::memcpy(&result.objectIndex, &indexBits, sizeof(result.objectIndex));
                std::memcpy(&result.objectSerial, &serialBits, sizeof(result.objectSerial));
                if (nameOffset >= 0)
                {
                    const auto nameAddress = Add(address, static_cast<uint32_t>(nameOffset));
                    const auto name = nameAddress ? objects_.Names().ReadFName(*nameAddress) : std::nullopt;
                    if (!name)
                    {
                        result.targetStatus = "function-name-unreadable";
                        return result;
                    }
                    result.functionName = *name;
                }
                if (remote)
                    result.targetStatus = "remote-object-resolution-not-performed";
                else if (result.objectSerial == 0 && (result.objectIndex == 0 || result.objectIndex == -1))
                    result.targetStatus = "null-weak-pair";
                else if (result.objectIndex < 0 || result.objectIndex >= objects_.Count() || result.objectSerial <= 0)
                    result.targetStatus = "invalid-weak-pair";
                else
                {
                    const auto target = objects_.Objects().ReadObject(result.objectIndex);
                    const auto actualSerial = objects_.Objects().ReadSerialNumber(target);
                    const auto actualIndex = target.IsValid() ? objects_.InternalIndex(target.address) : std::nullopt;
                    if (!actualSerial)
                        result.targetStatus = "object-slot-or-serial-unreadable";
                    else if (*actualSerial != result.objectSerial)
                        result.targetStatus = "stale-serial";
                    else if (!actualIndex || *actualIndex != result.objectIndex)
                        result.targetStatus = "target-index-mismatch";
                    else
                    {
                        result.target = target.address;
                        // serial 匹配只证明采集时的槽位身份，不等于 GC liveness 或可调用性。
                        result.targetStatus = "slot-index-and-serial-match-not-liveness";
                    }
                }
                return result;
            }

            void ReadInvocation(ir::ValueObservation &value, const std::vector<ir::StorageLayoutIR> &layouts)
            {
                if (layouts.empty())
                {
                    value.status = "multicast-source-layout-unselected";
                    return;
                }
                const auto smallest = std::min_element(layouts.begin(), layouts.end(),
                                                       [](const auto &left, const auto &right)
                                                       { return left.size < right.size; });
                if (!ReadHeader(value, smallest->size))
                    return;
                const int32_t offset = Offset(layouts.front(), "InvocationList");
                if (offset < 0 || std::any_of(layouts.begin(), layouts.end(), [&](const auto &layout)
                                              { return Offset(layout, "InvocationList") != offset; }))
                {
                    value.status = "invocation-list-offset-candidate-disagreement";
                    return;
                }
                const auto invocation = Add(value.address, static_cast<uint32_t>(offset));
                const auto array = HeapStorage(ir::PropertyKind::Array, pointerWidth_);
                if (!invocation || !array)
                {
                    value.status = "invocation-list-address-invalid";
                    return;
                }
                // 外层原始字节不因元素候选分歧而丢失，header 地址单独标识
                value.addresses["InvocationListHeader"] = *invocation;
                const bool tail = std::all_of(layouts.begin(), layouts.end(), [](const auto &layout)
                                              { return layout.id.starts_with("inline-delegate-tail-checked"); });
                struct Probe
                {
                    std::string id;
                    int32_t stride;
                    int32_t name;
                    bool remote;
                };
                std::vector<Probe> probes = {{"unicast-local", 8 + schema_.fname.size + (tail ? 12 : 0), 8, false}};
                if (!tail)
                    probes.push_back({"unicast-remote", (16 + schema_.fname.size + 7) / 8 * 8, 16, true});
                // FWeakObjectProperty 的 ElementSize 来自 sizeof(FWeakObjectPtr)，不是空值启发式
                // 没有该证据或证据冲突时保留两个结构族，不能用 Shipping 默认值代替
                if (!tail && weakSizes_.size() == 1)
                {
                    const bool remote = *weakSizes_.begin() == 16;
                    std::erase_if(probes, [remote](const auto &probe)
                                  { return probe.remote != remote; });
                    value.numbers["ReflectedWeakStorageSize"] = *weakSizes_.begin();
                }
                size_t accepted = 0;
                for (const auto &probe : probes)
                {
                    value.numbers[probe.id + ":stride"] = probe.stride;
                    auto list = ue::ReadContainerValue(capture_, *invocation, ir::PropertyKind::Array, *array,
                                                       pointerWidth_, probe.stride);
                    const bool readable = list.status == "allocated-elements-sampled" || list.status == "empty-header-observed";
                    std::vector<ir::DelegateBinding> bindings;
                    bool valid = readable;
                    for (const auto &element : list.elements)
                    {
                        auto binding = ReadBinding(element.address, element.bytes, 0, probe.name, probe.remote);
                        if (binding.targetStatus == "weak-pair-unreadable" || binding.targetStatus == "invalid-weak-pair" ||
                            binding.targetStatus == "function-name-unreadable" || binding.targetStatus == "target-index-mismatch")
                            valid = false;
                        bindings.push_back(std::move(binding));
                    }
                    value.candidateStatuses[probe.id] = valid ? "source-candidate-observed" : list.status + ":binding-check-failed";
                    if (!valid)
                        continue;
                    ++accepted;
                    // 空列表不提供 element stride 证据，保留 header，但不选择 ABI
                    if (list.count == 0)
                    {
                        value.container = std::move(list);
                        value.bindingsComplete = true;
                        value.status = "empty-invocation-header-observed-element-layout-unselected";
                        return;
                    }
                    if (accepted == 1)
                    {
                        value.bindings = std::move(bindings);
                        value.bindingsComplete = list.indicesComplete;
                        value.container = std::move(list);
                        value.status = "invocation-bindings-observed-source-candidate";
                    }
                    else
                    {
                        value.bindings.clear();
                        value.bindingsComplete = false;
                        value.container.reset();
                        value.status = "delegate-element-layout-candidate-disagreement";
                    }
                }
                if (!accepted)
                    value.status = "invocation-header-or-elements-unreadable";
            }

            void ReadValue(ir::ValueObservation &value, const ir::TypeReferenceIR &reference)
            {
                if (reference.containerStorage)
                {
                    const int64_t stride = reference.inner
                                               ? int64_t{reference.inner->elementSize} * reference.inner->arrayDim
                                               : 0;
                    value.container = ue::ReadContainerValue(capture_, value.address, reference.kind,
                                                             *reference.containerStorage, pointerWidth_, stride > 0 && stride <= INT32_MAX ? static_cast<int32_t>(stride) : 0);
                    value.layoutId = reference.containerStorage->layoutId;
                    value.status = value.container->status;
                    return;
                }
                if (!ReadHeader(value, reference.elementSize))
                    return;
                const auto layouts = ue::ValueStorageCandidates(reference, pointerWidth_, schema_.fname.size);
                for (const auto &layout : layouts)
                    value.layoutCandidates.push_back(layout.id);
                if (reference.kind == ir::PropertyKind::MulticastDelegate &&
                    reference.delegateStorage == ir::DelegateStorageKind::InlineMulticast)
                {
                    ReadInvocation(value, layouts);
                    return;
                }
                const auto layout = ue::CommonValueStorage(reference, pointerWidth_, schema_.fname.size);
                if (!layout)
                {
                    value.status = "value-source-layout-unselected-or-alignment-disagreement";
                    return;
                }
                value.layoutId = layout->id;
                if (reference.kind == ir::PropertyKind::Text)
                {
                    const auto data = Word(value.bytes, Offset(*layout, "TextData"), pointerWidth_);
                    const auto flags = Word(value.bytes, Offset(*layout, "Flags"), 4);
                    if (!data || !flags || *data > UINTPTR_MAX)
                    {
                        value.status = "text-header-offset-unresolved";
                        return;
                    }
                    value.addresses["TextData"] = static_cast<uintptr_t>(*data);
                    value.numbers["Flags"] = static_cast<int64_t>(*flags);
                    uintptr_t vtable = 0;
                    if (*data && capture_.Read(static_cast<uintptr_t>(*data), vtable))
                    {
                        value.addresses["TextDataVTable"] = vtable;
                        value.numbers["VTableInModule"] = module_.Contains(vtable);
                        value.status = "text-header-and-vtable-observed-display-cache-unresolved";
                    }
                    else
                        value.status = "text-header-observed-text-data-unreadable";
                }
                else if (reference.delegateStorage == ir::DelegateStorageKind::SparseMulticast)
                {
                    const auto bound = Word(value.bytes, Offset(*layout, "Bound"), 1);
                    if (!bound || *bound > 1)
                        value.status = "invalid-sparse-bound-byte";
                    else
                    {
                        value.numbers["Bound"] = static_cast<int64_t>(*bound);
                        value.status = "sparse-bound-byte-observed-bindings-external";
                    }
                }
                else
                {
                    const int32_t weak = Offset(*layout, "ObjectIndex");
                    const int32_t name = Offset(*layout, "FunctionName");
                    if (weak < 0 || Offset(*layout, "ObjectSerialNumber") != weak + 4 ||
                        (reference.kind == ir::PropertyKind::Delegate && name < 0))
                    {
                        value.status = "weak-or-delegate-offset-candidate-disagreement";
                        return;
                    }
                    const bool remote = std::any_of(layouts.begin(), layouts.end(), [](const auto &candidate)
                                                    { return Offset(candidate, "RemoteObjectId") >= 0; });
                    value.bindings.push_back(ReadBinding(value.address, value.bytes, weak, name, remote));
                    value.bindingsComplete = true;
                    value.status = "weak-or-delegate-fields-observed";
                }
            }

            void InspectSparse(ir::SparseRootEvidence &root)
            {
                if (!root.address)
                    return;
                const auto set = ue::DescribeHeapContainer(ir::PropertyKind::Set, pointerWidth_);
                const auto outer = set ? HeapStorage(ir::PropertyKind::Map, pointerWidth_, pointerWidth_, set->size,
                                                     pointerWidth_, pointerWidth_)
                                       : std::nullopt;
                const auto inner = HeapStorage(ir::PropertyKind::Map, pointerWidth_, schema_.fname.size,
                                               2 * pointerWidth_, 4, pointerWidth_);
                if (!outer || !inner)
                {
                    root.status = "sparse-source-map-layout-unavailable";
                    return;
                }
                capture_.Reset();
                {
                    memory::CaptureObservationScope observing(capture_);
                    root.owners = ue::ReadContainerValue(capture_, root.address, ir::PropertyKind::Map, *outer,
                                                         pointerWidth_, 0, {8, 128});
                    root.enumerationComplete = root.owners->elementsComplete;
                    for (const auto &owner : root.owners->elements)
                    {
                        const auto object = Word(owner.bytes, 0, pointerWidth_);
                        const auto mapAddress = Add(owner.address, static_cast<uint32_t>(outer->valueOffset));
                        if (!object || *object > UINTPTR_MAX || !mapAddress)
                        {
                            root.enumerationComplete = false;
                            continue;
                        }
                        const uintptr_t objectAddress = static_cast<uintptr_t>(*object);
                        const auto index = objects_.InternalIndex(objectAddress);
                        const auto identity = index ? objects_.Objects().ReadObject(*index) : ue::ObjectReadResult{};
                        const auto actualClass = objects_.Class(objectAddress);
                        const auto flags = objects_.Flags(objectAddress);
                        ir::ValueObservation value;
                        value.kind = "SparseOwnerMap";
                        value.objectAddress = objectAddress;
                        value.classAddress = actualClass.value_or(0);
                        value.address = *mapAddress;
                        if (!index || !identity.IsValid() || identity.address != objectAddress || !actualClass ||
                            !flags || (*flags & ue::kRFUnavailableDefinition) != 0)
                            value.status = "sparse-owner-identity-invalid";
                        else
                        {
                            value.container = ue::ReadContainerValue(capture_, *mapAddress, ir::PropertyKind::Map,
                                                                     *inner, pointerWidth_, 0, {8, 128});
                            value.status = value.container->status;
                        }
                        root.enumerationComplete = root.enumerationComplete && value.container && value.container->elementsComplete;
                        if (value.container)
                        {
                            for (const auto &entry : value.container->elements)
                            {
                                ir::ValueObservation delegate;
                                delegate.kind = "SparseDelegate";
                                delegate.objectAddress = objectAddress;
                                delegate.classAddress = value.classAddress;
                                const auto name = objects_.Names().ReadFName(entry.address);
                                const auto pointer = Word(entry.bytes, inner->valueOffset, pointerWidth_);
                                delegate.path = name.value_or("<unreadable>");
                                delegate.address = pointer && *pointer <= UINTPTR_MAX ? static_cast<uintptr_t>(*pointer) : 0;
                                if (!name || !delegate.address)
                                    delegate.status = "sparse-name-or-shared-pointer-unreadable";
                                else
                                {
                                    for (const auto &layout : inlineLayouts_)
                                        delegate.layoutCandidates.push_back(layout.id);
                                    ReadInvocation(delegate, inlineLayouts_);
                                }
                                root.enumerationComplete = root.enumerationComplete && delegate.bindingsComplete;
                                root.delegates.push_back(std::move(delegate));
                            }
                        }
                        root.delegates.push_back(std::move(value));
                    }
                }
                root.capture = Capture(capture_.Validate());
                root.enumerationComplete = root.enumerationComplete && root.capture.observationsStable;
                for (auto &value : root.delegates)
                    value.capture = root.capture;
                root.status = root.owners->status == "empty-header-observed"        ? "empty-root-observed"
                              : root.owners->status == "allocated-elements-sampled" ? "bounded-sparse-owner-maps-observed"
                                                                                    : root.owners->status;
            }

            memory::CaptureMemorySource capture_;
            ue::ObjectModelReader objects_;
            const ue::EngineSchema &schema_;
            int32_t pointerWidth_ = 0;
            const app::ModuleImage &module_;
            std::unordered_map<uintptr_t, const ir::TypeIR *> types_;
            std::vector<ir::StorageLayoutIR> inlineLayouts_;
            std::set<int32_t> weakSizes_;
        };
    } // namespace

    ir::RuntimeEvidence InspectRuntimeValues(memory::IMemorySource &memory, const binding::RuntimeBinding &binding,
                                             const ue::EngineSchema &schema, const app::ModuleImage &module, const ir::ReflectionIR &reflection,
                                             ir::SparseRootEvidence sparseRoot)
    {
        return Inspector(memory, binding, schema, module, reflection).Run(std::move(sparseRoot));
    }
} // namespace anduefker::reflection
