#include "Build/StandMarkingBuilder.h"

#include "AirsideLog.h"

#include "Build/MarkingQuads.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Solve/RoadGeom.h"

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
}

const uint8 FStandMarkingBuilder::GlyphSegments[6] = {
	SegA | SegB | SegC | SegE | SegF | SegG,   // A
	SegC | SegD | SegE | SegF | SegG,          // b
	SegA | SegD | SegE | SegF,                 // C
	SegB | SegC | SegD | SegE | SegG,          // d
	SegA | SegD | SegE | SegF | SegG,          // E
	SegA | SegE | SegF | SegG,                 // F
};

int32 FStandMarkingBuilder::Build(const URoadNetwork& Network, double Z, FRoadMeshBuffers& Out, FStandMarkingCensus* Census)
{
	FStandMarkingCensus Local;
	FStandMarkingCensus& C = Census != nullptr ? *Census : Local;
	C = FStandMarkingCensus();

	int32 Painted = 0;
	for (const FEntityInstance& Entity : Network.GetEntities())
	{
		// ONE LINE, BOTH NAMES (Check-Architecture's is-plotted-not-depot rule) - see
		// RoadEntity.h's own warning against reading IsPlotted() alone.
		if (!Entity.bAlive || !(Entity.IsStand() && Entity.IsPlotted()))
		{
			continue;
		}

		// THE LETTER PAINTED. Unset for a raw-model fixture stand whose DesignWingspan was
		// never captured - a legacy stand migrated to an outline at load (StandBox::BoxAt on
		// an outline-less stand) but never re-measured. No glyph is drawn for one; the lead-in
		// and stop bar still are below.
		TOptional<EIcaoCode> GlyphLetter;
		if (Entity.DesignWingspan > 0.0)
		{
			GlyphLetter = IcaoCode::Parse(IcaoCode::LetterForWingspan(Entity.DesignWingspan));
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
		// why this reads the pose rather than Outline[0]/[1].
		const double Distance = IcaoCode::StandDepthForLetter(DepthLetter) - IcaoCode::MaxNoseFwdForLetter(DepthLetter);
		const FVector2D EntranceMid = Entity.Position - Facing * Distance;

		// LEAD-IN: entrance midpoint to the stop mark, along the heading, LeadInWidth wide.
		MarkingQuads::AddRect(Out, Z, EntranceMid, Facing, Right,
			0.0, Distance, -LeadInWidth * 0.5, LeadInWidth * 0.5);
		++C.LeadIns;

		// STOP BAR: across the heading, centred ON the stop mark (Entity.Position).
		MarkingQuads::AddRect(Out, Z, Entity.Position, Facing, Right,
			-StopBarWidth * 0.5, StopBarWidth * 0.5, -StopBarLength * 0.5, StopBarLength * 0.5);
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
					Segment.XMin, Segment.XMax, Segment.YMin, Segment.YMax);
				++C.LetterSegments;
			}
		}

		// A LOG LINE IS A FEATURE (CLAUDE.md, "Diagnosing") - one per stand, once per surface
		// rebuild, so a report of missing or misplaced stand paint is one grep away.
		UE_LOG(LogAirside, Log,
			TEXT("StandPaint: stand at (%.0f, %.0f), letter %s, entrance (%.0f, %.0f)"),
			Entity.Position.X, Entity.Position.Y,
			GlyphLetter.IsSet() ? IcaoCode::ToLetter(*GlyphLetter) : TEXT("unknown"),
			EntranceMid.X, EntranceMid.Y);

		++Painted;
	}
	return Painted;
}
