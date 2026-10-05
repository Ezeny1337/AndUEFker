#include "anduefker/ue/EngineSchema.hpp"

#include <algorithm>
#include <iterator>

namespace anduefker::ue
{
    std::string NormalizeRuntimeFieldName(std::string_view name)
    {
        if (name.size() > 1 && name.starts_with("UInt"))
            return std::string(name);
        if (name.size() > 1 && (name.front() == 'U' || name.front() == 'F') &&
            name[1] >= 'A' && name[1] <= 'Z')
            return std::string(name.substr(1));
        return std::string(name);
    }

    FieldKind FieldKindFromRuntimeName(std::string_view name, bool useFProperty)
    {
        const std::string normalized = NormalizeRuntimeFieldName(name);
        if (normalized == "Function" || normalized == "DelegateFunction" ||
            normalized == "SparseDelegateFunction" || normalized == "VerseFunction")
            return FieldKind::UFunction;

        static constexpr std::string_view propertyNames[] = {
            "Property", "BoolProperty", "ByteProperty", "Int8Property", "Int16Property", "IntProperty",
            "Int32Property", "Int64Property", "UInt16Property", "UInt32Property", "UInt64Property",
            "FloatProperty", "DoubleProperty", "NameProperty", "StrProperty", "Utf8StrProperty",
            "AnsiStrProperty", "TextProperty", "ObjectProperty", "ObjectPropertyBase", "ObjectPtrProperty",
            "SoftObjectProperty", "WeakObjectProperty", "LazyObjectProperty", "ClassProperty",
            "SoftClassProperty", "StructProperty", "EnumProperty", "ArrayProperty", "SetProperty",
            "MapProperty", "InterfaceProperty", "DelegateProperty", "MulticastDelegateProperty",
            "MulticastInlineDelegateProperty", "MulticastSparseDelegateProperty", "FieldPathProperty",
            "OptionalProperty"};
        if (std::find(std::begin(propertyNames), std::end(propertyNames), normalized) != std::end(propertyNames))
            return useFProperty ? FieldKind::FProperty : FieldKind::UProperty;

        return useFProperty ? FieldKind::FField : FieldKind::UField;
    }

    bool IsFunctionFieldKind(FieldKind kind)
    {
        return kind == FieldKind::UFunction;
    }

    bool IsPropertyFieldKind(FieldKind kind)
    {
        return kind == FieldKind::UProperty || kind == FieldKind::FProperty;
    }

    const char *SchemaLayoutVariantName(SchemaLayoutVariant variant)
    {
        switch (variant)
        {
        case SchemaLayoutVariant::UProperty:
            return "uproperty";
        case SchemaLayoutVariant::FProperty:
            return "fproperty";
        case SchemaLayoutVariant::FPropertyEnumFlags:
            return "fproperty-enumflags";
        case SchemaLayoutVariant::FFieldExplicit:
            return "ffield-explicit";
        case SchemaLayoutVariant::FFieldExplicitPackage:
            return "ffield-explicit-package";
        case SchemaLayoutVariant::FFieldTagged:
            return "ffield-tagged";
        case SchemaLayoutVariant::FFieldTaggedModern:
            return "ffield-tagged-modern";
        case SchemaLayoutVariant::Unknown:
            return "unknown";
        }
        return "unknown";
    }

    FNamePhysicalLayout GetFNamePhysicalLayout(const FNameSchema &schema)
    {
        return FNamePhysicalLayout{schema.size,
                                   schema.comparisonIndex,
                                   schema.number,
                                   schema.displayIndex,
                                   schema.numberLayout};
    }

    bool EngineSchema::HasSameReflectionLayout(const EngineSchema &other) const
    {
        return features == other.features && fname == other.fname && uobject == other.uobject &&
               ufield == other.ufield && ffield == other.ffield && ffieldClass == other.ffieldClass &&
               ustruct == other.ustruct && uclass == other.uclass && ufunction == other.ufunction &&
               uenum == other.uenum && property == other.property && propertySubtypes == other.propertySubtypes;
    }

    bool EngineSchema::IsReadyForReflection() const
    {
        const bool base = validation.uobject && validation.fname && validation.structs;
        if (!base)
            return false;
        if (uobject.internalIndex < 0 || uobject.classPointer < 0 || uobject.name < 0 ||
            uobject.outer < 0 || uobject.flags < 0)
            return false;
        if (ustruct.superStruct < 0 || ustruct.children < 0 || ustruct.propertiesSizeOffset < 0)
            return false;
        if (features.useFProperty && (ustruct.childProperties < 0 || !validation.fields ||
                                      ffield.classPointer < 0 || ffield.next < 0 || ffield.name < 0 ||
                                      ffieldClass.name < 0 || ffieldClass.id < 0 ||
                                      ffieldClass.castFlags < 0 || ffieldClass.superClass < 0))
            return false;
        if (ufield.next < 0 || property.arrayDim < 0 || property.elementSize < 0 ||
            property.propertyFlags < 0 || property.offsetInternal < 0 ||
            ufunction.functionFlags < 0 || ufunction.numParams < 0 || ufunction.paramSize < 0 ||
            ufunction.returnValueOffset < 0 || ufunction.nativeFunction < 0 || uenum.names < 0)
            return false;
        if (features.enumFlagsRequired && uenum.flags < 0)
            return false;
        if (features.enumHasPackage && uenum.enumPackage < 0)
            return false;
        return validation.properties && validation.functions && validation.enums;
    }
} // namespace anduefker::ue
