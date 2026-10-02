#pragma once

#include "anduefker/ue/EngineSchema.hpp"
#include "anduefker/ue/EngineVersion.hpp"

#include <vector>

namespace anduefker::ue
{
    struct EngineProfile
    {
        std::string id;
        std::string label;
        std::string versionRange;
        EngineVersion representativeVersion;
        EngineFamily family = EngineFamily::Unknown;
        EngineFeatures features;
        SchemaLayoutVariant layout = SchemaLayoutVariant::Unknown;

        [[nodiscard]] bool IsValid() const
        {
            return !id.empty() && !label.empty() && representativeVersion.IsValid() &&
                   family != EngineFamily::Unknown && layout != SchemaLayoutVariant::Unknown;
        }
    };

    /**
     * @brief 在运行时探测之前对原始 UE 布局族进行分类
     *
     * 本目录特意提供了特征边界，而非绝对远程偏移
     * 绝对偏移量仍然是运行时证据，并且仅在语义检查后才会被提交
     */
    class SchemaCatalog
    {
    public:
        [[nodiscard]] static std::vector<EngineProfile> Profiles();
        [[nodiscard]] static EngineFeatures FeaturesFor(const EngineVersion &version);
        [[nodiscard]] static EngineFeatures FeaturesForLayout(SchemaLayoutVariant layout);
        [[nodiscard]] static SchemaLayoutVariant LayoutFor(const EngineVersion &version);
        [[nodiscard]] static EngineFamily FamilyFor(const EngineVersion &version);
    };
} // namespace anduefker::ue
