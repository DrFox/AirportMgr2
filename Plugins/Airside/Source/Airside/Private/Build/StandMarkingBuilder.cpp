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

	Out.GlyphLetter = GlyphLetter;
	Out.Facing = Facing;
	Out.Right = Right;

	// THE ENTRANCE IS READ OFF THE OUTLINE, not derived from the pose, since 2026-09-27: the
	// pose is now measured from the FAR edge (StandBox::PoseFor), so on a stand drawn deeper
	// than its floor the entrance is further behind the stop mark than EntranceSetback, and only
	// the outline knows by how much. THE OUTLINE'S ENTRANCE EDGE IS READ, NOT SEARCHED FOR (#450's leftover): this
	// used to take the rearmost CORNER along Facing on every rebuild, measured rather than read from a corner index
	// for PoseFor's own reason - the facade reverses a clockwise outline - and now reads the edge placement stored
	// (FEntityInstance::FrontageEdge; the pose reader reads the same one, where it used to search by midpoint). Its
	// reach is the rearmost of the edge's two ends: for the stand the game makes - a rectangle whose entrance faces
	// its stop mark - both ends are level along Facing and it is the rearmost corner as before, which the pin measures
	// on every fixture; for a stand whose rearmost corner is NOT on its entrance, it is the entrance that is painted from.
	// ENFORCED BY: Airside.Model.StandFrontage.StoredEdgeIsTodaysHeuristicAnswer, Airside.Model.StandFrontage.ReadersReadTheStoredEdge
	// Centred across on the pose, which PoseFor centres on the entrance.
	//
	// A STAND WITH NO STORED ENTRANCE (an outline the migration has not reached) falls back to the floor figure its migrated
	// box was built with - Code C's floor for an unknown letter, the figure a stand nobody drew has always been painted
	// from - rather than losing its paint, and says so once: EnsureStandFrontages runs before any rebuild in both load paths.
	double Behind = -StandBox::EntranceSetback(DepthLetter, GlyphLetter.IsSet()
		? Envelopes[DepthLetter] : IcaoCode::FloorEnvelopeForLetter(DepthLetter));
	FVector2D EntranceA, EntranceB;
	if (Entity.GetFrontage(EntranceA, EntranceB))
	{
		Behind = FMath::Min(FVector2D::DotProduct(EntranceA - Entity.Position, Facing),
			FVector2D::DotProduct(EntranceB - Entity.Position, Facing));
	}
	else
	{
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogAirside, Warning,
				TEXT("FStandMarkingBuilder: a drawn stand has no stored entrance edge (EnsureStandFrontages did not run?) - painted at the floor setback."));
		}
	}
	Out.Setback = -Behind;
	Out.EntranceMid = Entity.Position + Facing * Behind;

	// THE STAND'S GROUND, in the paint frame: the drawn outline, wound counter-clockwise (the
	// frame is a rotation, so winding survives the change), and the same outline inset by the
	// boundary line - the MITRED inset, so a corner of the inner edge sits exactly on both
	// edges' inset lines. The lead-in is clipped to the inner one, so no paint starts off the
	// stand or under the white line.
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
		Out.XMin = FMath::Min(Out.XMin, Here.X); Out.XMax = FMath::Max(Out.XMax, Here.X);
		Out.YMin = FMath::Min(Out.YMin, Here.Y); Out.YMax = FMath::Max(Out.YMax, Here.Y);
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
	for (const FEntityInstance& Entity : Network.GetEntities())
	{

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

		// LEAD-IN: the taxi line up the middle, from just inside the entrance to the stop mark,
		// LeadInWidth wide. It STARTS INSIDE THE WHITE LINE, not at the entrance edge itself: every
		// stand paint lies at the same MarkingZ, so a yellow quad over the white one would z-fight
		// where they cross. CLIPPED TO THE GROUND, so on an outline that does not sit where the
		// pose implies the entrance end cannot start off the stand.
		C.LeadIns += Emit(LocalRect(BoundaryWidth, Distance, -LeadInWidth * 0.5, LeadInWidth * 0.5),
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

		// A LOG LINE IS A FEATURE (CLAUDE.md, "Diagnosing") - one per stand, once per surface
		// rebuild, so a report of missing or misplaced stand paint is one grep away. The setback
		// since 2026-09-27, when the pose moved to the far edge: "the aircraft stops in the wrong
		// place" is then one grep too - it is the entrance-to-stop-mark distance actually painted.
		UE_LOG(LogAirside, Log,
			TEXT("StandPaint: stand at (%.0f, %.0f), letter %s, entrance (%.0f, %.0f), setback %.0f of %.0f deep"),
			Entity.Position.X, Entity.Position.Y,
			GlyphLetter.IsSet() ? IcaoCode::ToLetter(*GlyphLetter) : TEXT("unknown"),
			EntranceMid.X, EntranceMid.Y, Distance, Frame.XMax - Frame.XMin);

		++Painted;
	}
	return Painted;
}
