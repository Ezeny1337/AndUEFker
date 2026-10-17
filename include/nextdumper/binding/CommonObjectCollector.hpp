#pragma once

#include <vector>

#include "nextdumper/binding/RuntimeBinding.hpp"
#include "nextdumper/memory/MemorySource.hpp"
#include "nextdumper/ue/EngineSchema.hpp"
#include "nextdumper/ue/ObjectModelReader.hpp"

namespace nextdumper::binding
{
    using ::nextdumper::memory::IMemorySource;
    using ::nextdumper::ue::EngineSchema;
    using ::nextdumper::ue::ObjectModelReader;

    class CommonObjectCollector
    {
    public:
        CommonObjectCollector(const IMemorySource &memory,
                              const RuntimeBinding &binding,
                              const EngineSchema &schema);

        [[nodiscard]] std::vector<CommonObjectInfo> Collect();

    private:
        ObjectModelReader objects_;
    };
} // namespace nextdumper::binding
