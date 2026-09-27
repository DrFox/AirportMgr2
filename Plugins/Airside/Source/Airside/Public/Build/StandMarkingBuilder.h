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
	// Restraint and the two hatch meanings were removed 2026-09-27: the user judged the red
	// line and the red/white hatch over the vehicle ground "awful" in PIE and asked for the
	// white edge and the taxi line with its stop line only.
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
};

/**
 * A stand's PAINT FRAME: local x from the entrance edge along Facing (toward the nose and the
 * far edge), local y along Right = PerpCCW(Facing), the reader's right - see Build() for why that
 * sign.
 *
 * PUBLIC so the tests measure paint against the SAME frame the builder paints in, rather than a
 * second derivation of it that could agree with itself and not the paint.
 */
struct AIRSIDE_API FStandPaintFrame
{
	FVector2D EntranceMid = FVector2D::ZeroVector;
	FVector2D Facing = FVector2D(1.0, 0.0);
	FVector2D Right = FVector2D(0.0, 1.0);

	/** The letter painted - unset for a stand whose DesignWingspan was never captured. */
	TOptional<EIcaoCode> GlyphLetter;

	/** Entrance edge to the stop mark, uu, read off the outline - StandBox::EntranceSetback only on
	 *  a floor-sized stand, since the pose is measured from the far edge (2026-09-27). */
	double Setback = 0.0;

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
};

/**
 * The PAINT of a drawn stand: a lead-in line from the entrance to the stop mark, a stop bar
 * across the heading at the stop mark, and the stand's code letter as seven-segment strokes -
 * and a white boundary just inside the outline (2026-09-27, "just go with a white line around
 * the edge and the taxi line up the middle with stopping lines on it"; a red restraint line and
 * a red/white hatch tried the same day were removed). Each quad names its EStandPaint; the
 * caller picks colours -
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
	/** Stand boundary line width, uu (40 cm), painted just inside the drawn outline. 40, not the
	 *  20 first tried: 20 cm did not read from the build camera (PIE, 2026-09-27). */
	static constexpr double BoundaryWidth = 40.0;

	/** Lead-in (taxi line) width, uu (30 cm) - wider than a real stand's 15 cm for the build
	 *  camera, BoundaryWidth's reason. */
	static constexpr double LeadInWidth = 30.0;
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
	 * Entity's paint frame, or false when Entity is not a live drawn stand. The letter and
	 * setback are Build()'s own derivation - see Build's header.
	 */
	static bool FrameFor(const FEntityInstance& Entity, const FLetterEnvelopeTable& Envelopes, FStandPaintFrame& Out);

	/**
	 * Append the markings of every drawn (plotted) stand in Network to Out, in the road
	 * plane at Z. Returns how many stands were painted; Census, when given, says what was
	 * painted.
	 *
	 * For each alive entity with IsStand() && IsPlotted(): the entrance midpoint is the
	 * outline's rearmost reach along the pose's Facing, centred on the pose - measured, not
	 * read from Outline[0..1], because URoadEditFacade::PlaceStandInPlot reverses a clockwise
	 * outline (and swaps which of its two ORIGINAL corners is "entrance A/B" to match), which
	 * moves the entrance edge off indices 0->1 of the STORED array. NOT derived from the pose
	 * since 2026-09-27: StandBox::PoseFor now measures the pose from the far edge, so the
	 * entrance-to-stop distance varies with the depth drawn. L is
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
