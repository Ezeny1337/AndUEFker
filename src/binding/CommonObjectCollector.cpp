#include "anduefker/binding/CommonObjectCollector.hpp"

#include <algorithm>

namespace anduefker::binding
{
    CommonObjectCollector::CommonObjectCollector(const IMemorySource &memory,
                                                 const RuntimeBinding &binding,
                                                 const EngineSchema &schema)
        : objects_(memory, binding, schema)
    {
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
            const auto object = objects_.Objects().ReadObject(index);
            if (!object.IsValid())
                continue;

            const auto className = objects_.ClassName(object.address);
            if (!className || *className != "Class")
                continue;
            const auto name = objects_.Name(object.address);
            if (name && std::find(commonClasses.begin(), commonClasses.end(), *name) != commonClasses.end())
                result.push_back({*name, object.address, index});
        }

        return result;
    }
} // namespace anduefker::binding
