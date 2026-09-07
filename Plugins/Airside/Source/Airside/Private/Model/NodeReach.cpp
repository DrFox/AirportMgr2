#include "Model/NodeReach.h"

#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Solve/GuidelineGeom.h"

namespace
{
	/**
	 * The edge as a polyline walked AWAY from Node. A Bezier reversed is the same curve, so
	 * swapping the ends is all the reversal takes.
	 */
	bool SampleOutward(const URoadNetwork& Network, FGuidelineNodeId Node, const FGuidelineEdge& Edge,
		TArray<FVector2D>& OutPoints)
	{
		const FGuidelineNode* Near = Network.GetGuidelineNode(Node);
		const FGuidelineNode* Far = Network.GetGuidelineNode(Edge.A == Node ? Edge.B : Edge.A);
		if (Near == nullptr || Far == nullptr)
		{
			return false;
		}
		GuidelineGeom::Sample(Near->Position, Edge.Control, Far->Position, OutPoints);
		return OutPoints.Num() >= 2;
	}
}

double NodeReach::Compute(const URoadNetwork& Network, FGuidelineNodeId Node,
	FGuidelineEdgeId Edge, double Footprint)
{
	// F/2 IS THE FLOOR, never less: it is what the claim pass held before reach existed,
	// and a node that meets nothing else still has a body standing on it.
	const double Floor = Footprint * 0.5;

	const FGuidelineEdge* Own = Network.GetGuidelineEdge(Edge);
	if (Own == nullptr || (Own->A != Node && Own->B != Node))
	{
		return Floor;
	}
	TArray<FVector2D> OwnLine;
	if (!SampleOutward(Network, Node, *Own, OwnLine))
	{
		return Floor;
	}
	const double OwnLength = GuidelineGeom::PolylineLength(OwnLine);

	// A SIXTEENTH OF A FOOTPRINT per sample, and the answer is one sample PAST the last
	// pair that was still too close - so it errs long, never short. Sixteen is enough that
	// the straight-continuation case lands on exactly F/2 (samples at 0, F/16 ... 7F/16
	// are within F; the next one is not) and the arc case is within 6% of the true crossing.
	const double Step = FMath::Max(Footprint / 16.0, 1.0);

	const FGuidelineNode* At = Network.GetGuidelineNode(Node);
	if (At == nullptr)
	{
		return Floor;
	}

	double Reach = Floor;
	for (const FGuidelineEdgeId OtherId : At->Incident)
	{
		const FGuidelineEdge* Other = OtherId == Edge ? nullptr : Network.GetGuidelineEdge(OtherId);
		TArray<FVector2D> OtherLine;
		if (Other == nullptr || !SampleOutward(Network, Node, *Other, OtherLine))
		{
			continue;
		}

		// THE LAST too-close pair, not the first clear one: two curves can touch, part and
		// touch again, and a body released between the two touches would be released into
		// the second. The scan is bounded by the shorter edge because beyond its far node
		// the other line is somebody else's edge, with its own node and its own reach.
		const double Limit = FMath::Min(OwnLength, GuidelineGeom::PolylineLength(OtherLine));
		double Hug = 0.0;
		for (double S = 0.0; S <= Limit; S += Step)
		{
			FVector2D Mine, Theirs;
			double Heading = 0.0;
			if (!GuidelineGeom::PointAtDistance(OwnLine, S, Mine, Heading)
				|| !GuidelineGeom::PointAtDistance(OtherLine, S, Theirs, Heading))
			{
				break;
			}
			if (FVector2D::Distance(Mine, Theirs) < Footprint)
			{
				Hug = S + Step;
			}
		}
		Reach = FMath::Max(Reach, Hug);
	}

	// Capped at the edge: a reach past the far node is that node's business.
	return FMath::Min(Reach, FMath::Max(Floor, OwnLength));
}

double FNodeReachCache::Get(const URoadNetwork& Network, FGuidelineNodeId Node, FGuidelineEdgeId Edge, double Footprint)
{
	if (For != &Network || Revision != Network.GetGuidelineRevision())
	{
		Entries.Reset();
		For = &Network;
		Revision = Network.GetGuidelineRevision();
	}

	FKey Key;
	Key.Node = Node.Index;
	Key.Edge = Edge.Index;
	Key.Footprint = FMath::RoundToInt32(Footprint);
	if (const double* Found = Entries.Find(Key))
	{
		return *Found;
	}
	const double Reach = NodeReach::Compute(Network, Node, Edge, Footprint);
	Entries.Add(Key, Reach);
	return Reach;
}

void FNodeReachCache::Invalidate()
{
	Entries.Reset();
	For = nullptr;
	Revision = 0;
}
