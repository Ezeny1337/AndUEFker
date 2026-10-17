#include "../UEAnalyzer.h"

#include "GUObjectArrayStrategy.h"
#include "NameStrategy.h"
#include "ObjObjectsStrategy.h"

namespace nextdumper::analyzer::Targets
{
	std::vector<StrategyPtr> CreateAll()
	{
		std::vector<StrategyPtr> Out;
		Out.push_back(MakeObjObjectsStrategy());
		Out.push_back(MakeNameStrategy());
		Out.push_back(MakeGUObjectArrayStrategy());
		return Out;
	}
} // namespace nextdumper::analyzer::Targets
