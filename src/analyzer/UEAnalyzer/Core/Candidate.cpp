#include "Candidate.h"

#include <cstdio>

namespace anduefker::analyzer
{

	const char *EFindMethodToString(EFindMethod M)
	{
		switch (M)
		{
		case EFindMethod::Anchored:
			return "anchored";
		case EFindMethod::Pattern:
			return "pattern";
		case EFindMethod::Statistical:
			return "statistical";
		case EFindMethod::Symbol:
			return "symbol";
		default:
			return "unknown";
		}
	}

	bool ParseFindMethod(const std::string &Text, std::optional<EFindMethod> &Out)
	{
		if (Text == "auto")
		{
			Out.reset();
			return true;
		}
		if (Text == "anchor" || Text == "anchored")
		{
			Out = EFindMethod::Anchored;
			return true;
		}
		if (Text == "pattern")
		{
			Out = EFindMethod::Pattern;
			return true;
		}
		if (Text == "leads" || Text == "statistical")
		{
			Out = EFindMethod::Statistical;
			return true;
		}
		return false;
	}

	std::string Candidate::ToString() const
	{
		char Buf[128];
		std::snprintf(Buf, sizeof(Buf), "0x%llX  %.3f  %s", static_cast<unsigned long long>(Address), Confidence, Method.c_str());
		std::string Out = Buf;
		for (const auto &S : Signals)
		{
			Out += "\n    ";
			Out += S.Name;
			if (!S.Detail.empty())
			{
				Out += " (";
				Out += S.Detail;
				Out += ")";
			}
		}
		return Out;
	}

} // namespace anduefker::analyzer
