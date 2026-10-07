#include "anduefker/ue/SchemaCatalog.hpp"

namespace anduefker::ue
{
    std::vector<EngineProfile> SchemaCatalog::Profiles()
    {
        std::vector<EngineProfile> result = {
            {"ue4-uproperty", "UE4 UProperty", "4.23-4.24", ParseEngineVersion("4.24.0"),
             EngineFamily::UE4UProperty, FeaturesForLayout(SchemaLayoutVariant::UProperty),
             SchemaLayoutVariant::UProperty},
            {"ue4-fproperty", "UE4 FProperty", "4.25-4.27", ParseEngineVersion("4.25.0"),
             EngineFamily::UE4FProperty, FeaturesForLayout(SchemaLayoutVariant::FProperty),
             SchemaLayoutVariant::FProperty},
            {"ue4-fproperty-enumflags", "UE4 FProperty with enum flags", "4.26-4.27", ParseEngineVersion("4.26.0"),
             EngineFamily::UE4FProperty, FeaturesForLayout(SchemaLayoutVariant::FPropertyEnumFlags),
             SchemaLayoutVariant::FPropertyEnumFlags},
            {"ue5-ffield-explicit", "UE5 FField explicit owner", "5.0", ParseEngineVersion("5.0.0"),
             EngineFamily::UE5FProperty, FeaturesForLayout(SchemaLayoutVariant::FFieldExplicit),
             SchemaLayoutVariant::FFieldExplicit},
            {"ue5-ffield-explicit-package", "UE5 FField explicit owner with enum package", "5.1-5.2",
             ParseEngineVersion("5.2.0"), EngineFamily::UE5FProperty,
             FeaturesForLayout(SchemaLayoutVariant::FFieldExplicitPackage), SchemaLayoutVariant::FFieldExplicitPackage},
            {"ue5-ffield-tagged", "UE5 FField tagged owner", "5.3-5.4", ParseEngineVersion("5.3.0"),
             EngineFamily::UE5FProperty, FeaturesForLayout(SchemaLayoutVariant::FFieldTagged),
             SchemaLayoutVariant::FFieldTagged},
            {"ue5-modern", "UE5 modern reflection", "5.5-5.6", ParseEngineVersion("5.6.0"),
             EngineFamily::UE5FProperty, FeaturesForLayout(SchemaLayoutVariant::FFieldTaggedModern),
             SchemaLayoutVariant::FFieldTaggedModern},
        };
        for (auto &profile : result)
        {
            profile.optionalPropertyAvailable = profile.layout == SchemaLayoutVariant::FFieldTagged ||
                                                profile.layout == SchemaLayoutVariant::FFieldTaggedModern;
        }
        return result;
    }

    SchemaLayoutVariant SchemaCatalog::LayoutFor(const EngineVersion &version)
    {
        if (!version.IsValid())
            return SchemaLayoutVariant::Unknown;
        if (version.major < 5)
        {
            if (version.minor < 25)
                return SchemaLayoutVariant::UProperty;
            return version.minor >= 26 ? SchemaLayoutVariant::FPropertyEnumFlags : SchemaLayoutVariant::FProperty;
        }
        if (version.minor == 0)
            return SchemaLayoutVariant::FFieldExplicit;
        if (version.minor <= 2)
            return SchemaLayoutVariant::FFieldExplicitPackage;
        if (version.minor <= 4)
            return SchemaLayoutVariant::FFieldTagged;
        return SchemaLayoutVariant::FFieldTaggedModern;
    }

    EngineFamily SchemaCatalog::FamilyFor(const EngineVersion &version)
    {
        if (!version.IsValid())
            return EngineFamily::Unknown;
        if (version.major < 5)
            return version.minor >= 25 ? EngineFamily::UE4FProperty : EngineFamily::UE4UProperty;
        return EngineFamily::UE5FProperty;
    }

    EngineFeatures SchemaCatalog::FeaturesFor(const EngineVersion &version)
    {
        return FeaturesForLayout(LayoutFor(version));
    }

    EngineFeatures SchemaCatalog::FeaturesForLayout(SchemaLayoutVariant layout)
    {
        EngineFeatures result;
        result.useNamePool = true;
        result.enumUsesFNameData = false;
        // 在源码的运行时布局中，反射出的 FProperty/UProperty ArrayDim 字段仍保持为 int32 类型
        // 生成参数描述符中的 byte-sized ArrayDim 字段属于另一种不同的契约
        result.arrayDimIsByte = false;

        // 这些是运行时观测到的选项
        // 仅提供初始探测顺序，切勿将其视作已 Cook 或已修改二进制文件的证据
        result.casePreservingName = false;
        result.outlineNumberName = false;
        result.fnameDisplayLayout = FNameDisplayLayout::None;

        switch (layout)
        {
        case SchemaLayoutVariant::UProperty:
            result.fnameDisplayLayout = FNameDisplayLayout::BeforeNumber;
            result.propertyTailLayout = PropertyTailLayout::UProperty;
            break;
        case SchemaLayoutVariant::FProperty:
            result.useFProperty = true;
            result.fnameDisplayLayout = FNameDisplayLayout::BeforeNumber;
            result.propertyTailLayout = PropertyTailLayout::RepNotifyBeforeLinks;
            break;
        case SchemaLayoutVariant::FPropertyEnumFlags:
            result.useFProperty = true;
            result.fnameDisplayLayout = FNameDisplayLayout::BeforeNumber;
            result.enumHasFlags = true;
            result.enumFlagsRequired = true;
            result.enumTailLayout = EnumTailLayout::Flags;
            result.propertyTailLayout = PropertyTailLayout::RepNotifyBeforeLinks;
            break;
        case SchemaLayoutVariant::FFieldExplicit:
            result.useFProperty = true;
            result.fnameDisplayLayout = FNameDisplayLayout::BeforeNumber;
            result.largeWorldCoordinates = true;
            result.enumHasFlags = true;
            result.enumFlagsRequired = true;
            result.enumTailLayout = EnumTailLayout::Flags;
            result.propertyTailLayout = PropertyTailLayout::RepNotifyBeforeLinks;
            break;
        case SchemaLayoutVariant::FFieldExplicitPackage:
            result.useFProperty = true;
            result.largeWorldCoordinates = true;
            result.enumHasFlags = true;
            result.enumFlagsRequired = true;
            result.enumHasPackage = true;
            result.enumTailLayout = EnumTailLayout::FlagsDisplayNamePackage;
            result.fnameDisplayLayout = FNameDisplayLayout::AfterNumber;
            result.propertyTailLayout = PropertyTailLayout::RepNotifyBeforeLinks;
            break;
        case SchemaLayoutVariant::FFieldTagged:
            result.useFProperty = true;
            result.largeWorldCoordinates = true;
            result.enumHasFlags = true;
            result.enumFlagsRequired = true;
            result.enumFlagsIsByte = true;
            result.enumHasPackage = true;
            result.enumTailLayout = EnumTailLayout::FlagsDisplayNamePackage;
            result.fFieldOwnerEncoding = FFieldOwnerEncoding::TaggedPointer;
            result.functionDefaultsContinueAfterInitializer = true;
            result.fnameDisplayLayout = FNameDisplayLayout::AfterNumber;
            result.propertyTailLayout = PropertyTailLayout::LinksBeforeRepNotify;
            break;
        case SchemaLayoutVariant::FFieldTaggedModern:
            result.useFProperty = true;
            result.largeWorldCoordinates = true;
            result.enumHasFlags = true;
            result.enumFlagsRequired = true;
            result.enumCppFormIsByte = true;
            result.enumFlagsIsByte = true;
            result.enumHasPackage = true;
            result.enumTailLayout = EnumTailLayout::FlagsPackageDisplayName;
            result.fFieldOwnerEncoding = FFieldOwnerEncoding::TaggedPointer;
            result.functionDefaultsContinueAfterInitializer = true;
            result.objectArrayMayPackItem = true;
            result.fnameDisplayLayout = FNameDisplayLayout::AfterNumber;
            result.propertyTailLayout = PropertyTailLayout::LinksBeforeRepNotify;
            break;
        case SchemaLayoutVariant::Unknown:
            result = EngineFeatures{};
            break;
        }
        return result;
    }
} // namespace anduefker::ue
