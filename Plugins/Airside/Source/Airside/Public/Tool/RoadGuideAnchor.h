#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Tool/SnapGuideChain.h"

class URoadNetwork;
struct FRoadNode;

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

	/**
	 * Fills Out.Reference/ReferenceAt/ReferenceName from the ONE segment arriving at Node, if
	 * there is exactly one - "the road this node ends", the direction a chain extends or a drag
	 * holds. With none or several, no arm is "the" one and Out is left exactly as it came in:
	 * picking whichever segment happened to be stored first would make the guide change with an
	 * edit nobody connected to guides at all.
	 *
	 * ISSUE #303: FRoadDrawTool's chaining anchor and FEditTool's drag anchor asked this
	 * identical question TWO WAYS - FEditTool through the node's own FRoadNode::Incident (an
	 * O(degree) read), FRoadDrawTool by walking every live segment in the network (an O(N) scan,
	 * repeated on every MakeContext). Node.Incident is the SAME list FEditTool already trusted,
	 * so this drops the scan rather than choosing between the two answers - CLAUDE.md's "check
	 * where a list is CONSUMED": FRoadNode::Incident is already maintained sorted for exactly
	 * this kind of question, and nothing here re-derives it.
	 *
	 * NodeId is Node's OWN id, not Origin's exclusion or a candidate's - the one arm's far end is
	 * looked up through it, and a caller holding only a slot index reaches this via
	 * URoadNetwork::NodeIdAt the same way FRoadDrawTool and FEditTool's own callers already do.
	 */
	AIRSIDE_API void DescribeIncomingArm(const URoadNetwork& Network, const FRoadNode& Node,
		FRoadNodeId NodeId, FGuideAnchor& Out);
}
