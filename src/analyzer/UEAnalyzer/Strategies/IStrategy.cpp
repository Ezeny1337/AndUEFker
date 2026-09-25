#include "IStrategy.h"

#include "GUObjectArrayStrategy.h"
#include "NameStrategy.h"
#include "ObjObjectsStrategy.h"

namespace anduefker::analyzer
{

	// Nothing here but construction: a strategy carries its own anchors, its own
	// verification and its own scoring, so there is no table to keep in step with
	// it. Adding a target is this file plus one FindX() on UEAnalyzer.

	std::unique_ptr<ObjObjectsStrategy> MakeObjObjectsStrategy()
	{
		return std::make_unique<ObjObjectsStrategy>();
	}

	std::unique_ptr<GUObjectArrayStrategy> MakeGUObjectArrayStrategy()
	{
		return std::make_unique<GUObjectArrayStrategy>();
	}

	std::unique_ptr<NameStrategy> MakeNameStrategy()
	{
		return std::make_unique<NameStrategy>();
	}

} // namespace anduefker::analyzer
