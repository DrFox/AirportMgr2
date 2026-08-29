#include "Build/RoadMeshBuilder.h"

#include "Build/RoadProfileBands.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

namespace
{
	/** Maps -0.0 to 0.0; every other value passes through unchanged. */
	double NormalizeSignedZero(double Value)
	{
		return Value == 0.0 ? 0.0 : Value;
	}

	/** Masks for a vertex the junction owns. See AddJunction for why the blend is always 1. */
	FVector2f JunctionMasks(double Blend)
	{
		return FVector2f(static_cast<float>(FMath::Clamp(Blend, 0.0, 1.0)), 1.0f);
	}

	/**
	 * Furthest the junction's inset ring may travel toward the fan apex, as a fraction of
	 * each vertex's own distance to it.
	 *
	 * A tight corner has rim points close to the apex, and a full shoulder-width inset
	 * would take the ring past it and fold the fan inside out. Degrading to a thin ring
	 * loses the fade at that corner, which is a cosmetic loss; folding is a visible defect
	 * and a silent one.
	 */
	constexpr double MaxInsetFraction = 0.45;

	/**
	 * Below this, in uu², a triangle cannot cover a pixel at any sane texel density.
	 *
	 * A square 0.001 uu on a side. Absolute rather than texel-derived: spec section 12 (K3)
	 * assigns the drop to the mesh builder precisely because the solver has no texel scale
	 * to judge "too small" against, and neither does this - what it has is the knowledge
	 * that nothing this small is ever rasterised, whatever the scale.
	 */
	constexpr double MinTriangleArea = 1e-6;
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

	// Zero-area slivers. A pass-through node a hair off collinear emits a fan whose corner
	// has collapsed: the triangles are correctly wound and have distinct indices, so every
	// check above passes them, but they carry ~2.6e-07 uu² of area into FDynamicMesh3 and
	// its normal computation. Spec section 12 (K3) assigns this to the mesh builder rather
	// than the solver. Note an exactly collinear node never reaches here - the solver finds
	// no apex that sees its rim and declines to emit a fan at all.
	{
		const FVector3d& PA = Buffers.Positions[A];
		const FVector3d& PB = Buffers.Positions[B];
		const FVector3d& PC = Buffers.Positions[C];
		const double Area = FMath::Abs(
			0.5 * ((PB.X - PA.X) * (PC.Y - PA.Y) - (PB.Y - PA.Y) * (PC.X - PA.X)));
		if (Area < MinTriangleArea)
		{
			return;
		}
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

void FRoadMeshBuilder::AddJunction(const URoadNetwork& Network, int32 NodeIndex,
	const FJunctionResult& Junction, const TArray<FRoadSegmentId>& ArmSegments)
{
	// Triangles empty is the solver's own veto and must stay the guard here.
	//
	// It covers two distinct cases. A dead end has a 2-point rim and no fan at all; its
	// cap is built by AddSegment, where the node position and profile are both to hand.
	// But SolveBoundary ALSO leaves Triangles empty - with Boundary fully populated - when
	// no apex it tried could see the whole rim, deliberately, because "a correct empty
	// result beats a silently inverted one". Testing Boundary.Num() instead would resurrect
	// exactly the inverted fan the solver declined to emit, and the fan below trusts the
	// star-shaped guarantee that veto stands for.
	if (!Junction.bValid || Junction.Triangles.Num() == 0 || Junction.Boundary.Num() < 4)
	{
		return;
	}

	(void)NodeIndex;   // identity is the caller's; kept in the signature for diagnostics

	// Boundary holds the rim followed by the fan apex.
	const int32 ApexSlot = Junction.Boundary.Num() - 1;
	const FVector2D Apex = Junction.Boundary[ApexSlot];

	// The widest shoulder among this node's arms sets the inset. Different arms may carry
	// different profiles; the ring is one loop, so it takes the largest.
	double ShoulderWidth = 0.0;
	for (const FRoadSegmentId ArmSegment : ArmSegments)
	{
		const FRoadSegment* Seg = Network.GetSegment(ArmSegment);
		const URoadProfile* ArmProfile = Seg ? Seg->Profile.Get() : nullptr;
		if (ArmProfile == nullptr || ArmProfile->Bands.Num() == 0)
		{
			continue;
		}
		if (ArmProfile->Bands[0].Type == ERoadBandType::Shoulder)
		{
			ShoulderWidth = FMath::Max(ShoulderWidth, ArmProfile->Bands[0].Width);
		}
		if (ArmProfile->Bands.Last().Type == ERoadBandType::Shoulder)
		{
			ShoulderWidth = FMath::Max(ShoulderWidth, ArmProfile->Bands.Last().Width);
		}
	}

	// Rebuild the rim with each arm's band points inserted along its cut line. The solver's
	// own Triangles array indexes the ORIGINAL boundary, so it cannot be reused once points
	// are inserted - the fan is rebuilt below instead.
	TArray<FVector2D> Rim;
	Rim.Reserve(ApexSlot * 2);

	for (int32 Slot = 0; Slot < ApexSlot; ++Slot)
	{
		Rim.Add(Junction.Boundary[Slot]);

		// SolveBoundary emits each arm's RightCut immediately followed by its LeftCut, so
		// a matching adjacent pair identifies that arm's cut line. Matched bitwise: these
		// are the same values, not merely nearby ones.
		const int32 NextSlot = Slot + 1;
		if (NextSlot >= ApexSlot)
		{
			continue;
		}

		for (int32 ArmIndex = 0; ArmIndex < Junction.Arms.Num(); ++ArmIndex)
		{
			const FJunctionArmResult& Arm = Junction.Arms[ArmIndex];
			const bool bIsCutLine =
				Junction.Boundary[Slot].X == Arm.RightCut.X &&
				Junction.Boundary[Slot].Y == Arm.RightCut.Y &&
				Junction.Boundary[NextSlot].X == Arm.LeftCut.X &&
				Junction.Boundary[NextSlot].Y == Arm.LeftCut.Y;

			if (!bIsCutLine || !ArmSegments.IsValidIndex(ArmIndex))
			{
				continue;
			}

			const FRoadSegment* ArmSegment = Network.GetSegment(ArmSegments[ArmIndex]);
			const URoadProfile* ArmProfile = ArmSegment ? ArmSegment->Profile.Get() : nullptr;
			const FRoadProfileBands Bands = FRoadProfileBands::FromProfile(ArmProfile);

			// Interior boundaries only: 0 and 1 are the cut vertices already in the rim.
			for (int32 Boundary = 1; Boundary + 1 < Bands.Alphas.Num(); ++Boundary)
			{
				Rim.Add(CutLinePoint(Arm.RightCut, Arm.LeftCut, Bands.Alphas[Boundary]));
			}
			break;
		}
	}

	// Weld the rim, its inset ring and the apex. Cut vertices and band points are already
	// owned by their segments, so these attributes are discarded for them; they land on arc
	// samples, the ring and the apex. Full junction blend on every one, so no marking can
	// reach a junction.
	//
	// The ring is BOUNDARY GEOMETRY WITH NO CURRENT EFFECT. It was built to give a shoulder
	// fade somewhere to end; that fade is gone - an airport's surfaces meet at hard material
	// lines, not alpha ramps. The ring stays because it is exactly the boundary between a
	// junction's outer shoulder band and its interior, which is what per-band materials
	// need next. Until then it is a coplanar subdivision of a single-material surface:
	// invisible, and cheaper to keep than to rebuild.
	TArray<int32> RimIndices;
	TArray<int32> RingIndices;
	RimIndices.Reserve(Rim.Num());
	RingIndices.Reserve(Rim.Num());

	for (int32 Slot = 0; Slot < Rim.Num(); ++Slot)
	{
		const FVector2D& Point = Rim[Slot];

		RimIndices.Add(WeldVertex(Point, FVector2f(0.0f, 0.0f), JunctionMasks(1.0)));

		// Ring: one shoulder-width toward the apex, clamped so it can never reach or pass
		// it. The solver guarantees the rim is star-shaped about the apex, so a straight
		// move toward it stays inside the polygon and cannot self-intersect - which a
		// general inward polygon offset would, at any sufficiently tight corner.
		//
		// With no shoulder the inset is zero, the ring point IS the rim point, and it welds
		// to the same index; every strip triangle below is then index-degenerate and
		// dropped, leaving exactly the plain apex fan this used to build.
		const FVector2D ToApex = Apex - Point;
		const double Distance = ToApex.Size();
		const double Inset = FMath::Min(ShoulderWidth, Distance * MaxInsetFraction);
		const FVector2D RingPoint = (Distance > UE_KINDA_SMALL_NUMBER)
			? Point + (ToApex / Distance) * Inset
			: Point;

		RingIndices.Add(WeldVertex(RingPoint, FVector2f(0.0f, 0.0f), JunctionMasks(1.0)));
	}

	const int32 ApexIndex = WeldVertex(Apex, FVector2f(0.0f, 0.0f), JunctionMasks(1.0));

	// Rim -> ring as a quad strip, then ring -> apex as a fan. The solver validates that
	// the rim is star-shaped about the apex before emitting a fan at all, and the ring lies
	// on the straight lines from rim to apex, so every triangle here is well formed.
	for (int32 Slot = 0; Slot < RimIndices.Num(); ++Slot)
	{
		const int32 Next = (Slot + 1) % RimIndices.Num();

		AddTriangle(RimIndices[Slot], RimIndices[Next], RingIndices[Next]);
		AddTriangle(RimIndices[Slot], RingIndices[Next], RingIndices[Slot]);
		AddTriangle(ApexIndex, RingIndices[Slot], RingIndices[Next]);
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

	const FRoadProfileBands Bands = FRoadProfileBands::FromProfile(SegProfile);
	const int32 RailCount = Bands.Alphas.Num();

	// Rails[Boundary][Step]. Boundary 0 is the right edge and the last is the left, so the
	// outermost two reproduce the stored cut vertices exactly and weld as they always did.
	TArray<TArray<int32>> Rails;
	Rails.SetNum(RailCount);
	for (TArray<int32>& Rail : Rails)
	{
		Rail.Reserve(Steps + 1);
	}

	for (int32 Step = 0; Step <= Steps; ++Step)
	{
		const double Alpha = static_cast<double>(Step) / static_cast<double>(Steps);
		const float Along = static_cast<float>(Alpha * RibbonLength);

		// 1 at both ends, 0 everywhere inside, so the centreline is full down the middle
		// and gone by the time it reaches a junction.
		//
		// The end cross-sections are the ones SHARED with a junction, so writing 1 there is
		// what makes the whole junction fan unmarked: every fan vertex then carries blend 1
		// and nothing in it can reach the marking mask. Leaving them at 0 puts a band across
		// each fan triangle where the lateral has crossed into the marking width but the
		// fade has not yet reached zero - which paints a smear into every junction, worst on
		// the inside of a bend where the fillet arc is tightest.
		const bool bIsEnd = (Step == 0) || (Step == Steps);
		const double JunctionBlend = bIsEnd ? 1.0 : 0.0;

		// The cross-section's own two ends. At Step 0 and Step Steps these ARE the stored
		// cut vertices, untouched; in between they are ours to interpolate.
		const FVector2D RightAt = (Step == 0) ? RightStart
			: (Step == Steps) ? RightEnd
			: FMath::Lerp(RightStart, RightEnd, Alpha);
		const FVector2D LeftAt = (Step == 0) ? LeftStart
			: (Step == Steps) ? LeftEnd
			: FMath::Lerp(LeftStart, LeftEnd, Alpha);

		for (int32 Boundary = 0; Boundary < RailCount; ++Boundary)
		{
			const FVector2D Point = CutLinePoint(RightAt, LeftAt, Bands.Alphas[Boundary]);
			Rails[Boundary].Add(WeldVertex(
				Point,
				FVector2f(Bands.Laterals[Boundary], Along),
				FVector2f(static_cast<float>(JunctionBlend), 1.0f)));
		}
	}

	// One quad strip per band, per step.
	for (int32 Boundary = 0; Boundary + 1 < RailCount; ++Boundary)
	{
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			const int32 R0 = Rails[Boundary][Step];
			const int32 R1 = Rails[Boundary][Step + 1];
			const int32 L0 = Rails[Boundary + 1][Step];
			const int32 L1 = Rails[Boundary + 1][Step + 1];

			// Counter-clockwise seen from +Z; AddTriangle swaps for Unreal's winding.
			AddTriangle(R0, R1, L1);
			AddTriangle(R0, L1, L0);
		}
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

	// A cap carries the SAME cross-section as the ribbon it closes, band for band.
	//
	// It used to be built from the two outer rails alone, on the reasoning that a cap is a
	// flat end rather than a length of road and needs no subdivision. That is wrong the
	// moment bands mean anything. It was caught through the shoulder fade, which made every
	// corner of such a cap transparent so the road visibly stopped at its trimmed cut and
	// only reached its node once a second click turned it into a junction. The fade is gone
	// and the bug it exposed is not: per-band materials would paint the whole cap in the
	// outermost band's material, one band's worth of road end wearing the shoulder's
	// surface.
	auto BuildCapRail = [this, &Bands, RailCount](
		const FVector2D& CapRight, const FVector2D& CapLeft, float Along)
	{
		TArray<int32> Rail;
		Rail.Reserve(RailCount);
		for (int32 Boundary = 0; Boundary < RailCount; ++Boundary)
		{
			Rail.Add(WeldVertex(
				CutLinePoint(CapRight, CapLeft, Bands.Alphas[Boundary]),
				FVector2f(Bands.Laterals[Boundary], Along),
				FVector2f(0.0f, 1.0f)));
		}
		return Rail;
	};

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
		const TArray<int32> CapRail = BuildCapRail(CapRight, CapLeft, 0.0f);

		for (int32 Boundary = 0; Boundary + 1 < RailCount; ++Boundary)
		{
			const int32 R0 = CapRail[Boundary];
			const int32 R1 = Rails[Boundary][0];
			const int32 L1 = Rails[Boundary + 1][0];
			const int32 L0 = CapRail[Boundary + 1];

			AddTriangle(R0, R1, L1);
			AddTriangle(R0, L1, L0);
		}
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
		const TArray<int32> CapRail = BuildCapRail(CapRight, CapLeft, CapAlong);

		for (int32 Boundary = 0; Boundary + 1 < RailCount; ++Boundary)
		{
			const int32 R0 = Rails[Boundary][Steps];
			const int32 R1 = CapRail[Boundary];
			const int32 L1 = CapRail[Boundary + 1];
			const int32 L0 = Rails[Boundary + 1][Steps];

			AddTriangle(R0, R1, L1);
			AddTriangle(R0, L1, L0);
		}
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
		const TArray<FRoadSegmentId>* ArmSegments = Solved.NodeArmSegments.Find(Pair.Key);
		static const TArray<FRoadSegmentId> Empty;
		AddJunction(Network, Pair.Key, Pair.Value, ArmSegments ? *ArmSegments : Empty);
	}
}

void FRoadMeshBuilder::Emit(IRoadMeshSink& Sink) const
{
	Sink.Accept(Buffers);
}
