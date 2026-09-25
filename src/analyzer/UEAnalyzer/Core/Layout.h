#pragma once

#include <cstdint>
#include <string>

namespace anduefker::analyzer
{

	/// A reading of the Unreal structure an address is shaped like, together
	/// with how strongly the evidence supports it.
	///
	/// Kind is the strategy's own name for the shape it found (e.g. "FNamePool",
	/// "FUObjectArray") - free-form rather than a shared enum, since the set of
	/// nameable shapes grows with the set of strategies, and a shared enum would
	/// need editing every time a strategy is added. Empty means undetermined.
	///
	/// The engine version is never read, so the kind is a reading of the access
	/// shape and not a fact about the build. Callers that must not guess should
	/// require a Confidence they are comfortable with, or accept an unknown kind.
	struct LayoutClassification
	{
		std::string Kind;

		/// Strength of the reading in [0,1]. Zero whenever Kind is empty.
		float Confidence = 0.0f;

		/// True when a kind was determined at all.
		bool IsKnown() const { return !Kind.empty(); }
	};

	/// Why a structure check accepted or rejected an address.
	///
	/// Deliberately small and universal: these are the gates every strategy
	/// checks identically before its own structure-specific logic runs. A
	/// structure-specific rejection reason is not a new enumerator here - it is
	/// Rejected, with the specifics in StructureEvidence::Why - because growing
	/// this enum per strategy is exactly the coupling extensibility is meant to
	/// avoid.
	enum class EVerdict : uint8_t
	{
		/// The access shape matches the target structure.
		Accepted = 0,

		/// Not in writable, non-executable memory.
		NotWritableData,

		/// No instruction computes this address.
		Unreferenced,

		/// Structure-specific verification declined it; see StructureEvidence::Why.
		Rejected,
	};

	/// One-line wording of a verdict, for reporting.
	const char *EVerdictToString(EVerdict Verdict);

} // namespace anduefker::analyzer
