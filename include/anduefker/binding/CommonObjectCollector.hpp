#pragma once

#include <vector>

#include "anduefker/binding/RuntimeBinding.hpp"
#include "anduefker/memory/MemorySource.hpp"
#include "anduefker/ue/EngineSchema.hpp"
#include "anduefker/ue/ObjectModelReader.hpp"

namespace anduefker::binding
{
    using ::anduefker::memory::IMemorySource;
    using ::anduefker::ue::EngineSchema;
    using ::anduefker::ue::ObjectModelReader;

    class CommonObjectCollector
    {
    public:
        CommonObjectCollector(const IMemorySource &memory,
                              const RuntimeBinding &binding,
                              const EngineSchema &schema);

        [[nodiscard]] std::vector<CommonObjectInfo> Collect();

    private:
        [[nodiscard]] bool IsClassObject(uintptr_t object, const std::string &expectedName) const;

        const IMemorySource &memory_;
        ObjectModelReader objects_;
        const EngineSchema &schema_;
    };
} // namespace anduefker::binding
