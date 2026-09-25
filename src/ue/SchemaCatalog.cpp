#include "anduefker/ue/SchemaCatalog.hpp"

namespace anduefker::ue
{
    EngineFamily SchemaCatalog::FamilyFor(const EngineVersion &version)
    {
        if (!version.IsValid())
            return EngineFamily::Unknown;
        if (version.major < 5)
            return version.minor >= 25 ? EngineFamily::UE4FProperty : EngineFamily::UE4UProperty;
        return EngineFamily::UE5FProperty;
    }

    EngineFeatures SchemaCatalog::FeaturesFor(const EngineVersion &version)
    {
        EngineFeatures result;
        if (!version.IsValid())
            return result;

        result.useFProperty = (version.major == 4 && version.minor >= 25) || version.major >= 5;
        result.useNamePool = version.major >= 4 && version.minor >= 23;
        result.largeWorldCoordinates = version.major >= 5;
        result.enumHasUnderlyingType = false;
        result.enumUsesFNameData = false;
        // 在源码的运行时布局中，反射出的 FProperty/UProperty ArrayDim 字段仍保持为 int32 类型
        // 生成参数描述符中的 byte-sized ArrayDim 字段属于另一种不同的契约
        result.arrayDimIsByte = false;

        // 这些是运行时观测到的选项
        // 仅提供初始探测顺序，切勿将其视作已 Cook 或已修改二进制文件的证据
        result.casePreservingName = false;
        result.outlineNumberName = false;
        // 在 UE 5.2 及之前，FFieldVariant 使用显式 bool 鉴别器（discriminator）存储类型
        // 而在 UE 5.3+ 中，它切换为了低位标记指针（low-bit tagged pointer）
        result.fFieldOwnerMask = version.major > 5 || (version.major == 5 && version.minor >= 3);
        return result;
    }
} // namespace anduefker::ue
