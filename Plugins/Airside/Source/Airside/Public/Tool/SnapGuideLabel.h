#pragma once

#include "CoreMinimal.h"
#include "Solve/GuideArbiter.h"

class URoadNetwork;
struct FGuideAnchor;

/**
 * Turns a FGuideLabel back into the FString a player reads - #183.
 *
 * NEXT TO RoadNaming rather than inside it: RoadNaming answers "what to call a ROAD", and stays
 * usable by anything that just wants that, with no idea what a guide or an anchor is. Describe
 * answers "what to call a whole CANDIDATE", which needs FGuideAnchor for the two subjects
 * RoadNaming cannot resolve on its own - see ELabelSubject::GestureReference/GesturePoint - so
 * giving RoadNaming.h that dependency would be the wrong file learning about snap guides.
 *
 * CALLED ONLY FOR WINNERS. FSnapGuideChain::Resolve calls this once per surviving candidate
 * (at most two) right after Arbitrate returns, while Network and Anchor are still the same ones
 * Propose saw - never inside Propose itself, which is the whole point of #183.
 */
namespace SnapGuide
{
	AIRSIDE_API FString Describe(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FGuideLabel& Label);

	/**
	 * How many times Describe has actually run, for the #183 regression: a candidate list of any
	 * size must cost exactly Result.Winners.Num() calls, never Candidates.Num(). Free functions
	 * behind a namespace, not a class static, because Describe itself is a free function - the
	 * same reasoning RouteSearch::NodeVisitCountForTest gives for GNodeVisitCountForTest.
	 */
	AIRSIDE_API int32 DescribeCallCountForTest();
	AIRSIDE_API void ResetDescribeCallCountForTest();
}
