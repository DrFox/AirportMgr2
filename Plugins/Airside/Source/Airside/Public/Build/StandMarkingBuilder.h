#pragma once

#include "CoreMinimal.h"
#include "Build/RoadMeshSink.h"
#include "Solve/IcaoCode.h"

class URoadNetwork;

/** How many of each stand marking one Build painted, for the log line and the tests. */
struct AIRSIDE_API FStandMarkingCensus
{
	int32 LeadIns = 0;
	int32 StopBars = 0;
	int32 LetterSegments = 0;
};

/**
 * The PAINT of a drawn stand: a lead-in line from the entrance to the stop mark, a stop bar
 * across the heading at the stop mark, and the stand's code letter as seven-segment strokes -
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
	/** Lead-in line width, uu (15 cm). */
	static constexpr double LeadInWidth = 15.0;
	/** Stop bar: across the heading (40 cm) by along it (3 m), uu. */
	static constexpr double StopBarWidth = 40.0;
	static constexpr double StopBarLength = 300.0;

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
	 * Append the markings of every drawn (plotted) stand in Network to Out, in the road
	 * plane at Z. Returns how many stands were painted; Census, when given, says what was
	 * painted.
	 *
	 * For each alive entity with IsStand() && IsPlotted(): the entrance midpoint is derived
	 * from the pose, Position - Facing * (StandDepthForLetter(L) - MaxNoseFwdForLetter(L)) -
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
	 */
	static int32 Build(const URoadNetwork& Network, double Z, FRoadMeshBuffers& Out,
		FStandMarkingCensus* Census = nullptr);
};
