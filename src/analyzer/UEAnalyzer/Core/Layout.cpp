#include "Layout.h"

namespace nextdumper::analyzer
{

	const char *EVerdictToString(EVerdict Verdict)
	{
		switch (Verdict)
		{
		case EVerdict::Accepted:
			return "accepted";
		case EVerdict::NotWritableData:
			return "not writable data";
		case EVerdict::Unreferenced:
			return "no recorded access";
		case EVerdict::Rejected:
			return "rejected";
		}
		return "unknown";
	}

} // namespace nextdumper::analyzer
