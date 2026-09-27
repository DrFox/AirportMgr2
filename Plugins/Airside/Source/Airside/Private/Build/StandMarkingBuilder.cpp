#include "Build/StandMarkingBuilder.h"

#include "AirsideLog.h"

#include "Algo/Reverse.h"

#include "Build/MarkingQuads.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Solve/RoadGeom.h"
#include "Solve/StandBox.h"

namespace
{
	constexpr uint8 SegA = 0x01, SegB = 0x02, SegC = 0x04, SegD = 0x08, SegE = 0x10, SegF = 0x20, SegG = 0x40;

	/** One segment's rectangle in the glyph's own (Right, Up) frame - x across, y up. */
	struct FGlyphSegmentSpec
	{
		uint8 Bit;
		double XMin, XMax, YMin, YMax;
	};

	/**
	 * The seven segments' rectangles, built fresh per call rather than as file statics
	 * because they read FStandMarkingBuilder's own constexpr figures (GlyphWidth etc) -
	 * constexpr math anyway, so there is no runtime cost to paying for this on every stand.
	 */
	TArray<FGlyphSegmentSpec, TInlineAllocator<7>> GlyphSegmentRects()
	{
		using B = FStandMarkingBuilder;
		constexpr double HW = B::GlyphWidth * 0.5;
		constexpr double HH = B::GlyphHeight * 0.5;
		constexpr double HS = B::GlyphStroke * 0.5;
		return {
			{ SegA, -HW, HW, HH - B::GlyphStroke, HH },              // top
			{ SegB, HW - B::GlyphStroke, HW, HS, HH - HS },          // upper right
			{ SegC, HW - B::GlyphStroke, HW, -HH + HS, -HS },        // lower right
			{ SegD, -HW, HW, -HH, -HH + B::GlyphStroke },            // bottom
			{ SegE, -HW, -HW + B::GlyphStroke, -HH + HS, -HS },      // lower left
			{ SegF, -HW, -HW + B::GlyphStroke, HS, HH - HS },        // upper left
			{ SegG, -HW + B::GlyphStroke, HW - B::GlyphStroke, -HS, HS }, // middle
		};
	}

	/** A convex polygon in a stand's paint frame, as the clip below leaves it. */
	using FPaintPoly = TArray<FVector2D, TInlineAllocator<12>>;

	/**
	 * Keep the part of Poly where Dot(N, P) <= C - one Sutherland-Hodgman pass. A convex
	 * polygon clipped by a half-plane stays convex, which is all the paint below needs: a
	 * stripe is the zone rectangle cut by two parallel half-planes and then by the boundary's.
	 */
	void ClipHalfPlane(FPaintPoly& Poly, const FVector2D& N, double C)
	{
		FPaintPoly Kept;
		const int32 Count = Poly.Num();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FVector2D& A = Poly[Index];
			const FVector2D& B = Poly[(Index + 1) % Count];
			const double DA = FVector2D::DotProduct(N, A) - C;
			const double DB = FVector2D::DotProduct(N, B) - C;
			if (DA <= 0.0)
			{
				Kept.Add(A);
			}
			if ((DA < 0.0 && DB > 0.0) || (DA > 0.0 && DB < 0.0))
			{
				Kept.Add(A + (B - A) * (DA / (DA - DB)));
			}
		}
		Poly = MoveTemp(Kept);
	}

	/**
	 * Drop repeated and collinear corners. A clip line through an existing corner leaves the
	 * corner twice, and a stripe edge along a zone edge leaves a corner on a straight run -
	 * either becomes a fan triangle of zero area that AppendTriangleUp drops, stranding a
	 * vertex no triangle uses, whose normal then reads as pointing down (QuadsFaceUp, measured).
	 */
	void DropDegenerateCorners(FPaintPoly& Poly)
	{
		bool bChanged = true;
		while (bChanged && Poly.Num() >= 3)
		{
			bChanged = false;
			for (int32 Index = 0; Index < Poly.Num(); ++Index)
			{
				const FVector2D& Prev = Poly[(Index + Poly.Num() - 1) % Poly.Num()];
				const FVector2D& Here = Poly[Index];
				const FVector2D& Next = Poly[(Index + 1) % Poly.Num()];
				// Twice the corner triangle's area, uu^2: under 0.01 is a hundredth of a
				// square uu - nothing that shows, everything that strands a vertex.
				if (FMath::Abs(FVector2D::CrossProduct(Here - Prev, Next - Here)) < 0.01)
				{
					Poly.RemoveAt(Index);
					bChanged = true;
					break;
				}
			}
		}
	}

	double TwiceSignedArea(TConstArrayView<FVector2D> Poly)
	{
		double Twice = 0.0;
		for (int32 Index = 0; Index < Poly.Num(); ++Index)
		{
			Twice += FVector2D::CrossProduct(Poly[Index], Poly[(Index + 1) % Poly.Num()]);
		}
		return Twice;
	}

	FPaintPoly LocalRect(double X0, double X1, double Y0, double Y1)
	{
		return { FVector2D(X0, Y0), FVector2D(X1, Y0), FVector2D(X1, Y1), FVector2D(X0, Y1) };
	}
}

const uint8 FStandMarkingBuilder::GlyphSegments[6] = {
	SegA | SegB | SegC | SegE | SegF | SegG,   // A
	SegC | SegD | SegE | SegF | SegG,          // b
	SegA | SegD | SegE | SegF,                 // C
	SegB | SegC | SegD | SegE | SegG,          // d
	SegA | SegD | SegE | SegF | SegG,          // E
	SegA | SegE | SegF | SegG,                 // F
};

bool FStandMarkingBuilder::FrameFor(const FEntityInstance& Entity, const FLetterEnvelopeTable& Envelopes,
	FStandPaintFrame& Out)
{
	// ONE LINE, BOTH NAMES (Check-Architecture's is-plotted-not-depot rule) - see
	// RoadEntity.h's own warning against reading IsPlotted() alone.
	if (!Entity.bAlive || !(Entity.IsStand() && Entity.IsPlotted()))
	{
		return false;
	}

	// THE LETTER PAINTED. Unset for a raw-model fixture stand whose DesignWingspan was
	// never captured - a legacy stand migrated to an outline at load (StandBox::BoxAt on
	// an outline-less stand) but never re-measured. No glyph is drawn for one; the lead-in
	// and stop bar still are (Build).
	TOptional<EIcaoCode> GlyphLetter;
	if (Entity.DesignWingspan > 0.0)
	{
		// CodeForWingspan, not Parse(LetterForWingspan(...)) - the primitive, shared with
		// AnchorLink's LeadInSizingFor and IcaoCode.cpp's own StandAdmits (#292).
		GlyphLetter = IcaoCode::CodeForWingspan(Entity.DesignWingspan);
	}

	// THE LETTER THE DEPTH CALC USES - ALWAYS SET, Code C standing for "unknown" exactly
	// as the migration's own BoxAt(pose, C) fallback does (see this class's header), so an
	// unknown stand's entrance is derived with the SAME figures its outline was built
	// with, not an arbitrary other letter's.
	const EIcaoCode DepthLetter = GlyphLetter.IsSet() ? *GlyphLetter : EIcaoCode::C;

	// Facing = (cos H, sin H), the project's idiom (RoadGeom::Bearing's own inverse) -
	// see StandPlotPlacementTest for the same derivation read the other way.
	const FVector2D Facing(FMath::Cos(Entity.Heading), FMath::Sin(Entity.Heading));
	// THE READER'S RIGHT IS PerpCCW(Facing), because Unreal is LEFT-HANDED: with X forward
	// and Z up, +Y is to the right, and (-Along.Y, Along.X) is +Y for Along = +X - the frame
	// RunwayMarkingBuilder's FRunwayFrame derives (its Across) and paints every designation
	// by. This read -PerpCCW until the final review (I3), borrowed from StandBox::BoxAt's
	// "Side" on the belief that the glyph had to agree with the box's own side - but the
	// box is symmetric about the heading, so its side's sign decides nothing there, while
	// here it decides which way every letter reads: the painted C opened to the left and
	// the "d" read as "b". That derivation assumed a right-handed map. The lead-in and stop
	// bar below are symmetric about the heading too, so only the glyph could show it.
	const FVector2D Right = RoadGeom::PerpCCW(Facing);

	// StandBox::PoseFor's own derivation, run in reverse - see this class's header for
	// why this reads the pose rather than Outline[0]/[1]. THE FLOOR, NOT Envelopes[C], for
	// the unknown-letter case - see Build's own header on why it must match the
	// migration's frozen figure rather than whatever the fleet has since raised Code C to.
	const FLetterEnvelope& DepthEnvelope = GlyphLetter.IsSet()
		? Envelopes[DepthLetter]
		: IcaoCode::FloorEnvelopeForLetter(DepthLetter);
	const double Distance = StandBox::EntranceSetback(DepthLetter, DepthEnvelope);
	const FVector2D EntranceMid = Entity.Position - Facing * Distance;

	Out.GlyphLetter = GlyphLetter;
	Out.Facing = Facing;
	Out.Right = Right;
	Out.Setback = Distance;
	Out.EntranceMid = EntranceMid;

	// THE RESTRAINT BOX - an EQUIPMENT RESTRAINT line in the real-world sense (controller
	// ruling R19, 2026-09-27): ground service vehicles wait OUTSIDE it until the aircraft has
	// stopped, then cross it to service. So a serve leg reaching a service point abeam the
	// fuselage crosses it by design; a park bay, lane, spur or contact never does.
	// ENFORCED BY: Airside.Present.StandMarking.LayoutClearsTheRestraintBox
	//
	// THE FIGURES THAT SIZE THE STAND, not new ones: across, the letter's widest span plus its
	// wingtip clearance - the first two terms of IcaoCode's WidthOf - and forward, the stop mark
	// plus the fleet's longest nose (the envelope PoseFor was given) plus the same clearance. For
	// the STAND letter (StandLetterFor), since an A airframe parks on B's ground. Aft, the box
	// ends at the stand's entrance, inside the boundary line and its own entrance side: the tail's
	// clearance is the setback's business, to the taxiway beyond.
	const EIcaoCode StandLetter = IcaoCode::StandLetterFor(DepthLetter);
	const double Clearance = IcaoCode::WingtipClearanceForLetter(StandLetter);
	Out.RestraintHalfWidth = 0.5 * IcaoCode::MaxWingspanForLetter(StandLetter) + Clearance;
	Out.RestraintForward = Distance + DepthEnvelope.MaxNoseFwd + Clearance;
	Out.RestraintAft = BoundaryWidth + RestraintWidth;

	// THE STAND'S GROUND, in the paint frame: the drawn outline, wound counter-clockwise (the
	// frame is a rotation, so winding survives the change), and the same outline inset by the
	// boundary line - the MITRED inset, so a corner of the inner edge sits exactly on both
	// edges' inset lines. Everything painted inside the boundary is clipped to the inner one,
	// which is what keeps the hatch "just inside the white line" on any outline the player
	// drew, not only the floor rectangle of its letter. HERE rather than in Build since R21,
	// because the box's forward clamp below is measured against it.
	Out.Ground.Reset();
	for (const FVector2D& Corner : Entity.Outline)
	{
		Out.Ground.Add(Out.ToLocal(Corner));
	}
	if (TwiceSignedArea(Out.Ground) < 0.0)
	{
		Algo::Reverse(Out.Ground);
	}
	const int32 Corners = Out.Ground.Num();
	Out.Inner.Reset();
	Out.XMin = DBL_MAX; Out.XMax = -DBL_MAX; Out.YMin = DBL_MAX; Out.YMax = -DBL_MAX;
	double InnerFar = -DBL_MAX;
	for (int32 Index = 0; Index < Corners; ++Index)
	{
		const FVector2D& Prev = Out.Ground[(Index + Corners - 1) % Corners];
		const FVector2D& Here = Out.Ground[Index];
		const FVector2D& Next = Out.Ground[(Index + 1) % Corners];
		// Inward = left of each edge, for a counter-clockwise polygon.
		const FVector2D InPrev = RoadGeom::PerpCCW((Here - Prev).GetSafeNormal());
		const FVector2D InNext = RoadGeom::PerpCCW((Next - Here).GetSafeNormal());
		const double Join = 1.0 + FVector2D::DotProduct(InPrev, InNext);
		Out.Inner.Add(Join > UE_KINDA_SMALL_NUMBER ? Here + (InPrev + InNext) * (BoundaryWidth / Join) : Here);
		InnerFar = FMath::Max(InnerFar, Out.Inner.Last().X);
		Out.XMin = FMath::Min(Out.XMin, Here.X); Out.XMax = FMath::Max(Out.XMax, Here.X);
		Out.YMin = FMath::Min(Out.YMin, Here.Y); Out.YMax = FMath::Max(Out.YMax, Here.Y);
	}

	// R21: CLAMPED TO THE FAR EDGE, never dropped. A fleet whose longest nose reaches past a
	// shallow stand (a raised Code C in GhostCommitAndPointPlacedAgree reached 7947 uu on a
	// 6500 uu stand; Code E's floor stand has 298 uu of slack) used to push the nose side off
	// the ground, where the clip removed it without a word - a three-sided box and no hatch.
	// The box's forward side now sits on the inner far edge instead, the strip ahead of it is
	// then empty (which is correct: there is no ground there), and Build warns, because a stand
	// too shallow for its fleet is a layout fact the player should hear about.
	// ENFORCED BY: Airside.Build.StandMarking.ShallowStandClampsTheNoseSide
	Out.EnvelopeForward = Out.RestraintForward;
	Out.bForwardClamped = Corners >= 3 && Out.RestraintForward + RestraintWidth > InnerFar;
	if (Out.bForwardClamped)
	{
		Out.RestraintForward = FMath::Max(InnerFar - RestraintWidth, Out.RestraintAft);
	}
	return true;
}

int32 FStandMarkingBuilder::Build(const URoadNetwork& Network, double Z, FRoadMeshBuffers& Out,
	const FLetterEnvelopeTable& Envelopes, FStandMarkingCensus* Census, const FStandPaintIds& PaintIds)
{
	FStandMarkingCensus Local;
	FStandMarkingCensus& C = Census != nullptr ? *Census : Local;
	C = FStandMarkingCensus();

	int32 Painted = 0;
	for (int32 EntityIndex = 0; EntityIndex < Network.GetEntities().Num(); ++EntityIndex)
	{
		const FEntityInstance& Entity = Network.GetEntities()[EntityIndex];

		// FrameFor's false IS the alive/stand/plotted test (its own ONE LINE, BOTH NAMES check),
		// honoured rather than repeated here - a second copy is a second place to drift.
		FStandPaintFrame Frame;
		if (!FrameFor(Entity, Envelopes, Frame))
		{
			continue;
		}
		const TOptional<EIcaoCode>& GlyphLetter = Frame.GlyphLetter;
		const FVector2D& Facing = Frame.Facing;
		const FVector2D& Right = Frame.Right;
		const FVector2D& EntranceMid = Frame.EntranceMid;
		const double Distance = Frame.Setback;

		// Clip a zone to the ground inside the boundary line and emit it, in the frame. True when
		// something was left to paint.
		auto Emit = [&](FPaintPoly Poly, EStandPaint Paint) -> bool
		{
			for (int32 Index = 0; Index < Frame.Inner.Num() && Poly.Num() >= 3; ++Index)
			{
				const FVector2D& A = Frame.Inner[Index];
				const FVector2D Edge = Frame.Inner[(Index + 1) % Frame.Inner.Num()] - A;
				// Keep the left of A->B: Dot((Edge.Y, -Edge.X), P - A) <= 0.
				const FVector2D N(Edge.Y, -Edge.X);
				ClipHalfPlane(Poly, N, FVector2D::DotProduct(N, A));
			}
			DropDegenerateCorners(Poly);
			if (Poly.Num() < 3 || FMath::Abs(TwiceSignedArea(Poly)) < 2.0)
			{
				return false;
			}
			TArray<FVector2D, TInlineAllocator<12>> World;
			for (const FVector2D& P : Poly)
			{
				World.Add(Frame.ToWorld(P.X, P.Y));
			}
			MarkingQuads::AddConvexPolygon(Out, Z, World, PaintIds[Paint]);
			return true;
		};

		// LEAD-IN: from just inside the entrance to the stop mark, along the heading, LeadInWidth
		// wide. It STARTS AT RestraintAft, past the boundary line and the restraint box's entrance
		// side, since task 13, not at the entrance edge itself: every stand paint lies at the same
		// MarkingZ, so a yellow quad over the white and red ones would z-fight where they cross.
		// Its far end - the one a pilot reads - is where it always was.
		// CLIPPED TO THE GROUND like every other stand paint (review of task 13): on an outline
		// that does not sit where the pose implies, the entrance end could otherwise start off the
		// stand.
		C.LeadIns += Emit(LocalRect(Frame.RestraintAft, Distance, -LeadInWidth * 0.5, LeadInWidth * 0.5),
			EStandPaint::Guidance) ? 1 : 0;

		// STOP BAR: across the heading, centred ON the stop mark (Entity.Position).
		MarkingQuads::AddRect(Out, Z, Entity.Position, Facing, Right,
			-StopBarWidth * 0.5, StopBarWidth * 0.5, -StopBarLength * 0.5, StopBarLength * 0.5,
			PaintIds[EStandPaint::Guidance]);
		++C.StopBars;

		// LETTER: seven-segment strokes, centred GlyphInset inside the entrance. "Up" is
		// Facing, so a pilot taxiing IN - moving along Facing, from the entrance toward the
		// stop mark - reads it upright, the same sense StandBox::FStandPose::Facing is
		// documented in (away from the taxiway the stand opens off).
		if (GlyphLetter.IsSet())
		{
			const FVector2D GlyphCenter = EntranceMid + Facing * GlyphInset;
			const uint8 Bits = GlyphSegments[static_cast<uint8>(*GlyphLetter)];
			for (const FGlyphSegmentSpec& Segment : GlyphSegmentRects())
			{
				if ((Bits & Segment.Bit) == 0)
				{
					continue;
				}
				MarkingQuads::AddRect(Out, Z, GlyphCenter, Right, Facing,
					Segment.XMin, Segment.XMax, Segment.YMin, Segment.YMax, PaintIds[EStandPaint::Guidance]);
				++C.LetterSegments;
			}
		}

		// BOUNDARY: a white line just inside the drawn outline, every edge - outer edge to its
		// mitred inner one, so the corners close without overlap.
		const TArray<FVector2D>& Ground = Frame.Ground;
		const TArray<FVector2D>& Inner = Frame.Inner;
		for (int32 Index = 0; Index < Ground.Num(); ++Index)
		{
			const int32 NextIndex = (Index + 1) % Ground.Num();
			MarkingQuads::AddQuad(Out, Z,
				Frame.ToWorld(Ground[Index].X, Ground[Index].Y), Frame.ToWorld(Ground[NextIndex].X, Ground[NextIndex].Y),
				Frame.ToWorld(Inner[NextIndex].X, Inner[NextIndex].Y), Frame.ToWorld(Inner[Index].X, Inner[Index].Y),
				PaintIds[EStandPaint::Boundary]);
			++C.BoundaryEdges;
		}

		// RESTRAINT: the red line round the aircraft's box, just OUTSIDE its interior (see
		// FrameFor for what the box is and why vehicles may cross it). Four sides, closed - the
		// entrance side against the boundary line, which the lead-in now starts beyond - and
		// butted, not overlapped, at the corners: the entrance and nose sides run the full
		// width, the two long sides between them.
		// ONE LINE PER STAND PER REBUILD (R21) - see FrameFor's clamp. A Warning because nothing
		// else says a stand is too shallow for the aircraft its letter admits.
		if (Frame.bForwardClamped)
		{
			UE_LOG(LogRoadMesh, Warning,
				TEXT("Stand %d (Code %s): aircraft envelope reaches %.0f uu, stand is %.0f uu deep - nose restraint ")
				TEXT("clamped to the far edge; the stand is too shallow for the fleet"),
				EntityIndex, GlyphLetter.IsSet() ? IcaoCode::ToLetter(*GlyphLetter) : TEXT("unknown"),
				Frame.EnvelopeForward + RestraintWidth, Frame.XMax - Frame.XMin);
		}

		const double H = Frame.RestraintHalfWidth;
		const double Fwd = Frame.RestraintForward;
		const double Aft = Frame.RestraintAft;
		const double Outer = H + RestraintWidth;
		C.RestraintSides += Emit(LocalRect(BoundaryWidth, Aft, -Outer, Outer), EStandPaint::Restraint) ? 1 : 0;
		C.RestraintSides += Emit(LocalRect(Fwd, Fwd + RestraintWidth, -Outer, Outer), EStandPaint::Restraint) ? 1 : 0;
		C.RestraintSides += Emit(LocalRect(Aft, Fwd, H, Outer), EStandPaint::Restraint) ? 1 : 0;
		C.RestraintSides += Emit(LocalRect(Aft, Fwd, -Outer, -H), EStandPaint::Restraint) ? 1 : 0;

		// HATCH: ALL the vehicle-only ground (controller ruling R20) - each side from the red
		// line out to the white one, the length of the box, and the whole strip ahead of it to the
		// far edge. DERIVED FROM THE OUTLINE AND THE BOX, never from a lane width: on a B stand
		// the tow lane sits 21-25 m out, not the 15-19 m that "restraint + ServiceLaneWidth" gave
		// (task 13's probe), because B's width is its tow lane's, not its span's.
		// ENFORCED BY: Airside.Present.StandMarking.LayoutLiesOnHatch
		//
		// Stripes at 45 degrees, HatchStripeWidth wide, alternating HatchMark and HatchSpace by
		// the stripe's index along x + y - ONE index across all three zones, so a stripe runs on
		// unbroken where a side zone meets the strip ahead.
		const double Stride = HatchStripeWidth * UE_DOUBLE_SQRT_2;
		const FPaintPoly Zones[3] = {
			LocalRect(Frame.XMin, Fwd + RestraintWidth, Outer, Frame.YMax),
			LocalRect(Frame.XMin, Fwd + RestraintWidth, Frame.YMin, -Outer),
			LocalRect(Fwd + RestraintWidth, Frame.XMax, Frame.YMin, Frame.YMax),
		};
		for (const FPaintPoly& Zone : Zones)
		{
			const double UMin = Zone[0].X + Zone[0].Y;
			const double UMax = Zone[2].X + Zone[2].Y;
			if (UMax <= UMin)
			{
				continue;
			}
			for (int64 Stripe = FMath::FloorToInt64(UMin / Stride); Stripe * Stride < UMax; ++Stripe)
			{
				FPaintPoly Piece = Zone;
				ClipHalfPlane(Piece, FVector2D(-1.0, -1.0), -Stripe * Stride);
				ClipHalfPlane(Piece, FVector2D(1.0, 1.0), (Stripe + 1) * Stride);
				const EStandPaint Paint = (Stripe % 2 == 0) ? EStandPaint::HatchMark : EStandPaint::HatchSpace;
				C.HatchStripes += (Piece.Num() >= 3 && Emit(MoveTemp(Piece), Paint)) ? 1 : 0;
			}
		}

		// A LOG LINE IS A FEATURE (CLAUDE.md, "Diagnosing") - one per stand, once per surface
		// rebuild, so a report of missing or misplaced stand paint is one grep away. The box's
		// figures appended since task 13, so "the red line is in the wrong place" is one grep too.
		UE_LOG(LogAirside, Log,
			TEXT("StandPaint: stand at (%.0f, %.0f), letter %s, entrance (%.0f, %.0f), restraint +-%.0f to %.0f"),
			Entity.Position.X, Entity.Position.Y,
			GlyphLetter.IsSet() ? IcaoCode::ToLetter(*GlyphLetter) : TEXT("unknown"),
			EntranceMid.X, EntranceMid.Y, H, Fwd);

		++Painted;
	}
	return Painted;
}
