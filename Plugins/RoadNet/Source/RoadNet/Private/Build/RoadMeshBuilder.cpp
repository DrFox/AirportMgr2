#include "Build/RoadMeshBuilder.h"

#include "Model/RoadNetwork.h"
#include "Solve/RoadGeom.h"

FRoadMeshBuilder::FRoadMeshBuilder(double InZHeight)
	: ZHeight(InZHeight)
{
}

int32 FRoadMeshBuilder::WeldVertex(const FVector2D& Point)
{
	// FVector2D::operator== is an exact comparison and its GetTypeHash covers both
	// components, so this map welds on bits, not on proximity. That is deliberate:
	// a tolerance here would silently paper over a solver that had stopped sharing
	// its vertices, which is the exact failure this design exists to prevent.
	if (const int32* Existing = WeldMap.Find(Point))
	{
		return *Existing;
	}

	const int32 NewIndex = Buffers.Positions.Add(FVector3d(Point.X, Point.Y, ZHeight));
	WeldMap.Add(Point, NewIndex);
	return NewIndex;
}

void FRoadMeshBuilder::AddTriangle(int32 A, int32 B, int32 C)
{
	// A degenerate triangle contributes nothing and upsets downstream normal
	// computation, so drop it rather than emit it.
	if (A == B || B == C || A == C)
	{
		return;
	}
	Buffers.Indices.Add(A);
	Buffers.Indices.Add(B);
	Buffers.Indices.Add(C);
}

void FRoadMeshBuilder::AddJunction(const FJunctionResult& Junction)
{
	if (!Junction.bValid || Junction.Triangles.Num() == 0)
	{
		return;
	}

	// Boundary holds the rim followed by the fan apex; Triangles indexes into it.
	// Map every boundary slot through the weld map once, then re-index.
	TArray<int32> Mapped;
	Mapped.Reserve(Junction.Boundary.Num());
	for (const FVector2D& Point : Junction.Boundary)
	{
		Mapped.Add(WeldVertex(Point));
	}

	for (int32 Slot = 0; Slot + 2 < Junction.Triangles.Num(); Slot += 3)
	{
		AddTriangle(
			Mapped[Junction.Triangles[Slot]],
			Mapped[Junction.Triangles[Slot + 1]],
			Mapped[Junction.Triangles[Slot + 2]]);
	}
}

void FRoadMeshBuilder::AddSegment(const URoadNetwork& Network, FRoadSegmentId SegmentId, int32 RibbonSegments)
{
	const FRoadSegment* Segment = Network.GetSegment(SegmentId);
	if (Segment == nullptr || !Segment->bSolved)
	{
		return;
	}

	const int32 Steps = FMath::Max(RibbonSegments, 1);

	// The two ends come from the model verbatim. Never recompute them: these are the
	// same values the junction boundary holds, and only bitwise equality welds.
	const FVector2D LeftStart  = Segment->LeftCutA;
	const FVector2D RightStart = Segment->RightCutA;

	// End B's cut line is authored from B's point of view, so its left is this
	// segment's right when walking A to B. Swap so the ribbon does not cross itself.
	const FVector2D LeftEnd  = Segment->RightCutB;
	const FVector2D RightEnd = Segment->LeftCutB;

	TArray<int32> LeftRail;
	TArray<int32> RightRail;
	LeftRail.Reserve(Steps + 1);
	RightRail.Reserve(Steps + 1);

	for (int32 Step = 0; Step <= Steps; ++Step)
	{
		if (Step == 0)
		{
			LeftRail.Add(WeldVertex(LeftStart));
			RightRail.Add(WeldVertex(RightStart));
		}
		else if (Step == Steps)
		{
			LeftRail.Add(WeldVertex(LeftEnd));
			RightRail.Add(WeldVertex(RightEnd));
		}
		else
		{
			// Interior samples are ours alone and may be interpolated freely; only the
			// ends are shared with a junction.
			const double Alpha = static_cast<double>(Step) / static_cast<double>(Steps);
			LeftRail.Add(WeldVertex(FMath::Lerp(LeftStart, LeftEnd, Alpha)));
			RightRail.Add(WeldVertex(FMath::Lerp(RightStart, RightEnd, Alpha)));
		}
	}

	for (int32 Step = 0; Step < Steps; ++Step)
	{
		const int32 R0 = RightRail[Step];
		const int32 R1 = RightRail[Step + 1];
		const int32 L0 = LeftRail[Step];
		const int32 L1 = LeftRail[Step + 1];

		// Wound counter-clockwise seen from +Z so the surface faces up.
		AddTriangle(R0, R1, L1);
		AddTriangle(R0, L1, L0);
	}
}

void FRoadMeshBuilder::Emit(IRoadMeshSink& Sink) const
{
	Sink.Accept(Buffers);
}
