#include "StructureVerifier.h"

#include <algorithm>

#include "../../Memory/IMemory.h"

#include "GlobalAccessHarvester.h"

namespace anduefker::analyzer
{

	bool StructureVerifier::IsPlausibleGlobalLocation(uint64_t Address) const
	{
		const MemRegionInfo *R = Module_.FindAddressRegion(Address);
		return R && R->IsWriteable() && !R->IsExecutable();
	}

	const std::vector<uint64_t> &StructureVerifier::SortedGlobals() const
	{
		if (bSortedBuilt_)
			return Sorted_;
		bSortedBuilt_ = true;

		Sorted_.reserve(Harvester_.GetAccesses().size());
		for (const auto &[Addr, Info] : Harvester_.GetAccesses())
		{
			(void)Info;
			Sorted_.push_back(Addr);
		}
		std::sort(Sorted_.begin(), Sorted_.end());
		return Sorted_;
	}

	uint32_t StructureVerifier::HotTableThreshold() const
	{
		if (bHotThresholdBuilt_)
			return HotThreshold_;
		bHotThresholdBuilt_ = true;

		// A percentile of this binary's own globals, so the bar scales with the
		// size of the image instead of using a fixed constant.
		std::vector<uint32_t> Counts;
		Counts.reserve(1024);
		for (const auto &[Addr, Info] : Harvester_.GetAccesses())
		{
			(void)Addr;
			if (Info.WideAccesses > 0)
				Counts.push_back(Info.WideAccesses);
		}
		if (Counts.empty())
		{
			HotThreshold_ = 0;
			return HotThreshold_;
		}

		// Top 2%. The element table is not merely above average, it is one of a
		// handful of globals the whole engine funnels through.
		constexpr size_t kHotPercentile = 98;
		const size_t Index = Counts.size() * kHotPercentile / 100;
		std::nth_element(Counts.begin(), Counts.begin() + static_cast<long>(Index), Counts.end());
		HotThreshold_ = Counts[Index];
		return HotThreshold_;
	}

} // namespace anduefker::analyzer
