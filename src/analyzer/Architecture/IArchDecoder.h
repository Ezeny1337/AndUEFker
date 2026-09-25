#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

/// @brief Supprted Architecture enum.
enum class EArch
{
	Unknown,
	Arm64,
	Arm32,
};

/// @brief Name of an architecture.
inline const char *EArchToString(EArch Arch)
{
	switch (Arch)
	{
	case EArch::Arm64:
		return "ARM64";
	case EArch::Arm32:
		return "ARM32";
	default:
		return "unknown";
	}
}

/// @brief Architecture-neutral view of one instruction.
struct NormalizedInsn
{
	enum class EKind : uint8_t
	{
		Other,	 ///< Unmodelled. Clobbers Dest when Dest >= 0.
		SetBase, ///< Materializes an address into Dest (ADRP page, ADR target).
		AddImm,	 ///< Dest = Base + Value.

		/**
		 * @brief Dest = the pointer-sized word stored in the image at Value.
		 *
		 * A literal load, and deliberately *not* an address computation: what
		 * the word means is decided by what the program does with it next. In
		 * ARM32's PIC idiom the pool word is a displacement, completed by a
		 * following `ADD Rd,PC,Rd`; elsewhere the same instruction loads an
		 * absolute address. Reading it as an address here would fabricate a
		 * global out of every displacement that happens to land in the image.
		 */
		LoadLiteral,

		Load,	  ///< Dest = *(Base + Offset).
		Store,	  ///< *(Base + Offset) = Dest.
		MoveImm,  ///< Dest = Value, or Dest |= Value when bComposes.
		MoveReg,  ///< Dest = Base. Register-to-register copy.
		Call,	  ///< Clobbers the caller-saved set.
		Branch,	  ///< Control transfer; does not invalidate tracked bases.
		Return,	  ///< Function end; invalidates everything.
		Prologue, ///< Frame setup; marks a likely function entry.
	};

	EKind Kind = EKind::Other; ///< What this instruction does, as modelled here.

	int Dest = -1; ///< Normalized destination register index, -1 when absent.
	int Base = -1; ///< Normalized source/base register index, -1 when absent.

	uint64_t Value = 0; ///< SetBase target / AddImm addend / MoveImm immediate.
	int64_t Offset = 0; ///< Load/Store displacement.

	uint8_t AccessWidth = 0; ///< Load/Store size in bytes (4, 8, ...), 0 if n/a.
	uint8_t Length = 4;		 ///< Encoded length, for variable-width targets.

	/// SetBase: Value is the complete address (ADR), not just a page (ADRP).
	bool bExactAddress = false;

	/// MoveImm: ORs into the register's accumulated immediate rather than
	/// replacing it (AArch64 MOVK, ARM32 MOVT).
	bool bComposes = false;

	/// MoveImm: the composed immediate is meant to be an address, not a scalar.
	/// AArch64 builds addresses with ADRP/ADD and uses MOVZ/MOVK only for
	/// constants, so it leaves this false; ARM32 builds them with MOVW/MOVT and
	/// will set it, letting the harvester promote the result to a base.
	bool bAddressLike = false;

	/**
	 * @brief Load/Store: the instruction also updates its base register.
	 *
	 * The pre- and post-indexed forms move Rn as a side effect of the access.
	 * A tracker that misses this keeps believing a base the instruction has
	 * just changed, and every later use of it names an address that was never
	 * computed - so consumers must invalidate Base when this is set.
	 */
	bool bWritesBackBase = false;
};

/**
 * @brief Global arch decoder layer interface.
 */
class IArchDecoder
{
public:
	virtual ~IArchDecoder() = default;

	/// Which architecture this decoder handles.
	virtual EArch Arch() const = 0;

	/// Fixed instruction size in bytes, or 0 when the target is variable-width
	/// (in which case NormalizedInsn::Length drives the walk).
	virtual uint32_t InstructionStride() const = 0;

	/// How many normalized register indices exist, for sizing tracking state.
	virtual int RegisterCount() const = 0;

	/// True when a call may destroy this register's value.
	virtual bool CallClobbers(int Reg) const = 0;

	/**
	 * @brief Decodes one instruction from a local buffer.
	 *
	 * Reads @p At directly. The bytes must already be in this process's memory -
	 * the decoder never reaches into the target - so a caller working on a live
	 * process must copy them across first, which is what CodeWindow does.
	 *
	 * @param At    Locally readable bytes to decode.
	 * @param Avail How many bytes are readable at At.
	 * @param Addr  Address those bytes live at in the target, for PC-relative forms.
	 * @param Out   Receives the normalized instruction.
	 * @return false when nothing usable was decoded; Out.Length must still be set
	 *         so the walk can advance.
	 */
	virtual bool DecodeLocalBytes(const uint8_t *At, size_t Avail, uint64_t Addr, NormalizedInsn &Out) const = 0;
};

/**
 * @brief Dispatches to the per-architecture factories.
 * @return unique_ptr of decoder or nullptr for an architecture with no decoder.
 */
std::unique_ptr<IArchDecoder> CreateArchDecoder(EArch Arch);

/**
 * @brief Global active arch decoder backend.
 */
inline std::unique_ptr<IArchDecoder> GArchDecoder;
