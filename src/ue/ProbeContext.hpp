#pragma once

#include "ProbeSupport.hpp"
#include "ProbeSession.hpp"

namespace anduefker::ue::schema_probe
{
    // 各个 Stages 共享借用同一个选择会话
    // 所有的 Readers 其生命周期绝不可超越该会话
    struct SchemaProbeContext
    {
        const IMemorySource &memory_;
        const RuntimeBinding &binding_;
        const EngineProfile &profile_;
        const SchemaProbeNames &names_;
        const uintptr_t moduleBase_;
        const uintptr_t moduleEnd_;
        const std::shared_ptr<const SchemaProbeBootstrap> &bootstrap_;
        SchemaProbeSession &session_;

        [[nodiscard]] std::optional<uintptr_t> FindObjectByName(const EngineSchema &schema,
                                                                const std::string &name) const;
        [[nodiscard]] bool FindPointerField(uintptr_t first,
                                            uintptr_t expected,
                                            int32_t minOffset,
                                            int32_t maxOffset,
                                            int32_t &result) const;
        [[nodiscard]] bool FindInt32Field(uintptr_t object,
                                          int32_t expected,
                                          int32_t minOffset,
                                          int32_t maxOffset,
                                          int32_t &result) const;

        // UObject 写入 FName/features 和 UObject 偏移；验证不修改 validation 标记
        [[nodiscard]] bool ResolveUObjectSchema(EngineSchema &schema, SchemaResolutionReport &report) const;
        [[nodiscard]] bool ValidateUObjectSchema(EngineSchema &schema, SchemaResolutionReport &report) const;
        // Struct 依赖 UObject，写入 SuperStruct/PropertiesSize/LWC，重置字段链偏移
        [[nodiscard]] bool ResolveStructSchema(EngineSchema &schema, SchemaResolutionReport &report) const;
        // Field 依赖 Struct，写入 UField/FField/FFieldClass 和属性链
        [[nodiscard]] bool ResolveFieldSchema(EngineSchema &schema, SchemaResolutionReport &report) const;
        // Property 依赖 Field；subtype 按源码结构候选验证，缺样本或歧义保留未解析
        [[nodiscard]] bool ResolvePropertySchema(EngineSchema &schema, SchemaResolutionReport &report) const;
        [[nodiscard]] bool ResolvePropertySubtypes(EngineSchema &schema, SchemaResolutionReport &report) const;
        // Function 依赖 Property，补全 UField::Next/UStruct::Children 并写入 UFunction
        [[nodiscard]] bool ResolveFunctionSchema(EngineSchema &schema, SchemaResolutionReport &report) const;
        // Enum 依赖 FName/UObject/UField，写入 UEnum 和 enum features
        [[nodiscard]] bool ResolveEnumSchema(EngineSchema &schema, SchemaResolutionReport &report) const;
    };
} // namespace anduefker::ue::schema_probe
