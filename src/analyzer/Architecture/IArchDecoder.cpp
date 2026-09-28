#include "IArchDecoder.h"

extern std::unique_ptr<IArchDecoder> CreateArm64Decoder();
extern std::unique_ptr<IArchDecoder> CreateArm32Decoder();

std::unique_ptr<IArchDecoder> CreateArchDecoder(EArch arch)
{
    if (arch == EArch::Arm64)
        return CreateArm64Decoder();
    if (arch == EArch::Arm32)
        return CreateArm32Decoder();
    return nullptr;
}
