#include "StrategyBase.h"

#include <cstdio>

#include "../../Memory/IMemory.h"

namespace anduefker::analyzer
{

	bool StrategyBase::IsPlausibleGlobalLocation(const AnalysisContext &Ctx, uint64_t Address)
	{
		if (!Ctx.Memory)
			return false;
		const MemRegionInfo *Seg = Ctx.Module->FindAddressRegion(Address);
		// Executable memory holds code; read-only memory holds constants. A mutable
		// engine global is neither.
		return Seg && Seg->IsWriteable() && !Seg->IsExecutable();
	}

	bool StrategyBase::ApplyCommonSignals(Candidate &C, const AnalysisContext &Ctx, const AccessInfo &Info)
	{
		if (!Ctx.Memory)
			return false;

		// Same check as IsPlausibleGlobalLocation, inlined so the segment lookup runs once:
		// executable memory holds code, read-only memory holds constants, a mutable engine
		// global is neither.
		const MemRegionInfo *Seg = Ctx.Module->FindAddressRegion(C.Address);
		if (!Seg || !Seg->IsWriteable() || Seg->IsExecutable())
			return false;

		C.Add("segment", 0.0f, Seg->GetPathName());

		char Buf[96];
		std::snprintf(Buf, sizeof(Buf), "%u sites, rank %u of %u", Info.Count, Ctx.RankOf(C.Address), Ctx.RankedCount);
		C.Add("access", 0.0f, Buf);
		return true;
	}

} // namespace anduefker::analyzer
