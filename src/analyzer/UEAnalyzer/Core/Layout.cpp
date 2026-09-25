#include "Layout.h"

namespace anduefker::analyzer
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

} // namespace anduefker::analyzer
