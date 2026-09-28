#include "IArchDecoder.h"

#include <KittyAsm.hpp>

#include <cstring>
#include <memory>
#include <string>

namespace
{
    int RegIndex(const std::string &name)
    {
        if (name.size() < 2 || name[0] != 'r')
            return -1;
        if (name == "sp" || name == "lr" || name == "pc")
            return name == "sp" ? 13 : name == "lr" ? 14
                                                    : 15;

        int value = 0;
        for (size_t index = 1; index < name.size(); ++index)
        {
            if (name[index] < '0' || name[index] > '9')
                return -1;
            value = value * 10 + (name[index] - '0');
        }
        return value >= 0 && value <= 15 ? value : -1;
    }

    bool IsPc(const std::string &name)
    {
        return name == "pc";
    }

    bool IsImmediateDataProcessing(uint32_t word)
    {
        return ((word >> 25) & 1u) != 0;
    }

    bool IsImmediateSingleDataTransfer(uint32_t word)
    {
        return ((word >> 25) & 1u) == 0;
    }

    bool IsImmediateHalfwordTransfer(uint32_t word)
    {
        return ((word >> 22) & 1u) != 0;
    }

    uint8_t AccessWidth(EKittyInsnTypeArm32 type)
    {
        switch (type)
        {
        case EKittyInsnTypeArm32::LDRB:
        case EKittyInsnTypeArm32::STRB:
        case EKittyInsnTypeArm32::LDRSB:
            return 1;
        case EKittyInsnTypeArm32::LDRH:
        case EKittyInsnTypeArm32::STRH:
        case EKittyInsnTypeArm32::LDRSH:
            return 2;
        default:
            return 4;
        }
    }

    bool IsLoad(EKittyInsnTypeArm32 type)
    {
        switch (type)
        {
        case EKittyInsnTypeArm32::LDR:
        case EKittyInsnTypeArm32::LDRB:
        case EKittyInsnTypeArm32::LDRH:
        case EKittyInsnTypeArm32::LDRSB:
        case EKittyInsnTypeArm32::LDRSH:
        case EKittyInsnTypeArm32::LDR_LITERAL:
            return true;
        default:
            return false;
        }
    }

    bool IsStore(EKittyInsnTypeArm32 type)
    {
        switch (type)
        {
        case EKittyInsnTypeArm32::STR:
        case EKittyInsnTypeArm32::STRB:
        case EKittyInsnTypeArm32::STRH:
            return true;
        default:
            return false;
        }
    }

    bool IsMovw(uint32_t word)
    {
        return (word & 0x0FF00000u) == 0x03000000u;
    }

    bool IsMovt(uint32_t word)
    {
        return (word & 0x0FF00000u) == 0x03400000u;
    }

    uint32_t MovWideImmediate(uint32_t word)
    {
        return ((word >> 4) & 0xF000u) | (word & 0x0FFFu);
    }

    bool IsPush(uint32_t word)
    {
        // PUSH is the ARM assembler alias for STMDB sp!, {register-list}.
        return (word & 0x0FFF0000u) == 0x092D0000u && (word & 0xFFFFu) != 0;
    }

    bool IsBx(uint32_t word)
    {
        return (word & 0x0FFFFFF0u) == 0x012FFF10u;
    }

    bool IsBlxRegister(uint32_t word)
    {
        return (word & 0x0FFFFFF0u) == 0x012FFF30u;
    }

    uint32_t PcRelativeTarget(uint32_t address, uint32_t word, int32_t immediate)
    {
        const bool subtract = ((word >> 21) & 1u) != 0;
        const int64_t signedOffset = subtract ? -static_cast<int64_t>(immediate) : immediate;
        return static_cast<uint32_t>(static_cast<uint64_t>(address) + 8u + signedOffset);
    }

    class Arm32Decoder final : public IArchDecoder
    {
    public:
        EArch Arch() const override { return EArch::Arm32; }
        uint32_t InstructionStride() const override { return 4; }
        int RegisterCount() const override { return 16; }

        bool CallClobbers(int reg) const override
        {
            // AAPCS32 caller-saved registers: r0-r3, r12 and lr.
            return reg >= 0 && (reg <= 3 || reg == 12 || reg == 14);
        }

        bool DecodeLocalBytes(const uint8_t *at, size_t avail, uint64_t addr, NormalizedInsn &out) const override
        {
            out = {};
            out.Length = 4;
            if (!at || avail < 4 || addr > UINT32_MAX)
                return false;

            uint32_t word = 0;
            std::memcpy(&word, at, sizeof(word));
            const uint32_t address = static_cast<uint32_t>(addr);

            if (IsPush(word))
            {
                out.Kind = NormalizedInsn::EKind::Prologue;
                return true;
            }

            if (IsBx(word))
            {
                const int source = static_cast<int>((word & 0xFu));
                if (source == 14)
                    out.Kind = NormalizedInsn::EKind::Return;
                else
                {
                    out.Kind = NormalizedInsn::EKind::Branch;
                    out.Base = source;
                }
                return true;
            }

            if (IsBlxRegister(word))
            {
                out.Kind = NormalizedInsn::EKind::Call;
                out.Base = static_cast<int>(word & 0xFu);
                return true;
            }

            if (IsMovw(word) || IsMovt(word))
            {
                out.Kind = NormalizedInsn::EKind::MoveImm;
                out.Dest = static_cast<int>((word >> 12) & 0xFu);
                out.Value = IsMovt(word) ? static_cast<uint64_t>(MovWideImmediate(word)) << 16
                                         : MovWideImmediate(word);
                out.bComposes = IsMovt(word);
                out.bAddressLike = true;
                return true;
            }

            const KittyInsnArm32 insn = KittyArm32::decodeInsn(word, address);
            if (!insn.isValid())
                return false;

            const int rd = RegIndex(insn.rd);
            const int rn = RegIndex(insn.rn);
            const int rt = RegIndex(insn.rt);

            switch (insn.type)
            {
            case EKittyInsnTypeArm32::ADR:
                out.Kind = NormalizedInsn::EKind::SetBase;
                out.Dest = rd;
                out.Value = PcRelativeTarget(address, word, insn.immediate);
                out.bExactAddress = true;
                return true;

            case EKittyInsnTypeArm32::ADD:
            case EKittyInsnTypeArm32::SUB:
            {
                if (!IsImmediateDataProcessing(word))
                {
                    out.Kind = NormalizedInsn::EKind::Other;
                    out.Dest = rd;
                    return true;
                }

                if (IsPc(insn.rn))
                {
                    out.Kind = NormalizedInsn::EKind::SetBase;
                    out.Dest = rd;
                    out.Value = PcRelativeTarget(address, word, insn.immediate);
                    out.bExactAddress = true;
                    return true;
                }

                out.Kind = NormalizedInsn::EKind::AddImm;
                out.Dest = rd;
                out.Base = rn;
                out.Value = insn.type == EKittyInsnTypeArm32::SUB
                                ? static_cast<uint64_t>(-static_cast<int64_t>(insn.immediate))
                                : static_cast<uint64_t>(insn.immediate);
                return true;
            }

            case EKittyInsnTypeArm32::MOV:
                if (IsImmediateDataProcessing(word))
                {
                    out.Kind = NormalizedInsn::EKind::MoveImm;
                    out.Dest = rd;
                    out.Value = static_cast<uint32_t>(insn.immediate);
                    out.bAddressLike = false;
                }
                else
                {
                    out.Kind = NormalizedInsn::EKind::MoveReg;
                    out.Dest = rd;
                    out.Base = rt;
                }
                return true;

            case EKittyInsnTypeArm32::LDR_LITERAL:
                out.Kind = NormalizedInsn::EKind::LoadLiteral;
                out.Dest = rd;
                out.Value = insn.target;
                out.AccessWidth = 4;
                return true;

            default:
                break;
            }

            if (IsLoad(insn.type) || IsStore(insn.type))
            {
                const bool immediate = insn.type == EKittyInsnTypeArm32::LDRH ||
                                               insn.type == EKittyInsnTypeArm32::STRH ||
                                               insn.type == EKittyInsnTypeArm32::LDRSB ||
                                               insn.type == EKittyInsnTypeArm32::LDRSH
                                           ? IsImmediateHalfwordTransfer(word)
                                           : IsImmediateSingleDataTransfer(word);
                out.Kind = IsLoad(insn.type) ? NormalizedInsn::EKind::Load : NormalizedInsn::EKind::Store;
                out.Dest = rd;
                out.Base = rn;
                out.Offset = immediate ? insn.immediate : 0;
                out.AccessWidth = AccessWidth(insn.type);
                return true;
            }

            if (insn.type == EKittyInsnTypeArm32::BL)
            {
                out.Kind = NormalizedInsn::EKind::Call;
                out.Value = insn.target;
                return true;
            }
            if (insn.type == EKittyInsnTypeArm32::B || insn.type == EKittyInsnTypeArm32::B_COND)
            {
                out.Kind = NormalizedInsn::EKind::Branch;
                out.Value = insn.target;
                return true;
            }

            return false;
        }
    };
} // namespace

std::unique_ptr<IArchDecoder> CreateArm32Decoder()
{
    return std::make_unique<Arm32Decoder>();
}
