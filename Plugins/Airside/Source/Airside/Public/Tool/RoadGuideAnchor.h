#pragma once

#include "CoreMinimal.h"
#include "Tool/SnapGuideChain.h"

class URoadNetwork;

/**
 * The network facts a guide anchor is built from, shared by every tool that has one.
 *
 * ONE LIST, NOT TWO. FRoadDrawTool's chaining anchor and FEditTool's drag anchor ask the
 * graph the identical question - "which live nodes near here could I line up with, and
 * which column does each belong to" - and a second copy would drift exactly as the four
 * private answers to RoadNaming::ReferenceOf's question did before that function existed.
 * See CLAUDE.md, "lists that must agree are ONE list"; applied here to a rule rather than
 * to a table.
 */
namespace RoadGuideAnchor
{
	/**
	 * Append one FGuidePoint per DISTINCT kind of segment meeting each live node within the
	 * guide chain's search radius of Origin.
	 *
	 * ExcludeIndex is the node whose own lines pass through Origin - the one being extended
	 * FROM for a chain, the one being MOVED for a drag. Both must be left out for the same
	 * reason, and that shared reason is why this takes an index rather than each caller
	 * filtering afterwards.
	 */
	AIRSIDE_API void AddNodeCandidates(const URoadNetwork& Network, const FVector2D& Origin,
		int32 ExcludeIndex, FGuideAnchor& Out);
}
