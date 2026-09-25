#pragma once

#include <cstdint>

#include "IStrategy.h"

namespace anduefker::analyzer
{

	/**
	 * @brief Common ground for the built-in strategies.
	 *
	 * Placement and access-shape evidence are identical no matter which global is
	 * being hunted; only the corroborating signals differ. Keeping that evidence
	 * here means a scoring fix reaches every strategy at once, and gives the shared
	 * helpers an owner instead of leaving them as free functions in a header.
	 *
	 * The helpers are protected statics rather than instance methods: they depend on
	 * the analysis context, not on any strategy's state.
	 */
	class StrategyBase : public IStrategy
	{
	protected:
		/**
		 * @brief Adds the evidence every strategy contributes, and screens placement.
		 *
		 * @param C    Candidate being built; gains its `segment` and `access` signals.
		 * @param Ctx  Shared analysis.
		 * @param Info Harvested accesses for C.Address.
		 * @return false when the address cannot be a global at all, in which case the
		 *         caller must discard it outright rather than score it.
		 */
		static bool ApplyCommonSignals(Candidate &C, const AnalysisContext &Ctx, const AccessInfo &Info);

		/**
		 * @brief True when an address sits where a UE global can live.
		 *
		 * Writable and non-executable, decided from permissions rather than section
		 * names - Mach-O calls it __common/__bss, a memory dump calls it LOAD, and an
		 * Android module's .bss is an anonymous mapping with no name at all.
		 */
		static bool IsPlausibleGlobalLocation(const AnalysisContext &Ctx, uint64_t Address);
	};

} // namespace anduefker::analyzer
