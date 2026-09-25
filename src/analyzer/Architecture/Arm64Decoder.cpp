#include "IArchDecoder.h"

#include <KittyAsm.hpp>

#include <cstring>
#include <memory>
#include <string>

namespace
{
    int RegIndex(const std::string &name)
    {
        if (name.size() < 2 || (name[0] != 'X' && name[0] != 'W'))
            return -1;
        if (name == "XZR" || name == "WZR" || name == "SP")
            return -1;

        int value = 0;
        for (size_t index = 1; index < name.size(); ++index)
        {
            if (name[index] < '0' || name[index] > '9')
                return -1;
            value = value * 10 + (name[index] - '0');
        }
        return value <= 30 ? value : -1;
    }

    bool IsLiteral(EKittyInsnTypeArm64 type)
    {
        return type == EKittyInsnTypeArm64::LDR_LITERAL || type == EKittyInsnTypeArm64::LDRW_LITERAL ||
               type == EKittyInsnTypeArm64::LDRSW_LITERAL;
    }

    bool IsLoad(EKittyInsnTypeArm64 type)
    {
        switch (type)
        {
        case EKittyInsnTypeArm64::LDR:
        case EKittyInsnTypeArm64::LDRW:
        case EKittyInsnTypeArm64::LDRB:
        case EKittyInsnTypeArm64::LDRH:
        case EKittyInsnTypeArm64::LDRSB:
        case EKittyInsnTypeArm64::LDRSH:
        case EKittyInsnTypeArm64::LDRSW:
        case EKittyInsnTypeArm64::LDR_PRE:
        case EKittyInsnTypeArm64::LDRB_PRE:
        case EKittyInsnTypeArm64::LDRH_PRE:
        case EKittyInsnTypeArm64::LDRSB_PRE:
        case EKittyInsnTypeArm64::LDRSH_PRE:
        case EKittyInsnTypeArm64::LDRSW_PRE:
        case EKittyInsnTypeArm64::LDR_POST:
        case EKittyInsnTypeArm64::LDRB_POST:
        case EKittyInsnTypeArm64::LDRH_POST:
        case EKittyInsnTypeArm64::LDRSB_POST:
        case EKittyInsnTypeArm64::LDRSH_POST:
        case EKittyInsnTypeArm64::LDRSW_POST:
        case EKittyInsnTypeArm64::LDUR:
        case EKittyInsnTypeArm64::LDURW:
        case EKittyInsnTypeArm64::LDURB:
        case EKittyInsnTypeArm64::LDURH:
        case EKittyInsnTypeArm64::LDURSB:
        case EKittyInsnTypeArm64::LDURSH:
        case EKittyInsnTypeArm64::LDURSW:
            return true;
        default:
            return false;
        }
    }

    bool IsStore(EKittyInsnTypeArm64 type)
    {
        switch (type)
        {
        case EKittyInsnTypeArm64::STR:
        case EKittyInsnTypeArm64::STRW:
        case EKittyInsnTypeArm64::STRB:
        case EKittyInsnTypeArm64::STRH:
        case EKittyInsnTypeArm64::STR_PRE:
        case EKittyInsnTypeArm64::STRB_PRE:
        case EKittyInsnTypeArm64::STRH_PRE:
        case EKittyInsnTypeArm64::STR_POST:
        case EKittyInsnTypeArm64::STRB_POST:
        case EKittyInsnTypeArm64::STRH_POST:
        case EKittyInsnTypeArm64::STUR:
        case EKittyInsnTypeArm64::STURW:
        case EKittyInsnTypeArm64::STURB:
        case EKittyInsnTypeArm64::STURH:
            return true;
        default:
            return false;
        }
    }

    bool IsPostIndex(EKittyInsnTypeArm64 type)
    {
        switch (type)
        {
        case EKittyInsnTypeArm64::LDR_POST:
        case EKittyInsnTypeArm64::LDRB_POST:
        case EKittyInsnTypeArm64::LDRH_POST:
        case EKittyInsnTypeArm64::LDRSB_POST:
        case EKittyInsnTypeArm64::LDRSH_POST:
        case EKittyInsnTypeArm64::LDRSW_POST:
        case EKittyInsnTypeArm64::STR_POST:
        case EKittyInsnTypeArm64::STRB_POST:
        case EKittyInsnTypeArm64::STRH_POST:
            return true;
        default:
            return false;
        }
    }

    bool IsPreIndex(EKittyInsnTypeArm64 type)
    {
        switch (type)
        {
        case EKittyInsnTypeArm64::LDR_PRE:
        case EKittyInsnTypeArm64::LDRB_PRE:
        case EKittyInsnTypeArm64::LDRH_PRE:
        case EKittyInsnTypeArm64::LDRSB_PRE:
        case EKittyInsnTypeArm64::LDRSH_PRE:
        case EKittyInsnTypeArm64::LDRSW_PRE:
        case EKittyInsnTypeArm64::STR_PRE:
        case EKittyInsnTypeArm64::STRB_PRE:
        case EKittyInsnTypeArm64::STRH_PRE:
            return true;
        default:
            return false;
        }
    }

    uint8_t AccessWidth(EKittyInsnTypeArm64 type)
    {
        switch (type)
        {
        case EKittyInsnTypeArm64::LDRB:
        case EKittyInsnTypeArm64::STRB:
        case EKittyInsnTypeArm64::LDRSB:
        case EKittyInsnTypeArm64::LDRB_PRE:
        case EKittyInsnTypeArm64::STRB_PRE:
        case EKittyInsnTypeArm64::LDRSB_PRE:
        case EKittyInsnTypeArm64::LDRB_POST:
        case EKittyInsnTypeArm64::STRB_POST:
        case EKittyInsnTypeArm64::LDRSB_POST:
        case EKittyInsnTypeArm64::LDURB:
        case EKittyInsnTypeArm64::STURB:
        case EKittyInsnTypeArm64::LDURSB:
            return 1;
        case EKittyInsnTypeArm64::LDRH:
        case EKittyInsnTypeArm64::STRH:
        case EKittyInsnTypeArm64::LDRSH:
        case EKittyInsnTypeArm64::LDRH_PRE:
        case EKittyInsnTypeArm64::STRH_PRE:
        case EKittyInsnTypeArm64::LDRSH_PRE:
        case EKittyInsnTypeArm64::LDRH_POST:
        case EKittyInsnTypeArm64::STRH_POST:
        case EKittyInsnTypeArm64::LDRSH_POST:
        case EKittyInsnTypeArm64::LDURH:
        case EKittyInsnTypeArm64::STURH:
        case EKittyInsnTypeArm64::LDURSH:
            return 2;
        case EKittyInsnTypeArm64::LDRW:
        case EKittyInsnTypeArm64::STRW:
        case EKittyInsnTypeArm64::LDRSW:
        case EKittyInsnTypeArm64::LDRSW_PRE:
        case EKittyInsnTypeArm64::LDRSW_POST:
        case EKittyInsnTypeArm64::LDURW:
        case EKittyInsnTypeArm64::STURW:
        case EKittyInsnTypeArm64::LDURSW:
        case EKittyInsnTypeArm64::LDRW_LITERAL:
        case EKittyInsnTypeArm64::LDRSW_LITERAL:
            return 4;
        default:
            return 8;
        }
    }

    class Arm64Decoder final : public IArchDecoder
    {
    public:
        EArch Arch() const override { return EArch::Arm64; }
        uint32_t InstructionStride() const override { return 4; }
        int RegisterCount() const override { return 31; }

        bool CallClobbers(int reg) const override { return (reg >= 0 && reg <= 18) || reg == 30; }

        bool DecodeLocalBytes(const uint8_t *at, size_t avail, uint64_t addr, NormalizedInsn &out) const override
        {
            out = {};
            out.Length = 4;
            if (!at || avail < 4)
                return false;

            uint32_t word = 0;
            std::memcpy(&word, at, sizeof(word));

            if ((word & 0xFFFFFC1Fu) == 0xD65F0000u)
            {
                out.Kind = NormalizedInsn::EKind::Return;
                return true;
            }

            if ((word & 0xFFE0FFE0u) == 0xAA0003E0u || (word & 0xFFE0FFE0u) == 0x2A0003E0u)
            {
                const int dest = static_cast<int>(word & 31u);
                const int source = static_cast<int>((word >> 16) & 31u);
                out.Kind = NormalizedInsn::EKind::MoveReg;
                out.Dest = dest == 31 ? -1 : dest;
                out.Base = source == 31 ? -1 : source;
                return true;
            }

            if (((word >> 23) & 0x1FFu) == 0x153u && ((word >> 5) & 31u) == 31u)
            {
                out.Kind = NormalizedInsn::EKind::Prologue;
                return true;
            }

            if ((word & 0xFFC003FFu) == 0xD10003FFu)
            {
                out.Kind = NormalizedInsn::EKind::Prologue;
                return true;
            }

            const KittyInsnArm64 insn = KittyArm64::decodeInsn(word, addr);
            if (!insn.isValid())
                return false;

            const int rd = RegIndex(insn.rd);
            const int rn = RegIndex(insn.rn);
            const int rt = RegIndex(insn.rt);

            switch (insn.type)
            {
            case EKittyInsnTypeArm64::ADR:
            case EKittyInsnTypeArm64::ADRP:
                out.Kind = NormalizedInsn::EKind::SetBase;
                out.Dest = rd;
                out.Value = insn.target;
                out.bExactAddress = insn.type == EKittyInsnTypeArm64::ADR;
                return true;
            case EKittyInsnTypeArm64::ADD:
                out.Kind = NormalizedInsn::EKind::AddImm;
                out.Dest = rd;
                out.Base = rn;
                out.Value = static_cast<uint64_t>(insn.immediate);
                return true;
            case EKittyInsnTypeArm64::MOVZ:
            case EKittyInsnTypeArm64::MOVK:
            case EKittyInsnTypeArm64::MOVN:
                out.Kind = NormalizedInsn::EKind::MoveImm;
                out.Dest = rd;
                out.Value = static_cast<uint64_t>(insn.immediate);
                out.bComposes = insn.type == EKittyInsnTypeArm64::MOVK;
                out.bAddressLike = false;
                return true;
            case EKittyInsnTypeArm64::SUB:
                out.Kind = NormalizedInsn::EKind::Other;
                out.Dest = rd;
                return true;
            case EKittyInsnTypeArm64::BL:
                out.Kind = NormalizedInsn::EKind::Call;
                out.Value = insn.target;
                return true;
            case EKittyInsnTypeArm64::B:
                out.Kind = NormalizedInsn::EKind::Branch;
                out.Value = insn.target;
                return true;
            case EKittyInsnTypeArm64::B_COND:
            case EKittyInsnTypeArm64::CBZ:
            case EKittyInsnTypeArm64::CBNZ:
            case EKittyInsnTypeArm64::TBZ:
            case EKittyInsnTypeArm64::TBNZ:
                out.Kind = NormalizedInsn::EKind::Branch;
                return true;
            default:
                break;
            }

            if (IsLiteral(insn.type))
            {
                out.Kind = NormalizedInsn::EKind::LoadLiteral;
                out.Dest = rt;
                out.Value = insn.target;
                out.AccessWidth = AccessWidth(insn.type);
                return true;
            }

            if (IsLoad(insn.type) || IsStore(insn.type))
            {
                out.Kind = IsLoad(insn.type) ? NormalizedInsn::EKind::Load : NormalizedInsn::EKind::Store;
                out.Dest = rt;
                out.Base = rn;
                out.Offset = IsPostIndex(insn.type) ? 0 : static_cast<int64_t>(insn.immediate);
                out.AccessWidth = AccessWidth(insn.type);
                out.bWritesBackBase = IsPreIndex(insn.type) || IsPostIndex(insn.type);
                return true;
            }

            return false;
        }
    };
} // namespace

std::unique_ptr<IArchDecoder> CreateArm64Decoder()
{
    return std::make_unique<Arm64Decoder>();
}
