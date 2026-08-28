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

	/**
	 * Masks for a vertex a segment owns. Blend is 1 at the ends and 0 in the interior, so
	 * markings run down the middle of a segment and fade out before its junctions.
	 *
	 * The end vertices are the ones SHARED with a junction, so writing 1 there is what
	 * makes the whole junction fan unmarked: every fan vertex then carries blend 1 and
	 * nothing in it can reach the marking mask. Leaving them at 0 puts a band across each
	 * fan triangle where the lateral has crossed into the marking width but the fade has
	 * not yet reached zero - which paints a smear into every junction, worst on the inside
	 * of a bend where the fillet arc is tightest.
	 */
	FVector2f SegmentMasks(double JunctionBlend = 0.0)
	{
		return FVector2f(static_cast<float>(FMath::Clamp(JunctionBlend, 0.0, 1.0)), 1.0f);
	}

	/** Masks for a vertex the junction owns. See AddJunction for why the blend is always 1. */
	FVector2f JunctionMasks(double Blend)
	{
		return FVector2f(static_cast<float>(FMath::Clamp(Blend, 0.0, 1.0)), 1.0f);
	}
}

FRoadMeshBuilder::FRoadMeshBuilder(double InZHeight, double InTexelsPerUnit)
	: ZHeight(InZHeight)
	, TexelsPerUnit(InTexelsPerUnit > 0.0 ? InTexelsPerUnit : 1.0)
{
}

int32 FRoadMeshBuilder::WeldVertex(const FVector2D& Point, const FVector2f& InUV1, const FVector2f& InUV2)
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
	Buffers.UV2.Add(InUV2);

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

	// Emitted B and C SWAPPED, because Unreal's front face is the opposite winding to the
	// mathematical convention every caller here uses.
	//
	// Callers build counter-clockwise as seen from +Z, which is correct maths and is what
	// the solver's polygons are. Unreal is left-handed: VectorUtil::Normal computes
	// cross(C-A, B-A) - the negation of the standard cross product, with a comment in the
	// engine saying exactly why - so a counter-clockwise triangle faces DOWN and is
	// backface-culled from above.
	//
	// This went unnoticed from slice 2a until the first genuinely lit material, because
	// the placeholder colour override substitutes Unreal's vertex-colour debug material,
	// which is two-sided. Every winding check - the tests, the hand-derivations, the
	// review - measured the maths convention and agreed with each other while disagreeing
	// with the rasteriser.
	Buffers.Indices.Add(A);
	Buffers.Indices.Add(C);
	Buffers.Indices.Add(B);
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
	// samples and the apex, which no segment touches.
	//
	// EVERY vertex the junction owns takes FULL junction blend, arc samples and apex
	// alike. Blending by position along the fan instead - 0 at the rim, 1 at the apex -
	// reads well and is badly wrong: an arc sample is welded by nobody, so it keeps this
	// UV1 of (0, 0), which the marking mask reads as "dead on the centreline". At a blend
	// of 0 nothing fades it, so every fillet renders as a solid fan of centreline paint,
	// and with 12 arc samples per corner those vertices dominate a bend's rim.
	//
	// The shared cut vertices carry blend 1 too, written by the segment - see
	// SegmentMasks. So every vertex of every fan triangle is fully blended and no part of
	// a junction can reach the marking mask. The taper happens on the segment side
	// instead, where the blend ramps from 0 in the middle to 1 at the ends.
	TArray<int32> Mapped;
	Mapped.Reserve(Junction.Boundary.Num());
	for (const FVector2D& Point : Junction.Boundary)
	{
		Mapped.Add(WeldVertex(Point, FVector2f(0.0f, 0.0f), JunctionMasks(1.0)));
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

	// At least three, whatever the caller asked for. The blend has to ramp from 1 at each
	// end to 0 in the middle, and with a single quad there is no interior cross-section to
	// hold the 0 - every vertex would be an end, so the whole segment would fade and no
	// centreline would survive anywhere.
	const int32 Steps = FMath::Max(RibbonSegments, 3);

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

		// 1 at both ends, 0 everywhere inside, so the centreline is full down the middle
		// and gone by the time it reaches a junction.
		const bool bIsEnd = (Step == 0) || (Step == Steps);
		const FVector2f Masks = SegmentMasks(bIsEnd ? 1.0 : 0.0);

		if (Step == 0)
		{
			LeftRail.Add(WeldVertex(LeftStart, FVector2f(LateralLeft, Along), Masks));
			RightRail.Add(WeldVertex(RightStart, FVector2f(-LateralRight, Along), Masks));
		}
		else if (Step == Steps)
		{
			LeftRail.Add(WeldVertex(LeftEnd, FVector2f(LateralLeft, Along), Masks));
			RightRail.Add(WeldVertex(RightEnd, FVector2f(-LateralRight, Along), Masks));
		}
		else
		{
			// Interior samples are ours alone and may be interpolated freely; only the
			// ends are shared with a junction.
			LeftRail.Add(WeldVertex(FMath::Lerp(LeftStart, LeftEnd, Alpha),
				FVector2f(LateralLeft, Along), Masks));
			RightRail.Add(WeldVertex(FMath::Lerp(RightStart, RightEnd, Alpha),
				FVector2f(-LateralRight, Along), Masks));
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
		const int32 R0 = WeldVertex(CapRight, FVector2f(-LateralRight, 0.0f), SegmentMasks(0.0));
		const int32 R1 = RightRail[0];
		const int32 L1 = LeftRail[0];
		const int32 L0 = WeldVertex(CapLeft, FVector2f(LateralLeft, 0.0f), SegmentMasks(0.0));

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
		const int32 R1 = WeldVertex(CapRight, FVector2f(-LateralRight, CapAlong), SegmentMasks(0.0));
		const int32 L1 = WeldVertex(CapLeft, FVector2f(LateralLeft, CapAlong), SegmentMasks(0.0));
		const int32 L0 = LeftRail[Steps];

		AddTriangle(R0, R1, L1);
		AddTriangle(R0, L1, L0);
	}
}

void FRoadMeshBuilder::Build(const URoadNetwork& Network, const FRoadSolveResult& Solved, int32 RibbonSegments)
{
	// Segments first. See the header: this ordering is the whole reason this function
	// exists rather than leaving each caller to remember it.
	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		if (!Segments[Index].bAlive)
		{
			continue;
		}

		FRoadSegmentId SegmentId;
		SegmentId.Index = Index;
		SegmentId.Generation = Segments[Index].Generation;
		AddSegment(Network, SegmentId, RibbonSegments);
	}

	for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
	{
		AddJunction(Pair.Value);
	}
}

void FRoadMeshBuilder::Emit(IRoadMeshSink& Sink) const
{
	Sink.Accept(Buffers);
}
