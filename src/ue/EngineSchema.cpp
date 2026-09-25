#include "anduefker/ue/EngineSchema.hpp"

namespace anduefker::ue
{
    bool EngineSchema::IsReadyForReflection() const
    {
        const bool base = validation.uobject && validation.fname && validation.structs;
        if (!base)
            return false;
        if (uobject.internalIndex < 0 || uobject.classPointer < 0 || uobject.name < 0 || uobject.flags < 0)
            return false;
        if (ustruct.superStruct < 0 || ustruct.children < 0 || ustruct.size < 0)
            return false;
        if (features.useFProperty && (ustruct.childProperties < 0 || !validation.fields ||
                                      ffield.classPointer < 0 || ffield.next < 0 || ffield.name < 0 ||
                                      ffieldClass.name < 0))
            return false;
        if (ufield.next < 0 || property.arrayDim < 0 || property.elementSize < 0 ||
            property.propertyFlags < 0 || property.offsetInternal < 0 ||
            ufunction.functionFlags < 0 || ufunction.nativeFunction < 0 || uenum.names < 0)
            return false;
        return validation.properties && validation.functions && validation.enums;
    }
} // namespace anduefker::ue
