#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace anduefker::analyzer
{

	/**
	 * @brief A single piece of evidence supporting (or opposing) a candidate.
	 *
	 * Signals are kept alongside the score so that every result can explain itself.
	 * A locator that says "0x1234, confidence 0.91" without saying why is not
	 * actionable on a binary nobody has looked at yet.
	 */
	struct Signal
	{
		std::string Name;	 ///< Short identifier, e.g. "access_purity".
		float Weight = 0.0f; ///< Contribution to the raw score; negative argues against.
		std::string Detail;	 ///< Human-readable specifics, e.g. "98.2% pointer-width".

		Signal() = default;
		Signal(std::string InName, float InWeight, std::string InDetail = {})
			: Name(std::move(InName)),
			  Weight(InWeight),
			  Detail(std::move(InDetail))
		{
		}
	};

	/**
	 * @brief Which mechanism produced a candidate.
	 *
	 * Kept separate from the free-form Method string so callers and the JSON output
	 * can act on it. The mechanisms are independent - a string anchor and a code
	 * shape share no assumptions - which is why agreement between two of them is
	 * treated as corroboration rather than duplication.
	 */
	enum class EFindMethod : uint8_t
	{
		Unknown = 0,
		/// A function identified by an engine string, resolved and structure-verified.
		Anchored,
		/// A byte pattern matched in executable memory.
		Pattern,
		/// Whole-image ranking. Produces leads, never answers.
		Statistical,
		/// An exported symbol named the target outright.
		Symbol,
	};

	/// @brief Name of a find method, for reporting.
	const char *EFindMethodToString(EFindMethod M);

	/// Parses a method selector such as a --method argument.
	///
	/// @param Text One of auto|anchor|anchored|pattern|leads|statistical.
	/// @param Out  Receives the method, or an empty optional for "auto".
	/// @return false when Text names no known method, leaving Out untouched.
	bool ParseFindMethod(const std::string &Text, std::optional<EFindMethod> &Out);

	/**
	 * @brief A located global, with the evidence that produced it.
	 */
	struct Candidate
	{
		/// Which mechanism produced this. `Method` stays the human-readable detail.
		EFindMethod FindMethod = EFindMethod::Unknown;

		uintptr_t Address = 0;		 ///< Runtime address of the global.
		float Confidence = 0.0f;	 ///< Normalised score in [0,1].
		std::string Method;			 ///< Strategy names, joined with '+' when fused.
		std::vector<Signal> Signals; ///< Why this address, in the order evidence arrived.

		Candidate() = default;
		Candidate(uintptr_t InAddress, float InConfidence, std::string InMethod)
			: Address(InAddress),
			  Confidence(InConfidence),
			  Method(std::move(InMethod))
		{
		}

		/// Records one piece of evidence.
		void Add(std::string Name, float Weight, std::string Detail = {})
		{
			Signals.emplace_back(std::move(Name), Weight, std::move(Detail));
		}

		/// True when a signal of this name has already been recorded.
		bool Has(const std::string &Name) const
		{
			for (const auto &S : Signals)
				if (S.Name == Name)
					return true;
			return false;
		}

		bool IsValid() const { return Address != 0; }

		/**
		 * @brief Whether this candidate clears the confidence gate.
		 *
		 * Takes a threshold rather than ScoringWeights: Scoring.h includes this
		 * header, so depending on it here would invert the dependency.
		 */
		bool IsConfident(float MinConfidence) const { return Confidence >= MinConfidence; }

		/// Multi-line rendering: address, confidence, method, then every signal.
		std::string ToString() const;
	};

} // namespace anduefker::analyzer
