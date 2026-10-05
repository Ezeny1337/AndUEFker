#include "anduefker/ue/SchemaResolver.hpp"
#include "ProbeContext.hpp"

#include <algorithm>

namespace anduefker::ue
{
    namespace
    {
        const char *EnumTailLayoutName(EnumTailLayout layout)
        {
            switch (layout)
            {
            case EnumTailLayout::Legacy:
                return "legacy";
            case EnumTailLayout::Flags:
                return "flags";
            case EnumTailLayout::FlagsDisplayNamePackage:
                return "flags-display-package";
            case EnumTailLayout::FlagsPackageDisplayName:
                return "flags-package-display";
            }
            return "unknown";
        }
    } // namespace

    std::shared_ptr<const SchemaProbeBootstrap> CreateSchemaProbeBootstrap(const IMemorySource &memory,
                                                                           const RuntimeBinding &binding,
                                                                           size_t maxSamples)
    {
        auto result = std::make_shared<SchemaProbeBootstrap>();
        auto objects = std::make_shared<ObjectStoreReader>(memory,
                                                           binding.objectRoot.address,
                                                           binding.objects,
                                                           binding.decode);
        if (!objects->Initialize())
        {
            result->failure = "object store could not be initialized for schema bootstrap";
            return result;
        }

        result->objectSamples.reserve(std::min<size_t>(maxSamples, static_cast<size_t>(objects->Count())));
        for (int32_t index = 0; index < objects->Count() && result->objectSamples.size() < maxSamples; ++index)
        {
            const auto object = objects->ReadObject(index);
            if (object.IsValid())
                result->objectSamples.emplace_back(index, object.address);
        }
        if (result->objectSamples.size() < 2)
        {
            result->failure = "not enough live UObject samples for schema bootstrap";
            return result;
        }

        result->objects = std::move(objects);
        return result;
    }

    SchemaResolver::SchemaResolver(const IMemorySource &memory,
                                   const RuntimeBinding &binding,
                                   const EngineProfile &profile,
                                   SchemaProbeNames names,
                                   uintptr_t moduleBase,
                                   uintptr_t moduleEnd,
                                   std::shared_ptr<const SchemaProbeBootstrap> bootstrap)
        : memory_(memory),
          binding_(binding),
          profile_(profile),
          names_(std::move(names)),
          moduleBase_(moduleBase),
          moduleEnd_(moduleEnd),
          bootstrap_(bootstrap ? std::move(bootstrap) : CreateSchemaProbeBootstrap(memory, binding))
    {
    }

    SchemaResolutionReport SchemaResolver::Resolve(EngineSchema &schema) const
    {
        SchemaResolutionReport report;
        const uint64_t addressSpaceGeneration = memory_.AddressSpaceGeneration();
        report.profileId = profile_.id;
        report.profileLabel = profile_.label;
        schema.family = profile_.family;
        schema.layout = profile_.layout;
        schema.features = profile_.features;
        const schema_probe::SchemaProbeContext probe{memory_, binding_, profile_, names_,
                                                     moduleBase_, moduleEnd_, bootstrap_};
        report.evidence.push_back("engine structure profile=" + profile_.id +
                                  " version_range=" + profile_.versionRange +
                                  " use_fproperty=" + std::to_string(schema.features.useFProperty) +
                                  " use_name_pool=" + std::to_string(schema.features.useNamePool) +
                                  " ffield_owner_encoding=" +
                                  std::to_string(schema.features.fFieldOwnerEncoding == FFieldOwnerEncoding::TaggedPointer) +
                                  " large_world_coordinates=" + std::to_string(schema.features.largeWorldCoordinates) +
                                  " layout=" + SchemaLayoutVariantName(schema.layout) +
                                  " enum_tail=" + EnumTailLayoutName(schema.features.enumTailLayout) +
                                  " enum_cpp_form_byte=" + std::to_string(schema.features.enumCppFormIsByte) +
                                  " enum_flags_required=" + std::to_string(schema.features.enumFlagsRequired));
        if (!profile_.IsValid())
        {
            report.failureStage = "profile";
            report.failures.push_back("engine structure profile is invalid");
            return report;
        }

        report.failureStage = "uobject";
        if (!probe.ResolveUObjectSchema(schema, report) || !probe.ValidateUObjectSchema(schema, report))
            return report;
        report.failureStage = "struct";
        if (!probe.ResolveStructSchema(schema, report))
            return report;
        report.failureStage = "field";
        if (!probe.ResolveFieldSchema(schema, report))
            return report;
        report.failureStage = "property";
        if (!probe.ResolvePropertySchema(schema, report) || !probe.ResolvePropertySubtypes(schema, report))
            return report;
        report.failureStage = "function";
        if (!probe.ResolveFunctionSchema(schema, report))
            return report;
        report.failureStage = "enum";
        if (!probe.ResolveEnumSchema(schema, report))
            return report;

        schema.validation.uobject = true;
        schema.validation.fname = binding_.names.IsValid();
        if (schema.features.useFProperty)
            schema.validation.fields = true;
        schema.validation.profileId = profile_.id;
        schema.validation.profileLabel = profile_.label;
        schema.validation.profileVersionRange = profile_.versionRange;
        schema.validation.familyEvidence = profile_.versionRange;
        if (memory_.AddressSpaceGeneration() != addressSpaceGeneration)
        {
            report.addressSpaceChanged = true;
            report.failureStage = "address-space-generation";
            report.failures.push_back("remote address-space generation changed during schema resolution");
            return report;
        }
        if (!schema.validation.properties || !schema.validation.functions || !schema.validation.enums)
        {
            report.failures.push_back("one or more schema semantic probes failed");
            return report;
        }
        report.accepted = schema.IsReadyForReflection();
        if (report.accepted)
        {
            report.score = 100;
            if (schema.uobject.outer == schema.uobject.name + schema.fname.size)
                report.score += 10;
            if (schema.ufield.next == schema.uobject.outer + static_cast<int32_t>(sizeof(uintptr_t)))
                report.score += 10;
            if (schema.uenum.names >= 0 && schema.uenum.cppForm >= 0)
                report.score += 5;
            if (schema.features.enumFlagsRequired && schema.uenum.flags >= 0)
                report.score += 5;
            if (schema.features.enumHasPackage && schema.uenum.enumPackage >= 0)
                report.score += 5;
        }
        return report;
    }
} // namespace anduefker::ue
