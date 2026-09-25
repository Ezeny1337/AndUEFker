#include "anduefker/ue/ObjectModelReader.hpp"

#include <unordered_set>

namespace anduefker::ue
{
    namespace
    {
        std::optional<uintptr_t> Add(uintptr_t base, int32_t offset)
        {
            if (offset < 0)
                return std::nullopt;
            const uintptr_t value = static_cast<uintptr_t>(offset);
            if (base > UINTPTR_MAX - value)
                return std::nullopt;
            return base + value;
        }
    } // namespace

    ObjectModelReader::ObjectModelReader(const IMemorySource &memory,
                                         const RuntimeBinding &binding,
                                         const EngineSchema &schema)
        : memory_(memory),
          binding_(binding),
          schema_(schema),
          objects_(memory, binding.objectRoot.address, binding.objects, binding.decode),
          names_(memory, binding.nameRoot.address, binding.names, binding.decode, schema.fname, schema.features)
    {
    }

    bool ObjectModelReader::Initialize()
    {
        initialized_ = objects_.Initialize();
        return initialized_;
    }

    std::optional<uintptr_t> ObjectModelReader::ReadPointer(uintptr_t address) const
    {
        uintptr_t value = 0;
        if (!memory_.Read(address, value) || value == 0)
            return std::nullopt;
        return value;
    }

    bool ObjectModelReader::IsReadableObject(uintptr_t object) const
    {
        return object != 0 && memory_.IsReadable(object, sizeof(uintptr_t));
    }

    std::optional<uintptr_t> ObjectModelReader::Class(uintptr_t object) const
    {
        const auto address = Add(object, schema_.uobject.classPointer);
        if (!address)
            return std::nullopt;
        const auto raw = ReadPointer(*address);
        if (!raw)
            return std::nullopt;
        return binding_.decode.objectClass(*raw, *address);
    }

    std::optional<uintptr_t> ObjectModelReader::Outer(uintptr_t object) const
    {
        const auto address = Add(object, schema_.uobject.outer);
        if (!address)
            return std::nullopt;
        uintptr_t raw = 0;
        if (!memory_.Read(*address, raw))
            return std::nullopt;
        const uintptr_t outer = binding_.decode.objectOuter(raw, *address);
        if (outer != 0 && !IsReadableObject(outer))
            return std::nullopt;
        return outer;
    }

    std::optional<int32_t> ObjectModelReader::InternalIndex(uintptr_t object) const
    {
        const auto address = Add(object, schema_.uobject.internalIndex);
        if (!address)
            return std::nullopt;
        int32_t raw = 0;
        if (!memory_.Read(*address, raw))
            return std::nullopt;
        return binding_.decode.objectIndex(raw, *address);
    }

    std::optional<uint32_t> ObjectModelReader::Flags(uintptr_t object) const
    {
        const auto address = Add(object, schema_.uobject.flags);
        if (!address)
            return std::nullopt;
        uint32_t raw = 0;
        if (!memory_.Read(*address, raw))
            return std::nullopt;
        return raw;
    }

    std::optional<std::string> ObjectModelReader::NameField(uintptr_t object) const
    {
        const auto address = Add(object, schema_.uobject.name);
        if (!address)
            return std::nullopt;
        return names_.ReadFName(*address);
    }

    std::optional<std::string> ObjectModelReader::Name(uintptr_t object) const
    {
        return NameField(object);
    }

    std::optional<std::string> ObjectModelReader::ClassName(uintptr_t object) const
    {
        const auto classAddress = Class(object);
        if (!classAddress)
            return std::nullopt;
        return NameField(*classAddress);
    }

    std::optional<std::string> ObjectModelReader::FullName(uintptr_t object, size_t maxDepth) const
    {
        const auto name = Name(object);
        const auto className = ClassName(object);
        if (!name || !className)
            return std::nullopt;

        std::vector<std::string> outers;
        std::unordered_set<uintptr_t> visited{object};
        uintptr_t current = object;
        for (size_t depth = 0; depth < maxDepth; ++depth)
        {
            const auto outer = Outer(current);
            if (!outer)
                return std::nullopt;
            if (*outer == 0)
                break;
            if (!visited.insert(*outer).second)
                return std::nullopt;
            const auto outerName = Name(*outer);
            if (!outerName)
                return std::nullopt;
            outers.push_back(*outerName);
            current = *outer;
        }
        if (outers.size() == maxDepth)
            return std::nullopt;

        std::string result = *className + " ";
        for (auto it = outers.rbegin(); it != outers.rend(); ++it)
            result += *it + ".";
        result += *name;
        return result;
    }

    std::optional<ObjectMetadata> ObjectModelReader::Metadata(uintptr_t object) const
    {
        const auto classAddress = Class(object);
        const auto outerAddress = Outer(object);
        const auto index = InternalIndex(object);
        const auto flags = Flags(object);
        const auto name = Name(object);
        const auto className = ClassName(object);
        const auto fullName = FullName(object);
        if (!classAddress || !outerAddress || !index || !flags || !name || !className || !fullName)
            return std::nullopt;
        return ObjectMetadata{object, *classAddress, *outerAddress, *index, *flags, *name, *className, *fullName};
    }

    std::optional<FieldMetadata> ObjectModelReader::Field(uintptr_t field) const
    {
        if (!IsReadableObject(field))
            return std::nullopt;

        if (!schema_.features.useFProperty)
            return UField(field);

        const auto classAddress = Add(field, schema_.ffield.classPointer);
        const auto ownerAddress = Add(field, schema_.ffield.owner);
        const auto nextAddress = Add(field, schema_.ffield.next);
        const auto nameAddress = Add(field, schema_.ffield.name);
        if (!classAddress || !ownerAddress || !nextAddress || !nameAddress)
            return std::nullopt;

        const auto classValue = ReadPointer(*classAddress);
        auto ownerValue = ReadPointer(*ownerAddress);
        const auto name = names_.ReadFName(*nameAddress);
        uintptr_t nextValue = 0;
        if (!classValue || !ownerValue || !memory_.Read(*nextAddress, nextValue) || !name)
            return std::nullopt;
        if (schema_.features.fFieldOwnerMask)
            *ownerValue &= ~static_cast<uintptr_t>(1);
        const auto classNameAddress = Add(*classValue, schema_.ffieldClass.name);
        if (!classNameAddress)
            return std::nullopt;
        const auto className = names_.ReadFName(*classNameAddress);
        if (!className)
            return std::nullopt;
        return FieldMetadata{field, *classValue, *ownerValue, nextValue, *name, *className};
    }

    std::optional<FieldMetadata> ObjectModelReader::UField(uintptr_t field) const
    {
        if (!IsReadableObject(field))
            return std::nullopt;

        const auto classAddress = Class(field);
        const auto nextAddress = Add(field, schema_.ufield.next);
        const auto nameAddress = Add(field, schema_.uobject.name);
        if (!classAddress || !nextAddress || !nameAddress)
            return std::nullopt;

        uintptr_t nextValue = 0;
        if (!memory_.Read(*nextAddress, nextValue))
            return std::nullopt;
        const auto name = names_.ReadFName(*nameAddress);
        if (!name)
            return std::nullopt;
        const auto className = NameField(*classAddress);
        if (!className)
            return std::nullopt;
        return FieldMetadata{field, *classAddress, 0, nextValue, *name, *className};
    }

    std::vector<FieldMetadata> ObjectModelReader::Fields(uintptr_t first, size_t maxFields) const
    {
        std::vector<FieldMetadata> result;
        std::unordered_set<uintptr_t> visited;
        uintptr_t current = first;
        while (current != 0 && result.size() < maxFields && visited.insert(current).second)
        {
            const auto field = Field(current);
            if (!field)
                break;
            result.push_back(*field);
            current = field->nextAddress;
        }
        return result;
    }

    std::optional<PropertyMetadata> ObjectModelReader::Property(uintptr_t field) const
    {
        const auto base = Field(field);
        if (!base)
            return std::nullopt;

        const auto arrayDimAddress = Add(field, schema_.property.arrayDim);
        const auto elementSizeAddress = Add(field, schema_.property.elementSize);
        const auto offsetAddress = Add(field, schema_.property.offsetInternal);
        const auto flagsAddress = Add(field, schema_.property.propertyFlags);
        if (!arrayDimAddress || !elementSizeAddress || !offsetAddress || !flagsAddress)
            return std::nullopt;

        PropertyMetadata result;
        static_cast<FieldMetadata &>(result) = *base;
        if (!memory_.Read(*arrayDimAddress, result.arrayDim) ||
            !memory_.Read(*elementSizeAddress, result.elementSize) ||
            !memory_.Read(*offsetAddress, result.offset) ||
            !memory_.Read(*flagsAddress, result.flags))
            return std::nullopt;

        const auto readOptionalPointer = [&](int32_t offset) -> uintptr_t
        {
            if (offset < 0)
                return 0;
            const auto address = Add(field, offset);
            uintptr_t value = 0;
            return address && memory_.Read(*address, value) ? value : 0;
        };
        if (base->className == "ObjectProperty" || base->className == "ObjectPropertyBase" ||
            base->className == "SoftObjectProperty" || base->className == "WeakObjectProperty" ||
            base->className == "LazyObjectProperty" || base->className == "InterfaceProperty")
            result.referencedAddress = readOptionalPointer(schema_.propertySubtypes.objectClass);
        else if (base->className == "ClassProperty" || base->className == "SoftClassProperty")
            result.referencedAddress = readOptionalPointer(schema_.propertySubtypes.classMetaClass);
        else if (base->className == "StructProperty")
            result.referencedAddress = readOptionalPointer(schema_.propertySubtypes.structType);
        else if (base->className == "ByteProperty")
            result.referencedAddress = readOptionalPointer(schema_.propertySubtypes.byteEnum);
        else if (base->className == "ArrayProperty")
            result.referencedAddress = readOptionalPointer(schema_.propertySubtypes.arrayInner);
        else if (base->className == "SetProperty")
            result.referencedAddress = readOptionalPointer(schema_.propertySubtypes.setElement);
        else if (base->className == "MapProperty")
        {
            result.referencedAddress = readOptionalPointer(schema_.propertySubtypes.mapBase);
            const int32_t valueOffset = schema_.propertySubtypes.mapBase >= 0
                                            ? schema_.propertySubtypes.mapBase + static_cast<int32_t>(sizeof(uintptr_t))
                                            : -1;
            result.secondaryAddress = readOptionalPointer(valueOffset);
        }
        else if (base->className == "EnumProperty")
        {
            result.referencedAddress = readOptionalPointer(schema_.propertySubtypes.enumBase);
            const int32_t enumOffset = schema_.propertySubtypes.enumBase >= 0
                                           ? schema_.propertySubtypes.enumBase + static_cast<int32_t>(sizeof(uintptr_t))
                                           : -1;
            result.secondaryAddress = readOptionalPointer(enumOffset);
        }
        else if (base->className == "DelegateProperty" || base->className == "MulticastDelegateProperty" ||
                 base->className == "MulticastInlineDelegateProperty" || base->className == "MulticastSparseDelegateProperty")
            result.referencedAddress = readOptionalPointer(schema_.propertySubtypes.delegateSignature);

        return result;
    }

    std::optional<uintptr_t> ObjectModelReader::StructChildren(uintptr_t structure) const
    {
        const auto address = Add(structure, schema_.ustruct.children);
        return address ? ReadPointer(*address) : std::nullopt;
    }

    std::optional<uintptr_t> ObjectModelReader::StructProperties(uintptr_t structure) const
    {
        const auto address = Add(structure, schema_.features.useFProperty ? schema_.ustruct.childProperties : schema_.ustruct.children);
        return address ? ReadPointer(*address) : std::nullopt;
    }

    std::optional<uintptr_t> ObjectModelReader::StructSuper(uintptr_t structure) const
    {
        const auto address = Add(structure, schema_.ustruct.superStruct);
        return address ? ReadPointer(*address) : std::nullopt;
    }

    std::optional<int32_t> ObjectModelReader::StructSize(uintptr_t structure) const
    {
        const auto address = Add(structure, schema_.ustruct.size);
        if (!address)
            return std::nullopt;
        int32_t value = 0;
        return memory_.Read(*address, value) ? std::optional<int32_t>(value) : std::nullopt;
    }

    std::vector<EnumValueMetadata> ObjectModelReader::EnumValues(uintptr_t enumeration, size_t maxValues) const
    {
        std::vector<EnumValueMetadata> result;
        if (enumeration == 0 || schema_.uenum.names < 0 || maxValues == 0)
            return result;

        const auto dataAddress = Add(enumeration, schema_.uenum.names);
        if (!dataAddress)
            return result;

        uintptr_t data = 0;
        int32_t count = 0;
        int32_t capacity = 0;
        const auto countAddress = Add(*dataAddress, static_cast<int32_t>(sizeof(uintptr_t)));
        const auto capacityAddress = Add(*dataAddress, static_cast<int32_t>(sizeof(uintptr_t) + sizeof(int32_t)));
        if (!memory_.Read(*dataAddress, data) || !countAddress || !capacityAddress ||
            !memory_.Read(*countAddress, count) || !memory_.Read(*capacityAddress, capacity))
            return result;
        if (count < 0 || count > 0x100000 || capacity < count || (count > 0 && !memory_.IsReadable(data, sizeof(uintptr_t))))
            return result;

        const int32_t nameSize = schema_.fname.size > 0 ? schema_.fname.size : static_cast<int32_t>(sizeof(uintptr_t));
        const int32_t valueOffset = (nameSize + static_cast<int32_t>(alignof(int64_t)) - 1) /
                                    static_cast<int32_t>(alignof(int64_t)) * static_cast<int32_t>(alignof(int64_t));
        const int32_t stride = valueOffset + static_cast<int32_t>(sizeof(int64_t));
        for (int32_t index = 0; index < count && result.size() < maxValues; ++index)
        {
            const auto entry = Add(data, index * stride);
            if (!entry)
                break;
            const auto name = names_.ReadFName(*entry);
            int64_t value = 0;
            const auto valueAddress = Add(*entry, valueOffset);
            if (!name || !valueAddress || !memory_.Read(*valueAddress, value))
                break;
            result.push_back(EnumValueMetadata{*name, value});
        }
        return result;
    }
} // namespace anduefker::ue
