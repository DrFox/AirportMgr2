#include "Tool/RoadSnap.h"

#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Solve/RoadGeom.h"

bool FRoadNodeSnapRule::Resolve(const URoadNetwork& Network, const FVector2D& Cursor,
	const FRoadSnapSettings& Settings, FRoadSnapResult& Out) const
{
	if (Settings.NodeRadius <= 0.0)
	{
		return false;
	}

	// Compared squared throughout, so a wide radius costs no square roots.
	double BestSquared = Settings.NodeRadius * Settings.NodeRadius;
	int32 Best = INDEX_NONE;

	const TArray<FRoadNode>& Nodes = Network.GetNodes();
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		if (!Nodes[Index].bAlive)
		{
			continue;
		}

		const double DistanceSquared = FVector2D::DistSquared(Nodes[Index].Position, Cursor);
		if (DistanceSquared <= BestSquared)
		{
			BestSquared = DistanceSquared;
			Best = Index;
		}
	}

	if (Best == INDEX_NONE)
	{
		return false;
	}

	Out.Kind = ERoadSnapKind::Node;
	Out.Node.Index = Best;
	Out.Node.Generation = Nodes[Best].Generation;

	// The node's stored position, copied - never the cursor, and never recomputed. A
	// click that reuses a node has to land on the coordinates the graph already holds.
	Out.Position = Nodes[Best].Position;
	return true;
}

bool FRoadSegmentSnapRule::Resolve(const URoadNetwork& Network, const FVector2D& Cursor,
	const FRoadSnapSettings& Settings, FRoadSnapResult& Out) const
{
	if (!Settings.bSnapToSegments || Settings.SegmentRadius <= 0.0)
	{
		return false;
	}

	double BestSquared = Settings.SegmentRadius * Settings.SegmentRadius;
	int32 Best = INDEX_NONE;
	double BestT = 0.0;
	FVector2D BestPoint = FVector2D::ZeroVector;

	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegment& Segment = Segments[Index];
		if (!Segment.bAlive)
		{
			continue;
		}

		const FRoadNode* EndA = Network.GetNode(Segment.A);
		const FRoadNode* EndB = Network.GetNode(Segment.B);
		if (EndA == nullptr || EndB == nullptr)
		{
			continue;
		}

		// The CHORD, not the Bezier. Every segment this tool can author is straight, and
		// AddSegment interpolates a segment's interior in a straight line regardless, so
		// the chord is the road. Curved authoring has to bring the curve's own closest
		// point with it; approximating a curve by its chord here would put the split
		// visibly off the pavement.
		const double T = RoadGeom::ClosestPointOnSegment(EndA->Position, EndB->Position, Cursor);

		// Exactly 0 or 1 is the clamp reporting that the closest point IS an endpoint.
		// That neighbourhood belongs to the node rule, which has already had its turn and
		// declined - so passing here means Free, which is right: the cursor is off the end
		// of the road, not on it.
		if (T <= 0.0 || T >= 1.0)
		{
			continue;
		}

		const FVector2D Point = FMath::Lerp(EndA->Position, EndB->Position, T);
		const double DistanceSquared = FVector2D::DistSquared(Point, Cursor);
		if (DistanceSquared > BestSquared)
		{
			continue;
		}

		// A split this close to an end leaves a stub the solver cannot trim: its two cut
		// lines would cross, and the junction it feeds would fold through itself.
		if (FVector2D::Distance(Point, EndA->Position) < Settings.MinSplitFromEndpoint
			|| FVector2D::Distance(Point, EndB->Position) < Settings.MinSplitFromEndpoint)
		{
			continue;
		}

		BestSquared = DistanceSquared;
		Best = Index;
		BestT = T;
		BestPoint = Point;
	}

	if (Best == INDEX_NONE)
	{
		return false;
	}

	Out.Kind = ERoadSnapKind::Segment;
	Out.Segment.Index = Best;
	Out.Segment.Generation = Segments[Best].Generation;
	Out.SegmentT = BestT;
	Out.Position = BestPoint;
	return true;
}

FRoadSnapChain::FRoadSnapChain()
{
	AddRule(MakeUnique<FRoadNodeSnapRule>());
	AddRule(MakeUnique<FRoadSegmentSnapRule>());
}

void FRoadSnapChain::AddRule(TUniquePtr<IRoadSnapRule> Rule)
{
	if (Rule.IsValid())
	{
		Rules.Add(MoveTemp(Rule));
	}
}

FRoadSnapResult FRoadSnapChain::Resolve(const URoadNetwork& Network, const FVector2D& Cursor,
	const FRoadSnapSettings& Settings) const
{
	for (const TUniquePtr<IRoadSnapRule>& Rule : Rules)
	{
		FRoadSnapResult Claimed;
		if (Rule->Resolve(Network, Cursor, Settings, Claimed))
		{
			return Claimed;
		}
	}

	FRoadSnapResult Free;
	Free.Kind = ERoadSnapKind::Free;
	Free.Position = Cursor;
	return Free;
}
