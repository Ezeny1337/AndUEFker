#include "anduefker/binding/CommonObjectCollector.hpp"

namespace anduefker::binding
{
    CommonObjectCollector::CommonObjectCollector(const IMemorySource &memory,
                                                 const RuntimeBinding &binding,
                                                 const EngineSchema &schema)
        : objects_(memory, binding, schema)
    {
    }

    bool CommonObjectCollector::IsClassObject(uintptr_t object, const std::string &expectedName) const
    {
        if (object == 0)
            return false;

        const auto classAddress = objects_.Class(object);
        if (!classAddress)
            return false;

        const auto className = objects_.Name(*classAddress);
        if (!className || *className != "Class")
            return false;

        const auto objectName = objects_.Name(object);
        if (!objectName)
            return false;

        return *objectName == expectedName;
    }

    std::vector<CommonObjectInfo> CommonObjectCollector::Collect()
    {
        std::vector<CommonObjectInfo> result;

        if (!objects_.Initialize())
            return result;

        const std::vector<std::string> commonClasses = {
            "Engine",
            "World",
            "GameInstance",
            "LocalPlayer",
            "PlayerController",
            "GameViewportClient",
            "Console",
            "CheatManager"};

        const int32_t count = objects_.Count();
        for (int32_t index = 0; index < count; ++index)
        {
            const auto object = objects_.ObjectAt(index);
            if (!object)
                continue;

            for (const std::string &targetName : commonClasses)
            {
                if (IsClassObject(*object, targetName))
                {
                    CommonObjectInfo info;
                    info.name = targetName;
                    info.address = *object;
                    info.index = index;
                    result.push_back(info);
                    break;
                }
            }
        }

        return result;
    }
} // namespace anduefker::binding
