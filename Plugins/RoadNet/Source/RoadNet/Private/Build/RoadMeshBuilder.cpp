#include "Build/RoadMeshBuilder.h"

#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

namespace
{
	/** Maps -0.0 to 0.0; every other value passes through unchanged. */
	double NormalizeSignedZero(double Value)
	{
		return Value == 0.0 ? 0.0 : Value;
	}

	/** Vertex colour for a vertex a segment owns: no junction blend, fully opaque. */
	FColor SegmentColour()
	{
		return FColor(0, 0, 0, 255);
	}

	/** Vertex colour for a junction's own vertices, blended by how far into the fan they are. */
	FColor JunctionColour(double Blend)
	{
		const uint8 G = static_cast<uint8>(FMath::Clamp(Blend, 0.0, 1.0) * 255.0 + 0.5);
		return FColor(0, G, 0, 255);
	}
}

FRoadMeshBuilder::FRoadMeshBuilder(double InZHeight, double InTexelsPerUnit)
	: ZHeight(InZHeight)
	, TexelsPerUnit(InTexelsPerUnit > 0.0 ? InTexelsPerUnit : 1.0)
{
}

int32 FRoadMeshBuilder::WeldVertex(const FVector2D& Point, const FVector2f& InUV1, const FColor& InColor)
{
	// FVector2D::operator== compares X and Y by value, under which -0.0 == +0.0. But
	// GetTypeHash(const TVector2<T>&) is a CRC over the raw bytes, so -0.0 and +0.0 hash
	// to different buckets - the one case where this map's key equality and its hash
	// disagree. Left alone, Add(-0.0, ...) followed by Find(+0.0) misses, and the map
	// ends up holding two entries for what operator== calls one position: a duplicate
	// coincident vertex, exactly what welding on bits exists to make unrepresentable.
	// Normalising signed zero on the way in closes that gap without adding a tolerance -
	// a tolerance here would paper over a solver that had stopped sharing its vertices.
	const FVector2D Key(NormalizeSignedZero(Point.X), NormalizeSignedZero(Point.Y));

	if (const int32* Existing = WeldMap.Find(Key))
	{
		// First writer wins - see the header for why this is the contract rather than a
		// convenience, and why it forces segments to be added before junctions.
		return *Existing;
	}

	const int32 NewIndex = Buffers.Positions.Add(FVector3d(Key.X, Key.Y, ZHeight));

	// UV0 is derived here and nowhere else, so two callers cannot supply different
	// world-aligned UVs for the same position.
	Buffers.UV0.Add(FVector2f(
		static_cast<float>(Key.X / TexelsPerUnit),
		static_cast<float>(Key.Y / TexelsPerUnit)));
	Buffers.UV1.Add(InUV1);
	Buffers.Colors.Add(InColor);

	WeldMap.Add(Key, NewIndex);
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
	//
	// Almost every rim vertex is a cut vertex some segment has already welded, so for
	// those these attributes are discarded - see WeldVertex. They land only on the arc
	// samples and the apex, which no segment touches. The apex carries full junction
	// blend so markings taper out toward a junction's centre rather than stopping dead
	// at the rim, which is what lets one welded vertex serve both sides.
	const int32 ApexSlot = Junction.Boundary.Num() - 1;

	TArray<int32> Mapped;
	Mapped.Reserve(Junction.Boundary.Num());
	for (int32 Slot = 0; Slot < Junction.Boundary.Num(); ++Slot)
	{
		// An arc sample is an outer-edge point and a junction has no centreline to
		// measure along, so UV1 here is meaningless by design; junction blend is what
		// stops anything reading it.
		Mapped.Add(WeldVertex(
			Junction.Boundary[Slot],
			FVector2f(0.0f, 0.0f),
			JunctionColour(Slot == ApexSlot ? 1.0 : 0.0)));
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
	if (Segment == nullptr || !Segment->bSolvedA || !Segment->bSolvedB)
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

	const URoadProfile* SegProfile = Segment->Profile;
	const float LateralLeft  = static_cast<float>(SegProfile ? FMath::Max(SegProfile->GetHalfWidthLeft(),  0.0) : 0.0);
	const float LateralRight = static_cast<float>(SegProfile ? FMath::Max(SegProfile->GetHalfWidthRight(), 0.0) : 0.0);

	// `along` runs from the A-end cut to the B-end cut, so it measures the ribbon rather
	// than the node-to-node distance. Markings therefore start where the surface starts.
	const double RibbonLength = FVector2D::Distance(LeftStart, LeftEnd);

	TArray<int32> LeftRail;
	TArray<int32> RightRail;
	LeftRail.Reserve(Steps + 1);
	RightRail.Reserve(Steps + 1);

	for (int32 Step = 0; Step <= Steps; ++Step)
	{
		const double Alpha = static_cast<double>(Step) / static_cast<double>(Steps);
		const float Along = static_cast<float>(Alpha * RibbonLength);

		if (Step == 0)
		{
			LeftRail.Add(WeldVertex(LeftStart, FVector2f(LateralLeft, Along), SegmentColour()));
			RightRail.Add(WeldVertex(RightStart, FVector2f(-LateralRight, Along), SegmentColour()));
		}
		else if (Step == Steps)
		{
			LeftRail.Add(WeldVertex(LeftEnd, FVector2f(LateralLeft, Along), SegmentColour()));
			RightRail.Add(WeldVertex(RightEnd, FVector2f(-LateralRight, Along), SegmentColour()));
		}
		else
		{
			// Interior samples are ours alone and may be interpolated freely; only the
			// ends are shared with a junction.
			LeftRail.Add(WeldVertex(FMath::Lerp(LeftStart, LeftEnd, Alpha),
				FVector2f(LateralLeft, Along), SegmentColour()));
			RightRail.Add(WeldVertex(FMath::Lerp(RightStart, RightEnd, Alpha),
				FVector2f(-LateralRight, Along), SegmentColour()));
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

	// Dead-end caps. A node with exactly one incident segment never gets a junction fan:
	// SolveCuts cuts it back by its own half-width "so an end cap has room" (JunctionSolver
	// comment), but a 1-arm node's rim has only 2 points, so SolveBoundary's RimCount >= 3
	// branch never runs and no fan apex is appended. AddJunction therefore has nothing to
	// build for that node. Build the cap here instead, where both node positions and the
	// profile's half-widths are available.
	// Clamped exactly like JunctionSolver.cpp clamps Arm.HalfWidthLeft/Right: a negative
	// band width must not mirror the cap to the wrong side while the ribbon end (which
	// came from the solver, already clamped) stays put.
	const double HalfWidthLeft  = static_cast<double>(LateralLeft);
	const double HalfWidthRight = static_cast<double>(LateralRight);

	const FRoadNode* NodeA = Network.GetNode(Segment->A);
	if (NodeA != nullptr && NodeA->Incident.Num() == 1)
	{
		// A's own outgoing tangent already points in the same direction the ribbon walks
		// (A toward B), so - unlike the B end below - no left/right swap is needed: this
		// mirrors LeftCutA/RightCutA's own convention directly.
		const FVector2D Tangent = Network.GetOutgoingTangent(SegmentId, Segment->A);
		const FVector2D Normal(-Tangent.Y, Tangent.X); // CCW perpendicular
		const FVector2D CapLeft  = NodeA->Position + Normal * HalfWidthLeft;
		const FVector2D CapRight = NodeA->Position - Normal * HalfWidthRight;

		// The cap sits BEFORE the ribbon's first cross-section walking A to B: node cap
		// line first, ribbon start second.
		// The cap spans from the node's own cut line to the ribbon's start, so it sits at
		// along = 0 - the surface really does begin at the node, not at the trimmed cut.
		const int32 R0 = WeldVertex(CapRight, FVector2f(-LateralRight, 0.0f), SegmentColour());
		const int32 R1 = RightRail[0];
		const int32 L1 = LeftRail[0];
		const int32 L0 = WeldVertex(CapLeft, FVector2f(LateralLeft, 0.0f), SegmentColour());

		AddTriangle(R0, R1, L1);
		AddTriangle(R0, L1, L0);
	}

	const FRoadNode* NodeB = Network.GetNode(Segment->B);
	if (NodeB != nullptr && NodeB->Incident.Num() == 1)
	{
		const FVector2D Tangent = Network.GetOutgoingTangent(SegmentId, Segment->B);
		const FVector2D Normal(-Tangent.Y, Tangent.X); // CCW perpendicular
		const FVector2D RawLeft  = NodeB->Position + Normal * HalfWidthLeft;
		const FVector2D RawRight = NodeB->Position - Normal * HalfWidthRight;

		// B's cut line is authored from B's own point of view, so its left is this
		// segment's right when walking A to B - swap, exactly like RightCutB/LeftCutB
		// above.
		const FVector2D CapLeft  = RawRight;
		const FVector2D CapRight = RawLeft;

		// The cap sits AFTER the ribbon's last cross-section walking A to B: ribbon end
		// first, node cap line second.
		const float CapAlong = static_cast<float>(RibbonLength);
		const int32 R0 = RightRail[Steps];
		const int32 R1 = WeldVertex(CapRight, FVector2f(-LateralRight, CapAlong), SegmentColour());
		const int32 L1 = WeldVertex(CapLeft, FVector2f(LateralLeft, CapAlong), SegmentColour());
		const int32 L0 = LeftRail[Steps];

		AddTriangle(R0, R1, L1);
		AddTriangle(R0, L1, L0);
	}
}

void FRoadMeshBuilder::Emit(IRoadMeshSink& Sink) const
{
	Sink.Accept(Buffers);
}
