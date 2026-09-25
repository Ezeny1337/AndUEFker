#include "../UEAnalyzer.h"

#include "GUObjectArrayStrategy.h"
#include "NameStrategy.h"
#include "ObjObjectsStrategy.h"

namespace anduefker::analyzer::Targets
{
	std::vector<StrategyPtr> CreateAll()
	{
		std::vector<StrategyPtr> Out;
		Out.push_back(MakeObjObjectsStrategy());
		Out.push_back(MakeNameStrategy());
		Out.push_back(MakeGUObjectArrayStrategy());
		return Out;
	}
} // namespace anduefker::analyzer::Targets
