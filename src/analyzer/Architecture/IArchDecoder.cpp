#include "IArchDecoder.h"

extern std::unique_ptr<IArchDecoder> CreateArm64Decoder();

std::unique_ptr<IArchDecoder> CreateArchDecoder(EArch arch)
{
    if (arch == EArch::Arm64)
        return CreateArm64Decoder();
    return nullptr;
}
