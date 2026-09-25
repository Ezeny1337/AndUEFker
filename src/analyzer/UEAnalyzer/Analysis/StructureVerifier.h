#pragma once

#include <cstdint>
#include <vector>

#include "../../Memory/IMemory.h"

namespace anduefker::analyzer
{

	class GlobalAccessHarvester;

	/**
	 * @brief Lazily-cached facts about one (Module, Harvester) pairing.
	 *
	 * Not a verification engine - it decides no structure's shape; that logic
	 * lives in the strategy that needs it (see NameStrategy.cpp's interiority
	 * test, or ObjObjectsStrategy's VerifyObjectArrayShape). This class exists
	 * for cache lifetime, not sharing: SortedGlobals()/HotTableThreshold() are
	 * each read by one strategy today but cost an O(n log n) pass over every
	 * global, and Score() calls Verify() once per candidate. Caching on the
	 * strategy instance instead would be wrong - verify_truths and
	 * explain_reserved_names both reuse one set of strategies across many
	 * different binaries - so a StructureVerifier is built fresh per query and
	 * discarded, keeping the cache's lifetime pinned to the one Harvester it
	 * was built from.
	 */
	class StructureVerifier
	{
	public:
		/**
		 * @param Module    Passed in rather than fetched: GetUnrealModule() returns by
		 *                  value, and a verifier is constructed per anchored site.
		 * @param Harvester The shared access map, which is all the evidence there is.
		 *
		 * Takes no memory backend: every decision comes from the access map and
		 * nothing is ever dereferenced.
		 */
		StructureVerifier(const ModuleInfo &Module, const GlobalAccessHarvester &Harvester)
			: Module_(Module),
			  Harvester_(Harvester)
		{
		}

		const ModuleInfo &Module() const { return Module_; }
		const GlobalAccessHarvester &Harvester() const { return Harvester_; }

		/**
		 * @brief True when Address sits where a UE global can live.
		 *
		 * Writable and non-executable, decided from permissions rather than section
		 * names: .bss is not reliably the last segment and the section naming
		 * differs per format, so this is the portable way to say "where globals
		 * live". Cheap and needs no cache; kept here anyway since every strategy's
		 * Verify() already receives this object and calls it first.
		 */
		bool IsPlausibleGlobalLocation(uint64_t Address) const;

		/// Addresses of recorded globals, ascending. Read today only by
		/// NameStrategy's interiority test, which walks outward from a candidate
		/// looking for a bigger enclosing global; cached here so that walk does
		/// not re-sort the whole access map per candidate.
		const std::vector<uint64_t> &SortedGlobals() const;

		/**
		 * @brief Pointer-width access count at or above which a field counts as "hot".
		 *
		 * Computed as a percentile of this binary's own globals, relative rather
		 * than absolute, so it travels between a 40 MB mobile build and a 700 MB
		 * dump. Read today by ObjObjectsStrategy's shape check, and transitively
		 * by GUObjectArrayStrategy through it.
		 */
		uint32_t HotTableThreshold() const;

	private:
		const ModuleInfo &Module_;
		const GlobalAccessHarvester &Harvester_;
		mutable uint32_t HotThreshold_ = 0;
		mutable bool bHotThresholdBuilt_ = false;
		mutable std::vector<uint64_t> Sorted_;
		mutable bool bSortedBuilt_ = false;
	};

} // namespace anduefker::analyzer
