#pragma once

#include "anduefker/ue/EngineSchema.hpp"
#include "anduefker/ue/EngineVersion.hpp"

namespace anduefker::ue
{
    /**
     * @brief 在运行时探测之前对原始 UE 布局族进行分类
     *
     * 本目录特意提供了特征边界，而非绝对远程偏移
     * 绝对偏移量仍然是运行时证据，并且仅在语义检查后才会被提交
     */
    class SchemaCatalog
    {
    public:
        [[nodiscard]] static EngineFeatures FeaturesFor(const EngineVersion &version);
        [[nodiscard]] static EngineFamily FamilyFor(const EngineVersion &version);
    };
} // namespace anduefker::ue
