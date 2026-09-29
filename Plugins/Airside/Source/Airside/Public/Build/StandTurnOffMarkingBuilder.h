#pragma once

#include "CoreMinimal.h"
#include "Build/RoadMeshSink.h"
#include "Build/StandMarkingBuilder.h"

class URoadNetwork;

/** One face of a turn-off's number sign: where its text sits and which way it reads. */
struct FStandSignFace
{
	/** Centre of the text's foot line, world - MarkingText::AddString's Origin. */
	FVector2D Origin = FVector2D::ZeroVector;
	/** From the glyphs' feet to their tops - toward the arrow. */
	FVector2D Up = FVector2D::ZeroVector;
	/** The reading direction - along the lead-in axis, opposite on the other face. */
	FVector2D Right = FVector2D::ZeroVector;
};

/** What one FStandTurnOffMarkingBuilder::Build painted, for the log line and the tests. */
struct AIRSIDE_API FStandTurnOffCensus
{
	/** Lead-ins that met a taxiway - one per stand, two for a taxi-through stand. */
	int32 TurnOffs = 0;
	int32 Numbers = 0;
	/** The stand number painted at each turn-off, in paint order. */
	TArray<int32> NumbersPainted;
	/** Where each turn-off's sign is centred on its axis, world - beside NumbersPainted, same order. */
	TArray<FVector2D> NumberOrigins;
	/** Both faces of every sign, in paint order - two per signed turn-off. */
	TArray<FStandSignFace> SignFaces;
	int32 Arrows = 0;
	/** Each arrow's tail and tip, world - on the lead-in axis, tip toward the stand. */
	TArray<FVector2D> ArrowTails;
	TArray<FVector2D> ArrowTips;
	/** World centre of every lead-in quad painted - the tests measure it against the sampled
	 *  edges (the graph samples once) and the pavement (the strip gap). */
	TArray<FVector2D> LeadInCentres;
};

/**
 * THE TURN-OFF PAINT (taxiway strip stage 5; spec "Paint (user, from BHX)"): where a stand's
 * lead-in leaves a taxiway, whatever of the lead-in itself lies on the TAXIWAY PAVEMENT (not the
 * sweeps onto the centreline - see below), and a SIGN on the lead-in's own axis just off the centreline: a short straight arrow
 * into the stand between two black boxes carrying the stand number, one reading from each side
 * (user 2026-09-29, samples/standsigns.png - it replaced a chevron half-way round the sweep and a
 * single number beside the centreline). The strip
 * between the pavement edge and the stand carries no paint - the gap - and the yellow resumes
 * inside the stand's white lines, which FStandMarkingBuilder paints.
 *
 * THE GAP IS "PAINT ONLY WHERE PAVED", NOT A SECOND RULE: every lead-in quad comes from
 * GuidelineGeom::Sample of the derived lead edge FAnchorLink::Join laid (so the guideline graph
 * still samples once), kept only where it lies on taxiway pavement. The two SWEEPS Join lays
 * from the lead end onto the taxiway are NOT painted (user 2026-09-29: not needed; the arrow
 * marks the turn-off) - the aircraft still turns along them, unpainted. Where the lead end sits
 * behind the pavement edge, which is the usual case, the turn-off paint is the sign alone.
 * Nothing records a turn-off; it is re-derived from the graph on every Topology rebuild, which
 * is when RebuildMarkings runs, after FAnchorLink::Build.
 * ENFORCED BY: Airside.Present.StandTurnOff.PaintsAfterPlacement,
 * Airside.Build.StandTurnOff.PaintsNoSweep, ...NoRoomBranch and ...LeadInOnlyOnPavement.
 *
 * A SEPARATE BUILDER, not more of FStandMarkingBuilder: that one paints inside a stand's own box
 * and is clipped to it; this paints outside every box, on ground the stand does not own.
 *
 * THE SIGN'S FIGURES BELOW ARE FIRST GUESSES FROM THE SAMPLE, UNTUNED (2026-09-29): to be judged
 * live with the owner in the editor and changed here.
 */
struct AIRSIDE_API FStandTurnOffMarkingBuilder
{
	/** From the taxiway centreline to the arrow's tail, along the axis, uu - clear of the
	 *  centreline paint. UNTUNED. */
	static constexpr double ArrowStartFromCentreline = 150.0;
	/** Arrow tail to tip, uu - "short and straight". Shortened to fit a narrow taxiway. UNTUNED. */
	static constexpr double ArrowLength = 400.0;
	/** The arrow head: along the axis, and across at its base, uu. UNTUNED. */
	static constexpr double ArrowHeadLength = 120.0;
	static constexpr double ArrowHeadWidth = 150.0;
	/** What the arrow keeps clear of the pavement edge, uu. */
	static constexpr double ArrowEdgeMargin = 50.0;
	/** Number glyph height, uu - across the axis, since the text runs along it. UNTUNED. */
	static constexpr double NumberHeight = 120.0;
	/** Gap between the number's glyphs, a fraction of a cell's width - the runway's own 0.4. UNTUNED. */
	static constexpr double NumberSpacing = 0.4;
	/** From the axis to each black box's inner edge, uu. UNTUNED. */
	static constexpr double SignFaceGap = 60.0;
	/** Black margin round the digits inside each box, uu. UNTUNED. */
	static constexpr double SignBoxPadding = 25.0;
	/**
	 * How far BELOW the rest of the paint the black boxes lie, uu. The digits are painted on top
	 * of their box; at one Z the two would z-fight. A quarter of a unit keeps the box above the
	 * pavement the paint layer already clears by half a unit (URoadSurfacePresenter::GetMarkingZ).
	 */
	static constexpr double SignBoxZDrop = 0.25;

	/**
	 * Append the turn-off paint of every live stand in Network to Out at Z. Returns how many
	 * turn-offs were painted. The lead-in, arrow and digits carry PaintIds[EStandPaint::Guidance] -
	 * the same meaning as the lead-in inside the box - and the boxes EStandPaint::SignBackground,
	 * resolved to slots by the caller.
	 *
	 * A lead-in is an Aircraft edge on the stand's pose node that is neither a road's own
	 * guideline (DerivedFrom unset) nor the stand's service lane (StandGeometryOwner unset); its
	 * far end is the lead end. The sweeps are the CURVED links leaving the lead end; with none,
	 * the lead end is the taxiway node itself (Join's no-room branch). A lead end that reaches no
	 * taxiway segment paints nothing - the stand is still numbered, just not signed.
	 */
	static int32 Build(const URoadNetwork& Network, double Z, FRoadMeshBuffers& Out,
		FStandTurnOffCensus* Census = nullptr, const FStandPaintIds& PaintIds = FStandPaintIds());
};
