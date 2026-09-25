#include "../UEAnalyzer.h"

#include <cstdarg>
#include <cstdio>

namespace anduefker::analyzer
{

	namespace
	{
		void AppendLine(std::string &Out, const char *Format, ...)
		{
			char Buf[512];
			va_list Args;
			va_start(Args, Format);
			std::vsnprintf(Buf, sizeof(Buf), Format, Args);
			va_end(Args);
			Out += Buf;
		}
	} // namespace

	std::string FormatExplanation(const AddressExplanation &Explanation)
	{
		std::string Out;
		AppendLine(Out, "=== 0x%llX ===\n", (unsigned long long)Explanation.Address);

		if (Explanation.bInModule)
			AppendLine(Out, "segment      %s%s%s\n", Explanation.Segment.c_str(), Explanation.bWritable ? " W" : "", Explanation.bExecutable ? " X" : "");
		else
			Out += "segment      none - the address is not in this module\n";

		if (Explanation.bReferenced)
		{
			const AccessSummary &A = Explanation.Access;
			AppendLine(Out, "accesses     %u total, %u pointer-width, %u 32-bit, rank %u of %u\n", A.Total, A.PointerWide, A.Narrow, A.Rank, A.RankedTotal);

			Out += "reached as   ";
			Out += A.bAddressTaken ? "address-taken " : "";
			Out += A.bIndirectBase ? "indirect-base " : "";
			Out += A.bLoaded ? "load " : "";
			Out += A.bStored ? "store " : "";

			Out += "\noffsets      ";
			for (int64_t Off : A.Offsets)
				AppendLine(Out, "%lld ", (long long)Off);
			Out += "\n";
		}
		else
		{
			Out += "accesses     none recorded - no code computes this address\n";
		}

		Out += "\nverification\n";
		for (const TargetAssessment &T : Explanation.Targets)
			AppendLine(Out, "  %-14s %-6s %s\n", T.Target, T.Evidence.Passed ? "PASS" : "REJECT", T.Evidence.Why.c_str());

		for (const TargetAssessment &T : Explanation.Targets)
		{
			if (!T.Layout.bValid)
				continue;

			AppendLine(Out, "\nlayout if this is %s", T.Target);
			if (T.Layout.Layout.IsKnown())
				AppendLine(Out, "  (likely %s, confidence %.2f)", T.Layout.Layout.Kind.c_str(), (double)T.Layout.Layout.Confidence);
			Out += "\n";

			if (T.Layout.Table.Offset >= 0)
				AppendLine(Out, "  element table  +0x%llX  (%u pointer-width reads)\n", (unsigned long long)T.Layout.Table.Offset, T.Layout.Table.Reads);
			for (const LayoutField &F : T.Layout.Counts)
				AppendLine(Out, "  32-bit field   +0x%llX  (%u reads)\n", (unsigned long long)F.Offset, F.Reads);
		}

		Out += "\nstanding in each target\n";
		for (const TargetAssessment &T : Explanation.Targets)
			AppendLine(Out, "  %-14s %s\n", T.Target, T.Reason.c_str());
		return Out;
	}

} // namespace anduefker::analyzer
