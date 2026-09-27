#pragma once

#include "CoreMinimal.h"
#include "Build/RoadMeshSink.h"
#include "Solve/IcaoCode.h"
#include "Solve/LetterEnvelope.h"

class URoadNetwork;
struct FEntityInstance;

/**
 * What a piece of stand paint MEANS - never what colour it is. Build/ describes intent, the
 * same discipline Tool/ keeps with IToolPreviewSink; URoadSurfacePresenter::StandPaintSlot is
 * the ONE place a meaning becomes a material slot (and so a colour).
 */
enum class EStandPaint : uint8
{
	/** Where the aircraft goes: lead-in, stop bar, the stand letter. */
	Guidance,
	/** The edge of the stand's ground. */
	Boundary,
	/** The equipment restraint line round the parked aircraft's envelope. */
	Restraint,
	/** The marked stripes of vehicle-only ground's hatch. */
	HatchMark,
	/** The stripes between them. */
	HatchSpace,
	Count
};

/**
 * The material id each EStandPaint's triangles carry, filled by the caller from its material
 * set. All 0 by default, so a caller with no set - every Build/ test before paint had meanings -
 * paints everything on slot 0 exactly as before.
 */
struct FStandPaintIds
{
	int32 Ids[static_cast<int32>(EStandPaint::Count)] = {};

	int32 operator[](EStandPaint Paint) const { return Ids[static_cast<int32>(Paint)]; }
};

/** How many of each stand marking one Build painted, for the log line and the tests. */
struct AIRSIDE_API FStandMarkingCensus
{
	int32 LeadIns = 0;
	int32 StopBars = 0;
	int32 LetterSegments = 0;
	/** One per edge of the drawn outline. */
	int32 BoundaryEdges = 0;
	/**
	 * Four per stand: entrance, both sides, nose - for every stand whose outline is the one its
	 * pose was derived from, which is every placement path (drawn, point-placed, migrated). True
	 * again since R21 (review of task 13): the nose side used to fall off the far edge, silently,
	 * whenever the fleet's envelope reached past a shallow stand; FrameFor now clamps the box's
	 * forward edge to the ground inside the boundary, so the nose side lands on the stand (and
	 * Build warns). A HAND-SET outline that disagrees with the pose - 2026-09-27, only the test
	 * fixtures StandOutlineIsNotADepot (a 15 x 8 m outline on a C pose, 1 side) and
	 * RoadLinesUpWithAPlotEdge / RoadRunsParallelToAPlotEdge (3 sides) - can still clip sides
	 * away; every vertex stays on the ground either way.
	 * ENFORCED BY: Airside.Build.StandMarking.ShallowStandClampsTheNoseSide, Airside.Build.StandMarking.PaintStaysInsideOutline
	 */
	int32 RestraintSides = 0;
	/** Hatch stripe pieces actually emitted, both kinds, after clipping to their zones. */
	int32 HatchStripes = 0;
};

/**
 * A stand's PAINT FRAME: local x from the entrance edge along Facing (toward the nose and the
 * far edge), local y along Right = PerpCCW(Facing), the reader's right - see Build() for why that
 * sign. And the RESTRAINT BOX in it: the ground the parked aircraft owns, whose interior is
 * RestraintAft < x < RestraintForward, |y| < RestraintHalfWidth.
 *
 * PUBLIC so the tests measure paint and the real vehicle layout against the SAME box the builder
 * paints, rather than a second derivation of it that could agree with itself and not the paint.
 */
struct AIRSIDE_API FStandPaintFrame
{
	FVector2D EntranceMid = FVector2D::ZeroVector;
	FVector2D Facing = FVector2D(1.0, 0.0);
	FVector2D Right = FVector2D(0.0, 1.0);

	/** The letter painted - unset for a stand whose DesignWingspan was never captured. */
	TOptional<EIcaoCode> GlyphLetter;

	/** Entrance edge to the stop mark, uu - StandBox::EntranceSetback. */
	double Setback = 0.0;

	double RestraintHalfWidth = 0.0;
	double RestraintForward = 0.0;
	double RestraintAft = 0.0;

	/** RestraintForward before R21's clamp: where the fleet's envelope says the box should end. */
	double EnvelopeForward = 0.0;
	/** The stand is too shallow for its letter's fleet, and RestraintForward was pulled in to fit. */
	bool bForwardClamped = false;

	/**
	 * The drawn outline in this frame, wound counter-clockwise, and the same outline inset by the
	 * boundary line (mitred) - the ground every non-boundary paint is clipped to.
	 */
	TArray<FVector2D> Ground;
	TArray<FVector2D> Inner;
	/** Ground's extents in this frame. */
	double XMin = 0.0, XMax = 0.0, YMin = 0.0, YMax = 0.0;

	FVector2D ToWorld(double X, double Y) const { return EntranceMid + Facing * X + Right * Y; }
	FVector2D ToLocal(const FVector2D& World) const
	{
		const FVector2D D = World - EntranceMid;
		return FVector2D(FVector2D::DotProduct(D, Facing), FVector2D::DotProduct(D, Right));
	}
	/** Strictly inside the restraint box, by more than Tolerance. */
	bool InRestraintInterior(const FVector2D& World, double Tolerance) const
	{
		const FVector2D P = ToLocal(World);
		return P.X > RestraintAft + Tolerance && P.X < RestraintForward - Tolerance
			&& FMath::Abs(P.Y) < RestraintHalfWidth - Tolerance;
	}
};

/**
 * The PAINT of a drawn stand: a lead-in line from the entrance to the stop mark, a stop bar
 * across the heading at the stop mark, and the stand's code letter as seven-segment strokes -
 * and since task 13 (2026-09-27, "white lines for the outer edge, paint in the no go zones for
 * the aircraft and the stop lines") a white boundary just inside the outline, a red restraint
 * line round the aircraft's box (FStandPaintFrame), and a red-and-white 45 degree hatch over all
 * the vehicle-only ground outside it. Each quad names its EStandPaint; the caller picks colours -
 * quads through the same builder idiom as FHoldingPositionMarkingBuilder and
 * FRunwayMarkingBuilder (MarkingQuads::AddQuad/AddRect, UV1 = 0 solid).
 *
 * SEVEN-SEGMENT PAINT, NOT A UTextRenderComponent - deviates from the spec's original Paint
 * section (2026-09-23-drawn-stands-design.md, revised this task). A-F are exactly the
 * letters a calculator's seven-segment display draws (A b C d E F), so no font is needed;
 * paint needs no component lifecycle - a UTextRenderComponent is a transient subobject, and
 * this project has already been bitten once by a transient subobject pointer resetting to
 * the CDO on level duplication (see the memory note) - is headless-testable the same way
 * every other marking is, through the buffers rather than a spawned actor, and lies flat by
 * construction because every vertex it emits carries the Z it is given, the same as any
 * other quad MarkingQuads::AddQuad produces.
 *
 * NOT MarkingGlyphs (Build/MarkingGlyphs.h), the runway designation font, although it is the
 * obvious table to share: it has L, C, R and the digits, and of A b C d E F only C - five of
 * the six letters would have to be authored into it anyway, as polylines on its 1 x 1.6 cell
 * rather than the seven rectangles here that ARE the letter set. The reading FRAME is shared
 * with it instead (reader's right = PerpCCW(up), RunwayMarkingBuilder's FRunwayFrame), which
 * is the part that was wrong - see Build() - and the part a second table cannot drift on.
 *
 * A Build/ class beside the other marking builders, for the same reason none of them are
 * part of FRoadMeshBuilder: a marking that lands on a road (or stand pad) vertex must not
 * weld to it. The two surfaces meet; they are not one surface.
 */
struct AIRSIDE_API FStandMarkingBuilder
{
	/** Stand boundary line width, uu (20 cm), painted just inside the drawn outline. */
	static constexpr double BoundaryWidth = 20.0;
	/** Restraint line width, uu (20 cm). */
	static constexpr double RestraintWidth = 20.0;
	/** Hatch stripe width, uu (50 cm, measured square to the stripe), at 45 degrees. */
	static constexpr double HatchStripeWidth = 50.0;

	/** Lead-in line width, uu (15 cm). */
	static constexpr double LeadInWidth = 15.0;
	/** Stop bar: along the heading (40 cm) by across it (6 m), uu. 6 m since task 13, up from 3:
	 *  with the overlay's stop and pose rings gone the bar IS the stop mark, and the user asked
	 *  for one bar, bigger, rather than a bar per aircraft type. */
	static constexpr double StopBarWidth = 40.0;
	static constexpr double StopBarLength = 600.0;

	/** Letter glyph: 3 m tall, 30 cm stroke, uu. Width is this builder's own choice - the
	 *  brief specifies height and stroke only - picked for a digit-like 2:3 aspect. */
	static constexpr double GlyphHeight = 300.0;
	static constexpr double GlyphStroke = 30.0;
	static constexpr double GlyphWidth = 200.0;
	/** How far inside the entrance the glyph's centre sits, uu (4 m). */
	static constexpr double GlyphInset = 400.0;

	/**
	 * The seven segments (bits a=0x01 .. g=0x40, the calculator-display convention) each
	 * letter A-F draws, indexed by EIcaoCode (A=0 .. F=5). 'B' and 'D' paint as the
	 * lower-case seven-segment shapes ('b', 'd') - the only shapes a seven-segment display
	 * can draw for those two letters without doubling as another character (upper-case B is
	 * indistinguishable from 8, upper-case D from 0).
	 */
	static const uint8 GlyphSegments[6];

	/**
	 * Entity's paint frame and restraint box, or false when Entity is not a live drawn stand.
	 * The letter, envelope and setback are Build()'s own derivation - see Build's header - and
	 * the box's figures are the SAME ones that size the stand (IcaoCode: MaxWingspan,
	 * WingtipClearance; the envelope's MaxNoseFwd), for the stand's own letter (StandLetterFor).
	 */
	static bool FrameFor(const FEntityInstance& Entity, const FLetterEnvelopeTable& Envelopes, FStandPaintFrame& Out);

	/**
	 * Append the markings of every drawn (plotted) stand in Network to Out, in the road
	 * plane at Z. Returns how many stands were painted; Census, when given, says what was
	 * painted.
	 *
	 * For each alive entity with IsStand() && IsPlotted(): the entrance midpoint is derived
	 * from the pose, Position - Facing * StandBox::EntranceSetback(L, Envelope(L)) -
	 * StandBox::PoseFor's own derivation run in reverse - rather than read from Outline[0..1],
	 * because URoadEditFacade::PlaceStandInPlot reverses a clockwise outline (and swaps which
	 * of its two ORIGINAL corners is "entrance A/B" to match), which moves the entrance edge
	 * off indices 0->1 of the STORED array; the pose needs no winding assumption at all. L is
	 * the letter LetterForWingspan(DesignWingspan) reads back, or Code C - the same fallback
	 * a migrated legacy stand's outline was built with (see StandBox::BoxAt at load) - when
	 * DesignWingspan is 0 (a raw-model fixture stand nobody measured). That letter is also
	 * what is painted; when it is unset (the DesignWingspan-0 case), no glyph is painted, but
	 * the lead-in and stop bar still are - a stand with no known letter is still a stand an
	 * arrival can be routed to.
	 *
	 * Envelopes, since #292: Build/ may not include Content/AirsideSettings (Check-Architecture's
	 * include-direction rule), so the caller (Present/RoadSurfacePresenter.cpp) resolves
	 * UAirsideSettings::ResolveLetterEnvelopeTable() ONCE per rebuild and hands it down, rather
	 * than this loop reaching Content/ once per stand painted. A DesignWingspan-0 stand's
	 * entrance is derived from IcaoCode::FloorEnvelopeForLetter(EIcaoCode::C) instead of
	 * Envelopes[C] - it must match the SAME figure Model/RoadNetwork.cpp's migration built that
	 * stand's outline from (the floor, since Model/ cannot reach Content/ either), not whatever
	 * the fleet has since raised Code C to.
	 */
	static int32 Build(const URoadNetwork& Network, double Z, FRoadMeshBuffers& Out,
		const FLetterEnvelopeTable& Envelopes, FStandMarkingCensus* Census = nullptr,
		const FStandPaintIds& PaintIds = FStandPaintIds());
};
