#include "anduefker/reflection/RuntimeInspector.hpp"
#include "RuntimeFieldPlanner.hpp"
#include "RuntimeReadSupport.hpp"
#include "RuntimeValueReader.hpp"

namespace anduefker::reflection
{
    namespace
    {
        using detail::Add;
        using detail::Capture;
        using detail::RuntimeFieldPlanner;
        using detail::RuntimeSampleCategory;
        using detail::RuntimeValueKind;
        using detail::RuntimeValueReader;
        using Limits = ir::RuntimeEvidenceLimits;
        static_assert(Limits::kArrayInstances + Limits::kArrayDefaults +
                          8 * (Limits::kOtherInstances + Limits::kOtherDefaults) <=
                      Limits::kMaxValues);

        // 编排对象身份核对、采样与稳定性验证，字段规划和具体值解码各自独立
        class Inspector
        {
        public:
            Inspector(memory::IMemorySource &memory, const binding::RuntimeBinding &binding,
                      const ue::EngineSchema &schema, const module::ModuleImage &module, const ir::ReflectionIR &reflection)
                : capture_(memory), objects_(capture_, binding, schema), pointerWidth_(module.pointerWidth),
                  fields_(reflection), values_(capture_, objects_, schema, module, reflection) {}

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
                const int32_t limit = std::min(result.objectSlots, Limits::kMaxObjectSlots);
                std::map<std::pair<uintptr_t, std::string>, std::pair<size_t, size_t>> samples;
                std::map<std::pair<std::string, bool>, size_t> categories;
                for (int32_t index = 0; index < limit; ++index)
                {
                    if (result.values.size() >= Limits::kMaxValues)
                    {
                        ++result.skipped["global-value-budget"];
                        break;
                    }
                    ++result.scannedSlots;
                    const auto object = objects_.Objects().ReadObject(index, true);
                    if (!object.IsValid())
                        continue;
                    const auto classAddress = objects_.Class(object.address, true);
                    const auto *type = classAddress ? fields_.Type(*classAddress) : nullptr;
                    if (!type || type->kind != ir::TypeKind::Class)
                        continue;
                    const auto flags = objects_.Flags(object.address);
                    if (!flags || (*flags & ue::kRFUnavailableDefinition) != 0)
                    {
                        ++result.skipped["object-not-ready"];
                        continue;
                    }
                    const bool cdo = (*flags & ue::kRFClassDefaultObject) != 0;
                    for (const auto &field : fields_.Fields(*classAddress))
                    {
                        if (result.values.size() >= Limits::kMaxValues)
                            break;
                        const char *kind = RuntimeValueKind(field.property->type.kind);
                        auto &count = samples[{field.property->address, field.path}];
                        auto &sampled = cdo ? count.second : count.first;
                        const auto category = RuntimeSampleCategory(field.property->type);
                        const bool array = field.property->type.kind == ir::PropertyKind::Array;
                        const size_t categoryLimit = array
                                                         ? (cdo ? Limits::kArrayDefaults : Limits::kArrayInstances)
                                                         : (cdo ? Limits::kOtherDefaults : Limits::kOtherInstances);
                        auto &categoryCount = categories[{category, cdo}];
                        if (sampled >= (cdo ? 1u : 2u) || categoryCount >= categoryLimit)
                        {
                            ++result.skipped["sample-budget:" + category + (cdo ? ":class-default" : ":instance")];
                            continue;
                        }
                        ++sampled;
                        ++categoryCount;
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
                                values_.ReadValue(value, field.property->type);
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
                for (const auto &[reason, count] : fields_.Skipped())
                    result.skipped[reason] += count;
                values_.ReadSparse(result.sparse);
                result.status = "bounded-observations-collected";
                return result;
            }

        private:
            memory::CaptureMemorySource capture_;
            ue::ObjectModelReader objects_;
            int32_t pointerWidth_ = 0;
            RuntimeFieldPlanner fields_;
            RuntimeValueReader values_;
        };
    } // namespace

    ir::RuntimeEvidence InspectRuntimeValues(memory::IMemorySource &memory, const binding::RuntimeBinding &binding,
                                             const ue::EngineSchema &schema, const module::ModuleImage &module, const ir::ReflectionIR &reflection,
                                             ir::SparseRootEvidence sparseRoot)
    {
        return Inspector(memory, binding, schema, module, reflection).Run(std::move(sparseRoot));
    }
} // namespace anduefker::reflection
