#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Solve/IcaoCode.h"

class URoadNetwork;

/**
 * The clearance strip beside every taxiway, as a keep-out question: does this footprint stand
 * where a taxiing wing sweeps? (taxiway clearance strip spec, 2026-09-28)
 *
 * ONE QUERY FOR EVERYTHING PLACED - stands now, roads and building plots in stage 3 - rather
 * than a rule per object type, so a road with buildings on it needs no special case: each
 * footprint asks the same question. World-free, so it is tested with a bare URoadNetwork.
 */
namespace TaxiwayStrip
{
	/**
	 * A road or taxiway that exists or is about to - enough to know its ground. The tool's
	 * preview, the facade's commit, a moved node and a heal all describe the segment they are
	 * ABOUT to make with one of these, so none of them needs a scratch network to be judged.
	 */
	struct FSegmentShape
	{
		FVector2D A = FVector2D::ZeroVector;
		/** (A+B)/2 for straight - GuidelineGeom's own spelling of it. */
		FVector2D Control = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		/** The WIDER half, GetMaxHalfWidth() - WorstIntrusion's own asymmetric-profile rule. */
		double HalfWidth = 0.0;
	};

	/**
	 * The pavement polygon: both edges of the sampled centreline, counter-clockwise (positive
	 * RoadGeom::PolygonArea), 2 * (GuidelineGeom::DefaultSamples + 1) points.
	 *
	 * ALWAYS SAMPLED, EVEN STRAIGHT - unlike GuidelineGeom::Sample's two-point short circuit -
	 * so a caller splitting it into per-sample quads (JudgeSegment's reverse query) sees one
	 * layout for every shape. The edges offset along GuidelineGeom::Tangent's ANALYTIC normal:
	 * differencing the samples would be a second evaluator of the same curve.
	 */
	AIRSIDE_API TArray<FVector2D> FootprintOf(const FSegmentShape& Shape);

	/** The shape of a live segment, its half-width through ProfileFor; false if dead or profile-less. */
	AIRSIDE_API bool ShapeOf(const URoadNetwork& Network, FRoadSegmentId Id, FSegmentShape& Out);

	/** One taxiway whose strip a footprint enters, and how far. */
	struct FIntrusion
	{
		FRoadSegmentId Taxiway;
		/** The taxiway's letter - what set the strip. */
		EIcaoCode Letter = EIcaoCode::A;
		/** The strip's width, uu - IcaoCode::TaxiwayStripForWidth of the pavement. */
		double Required = 0.0;
		/** How far inside the strip's outer edge the footprint reaches, uu. */
		double Depth = 0.0;
	};

	/**
	 * How far a footprint may reach past a strip edge before it counts, uu. One centimetre,
	 * the facade's OverlapToleranceUu, for the same reason: a stand the tool placed exactly on
	 * the edge must not be refused by the sampled curve's float noise.
	 */
	inline constexpr double ToleranceUu = 1.0;

	/**
	 * Does this segment carry aircraft and nothing else? THE taxiway-profile rule, moved here
	 * from PlotGesture::IsTaxiway (which now forwards) so Model/ can ask it - Model may not
	 * include Tool/. A runway passes this too; HasStrip is the one that excludes it.
	 */
	AIRSIDE_API bool IsAircraftOnly(const URoadNetwork& Network, FRoadSegmentId Id);

	/** A taxiway with a strip: aircraft only, and not a runway (runways have their own rules). */
	AIRSIDE_API bool HasStrip(const URoadNetwork& Network, FRoadSegmentId Id);

	/** The strip each side of this segment, uu; 0 for anything HasStrip refuses. */
	AIRSIDE_API double StripWidthOf(const URoadNetwork& Network, FRoadSegmentId Id);

	/**
	 * The deepest strip intrusion of a closed footprint polygon (any winding), or unset when it
	 * is clear of every strip by ToleranceUu. Pavement counts as strip: a footprint over the
	 * taxiway itself intrudes by the whole strip and more.
	 *
	 * DEEPEST, NOT ALL: every caller so far reports one reason, and the deepest is the one to
	 * fix first. A caller that needs the list is the day this grows one.
	 *
	 * Exempt names taxiways to skip - the ones a new segment MEETS (JudgeSegment), whose strip
	 * it may cross by definition. Stands and plots pass nothing: they meet no taxiway.
	 */
	AIRSIDE_API TOptional<FIntrusion> WorstIntrusion(const URoadNetwork& Network, TConstArrayView<FVector2D> Footprint,
		TConstArrayView<FRoadSegmentId> Exempt = {});

	/**
	 * Where a new segment's end meets the network: a node it shares, or a point on a segment it
	 * will split. Unset Node and Segment = a free end.
	 */
	struct FSegmentEnd
	{
		FRoadNodeId Node;
		/** A Segment snap: the ORIGINAL segment, before any split - the preview's ghost network
		 *  has split it and the real one has not, so only this id names the same taxiway in both. */
		FRoadSegmentId Segment;
		FVector2D At = FVector2D::ZeroVector;
	};

	/** What a placement is refused for, if anything. Written once; tool and facade both show Text. */
	struct FStripVerdict
	{
		bool bRefused = false;
		FString Text;
		/** How far inside, uu, for a strip refusal; 0 for an angle or a reverse-query refusal. */
		double Depth = 0.0;
	};

	/**
	 * Meets within 30 degrees of square. PER ARM at a node: at least MeetMinDegrees from EVERY
	 * strip-bearing arm there, i.e. never running back along one. Across a through-taxiway
	 * (two opposite arms) that IS the 60..120 band; at a split, the band is asked directly.
	 *
	 * NO SEPARATE "STRAIGHT ON" BAND, unlike the plan's 150-degree ruling 3 (ruled 2026-09-29,
	 * while implementing): at a taxiway's DEAD END the only arm is behind the new segment, so
	 * 120..150 is a bend heading AWAY from it, not a diagonal along its strip - and the plan's
	 * bands refused a taxiway chained on with a 45-degree bend while admitting a 90-degree one.
	 * Straight on (180) passes this rule as it passed ruling 3's.
	 */
	inline constexpr double MeetMinDegrees = 60.0;
	inline constexpr double MeetMaxDegrees = 120.0;

	/**
	 * Two strip-bearing pieces at a node with no third are ONE TAXIWAY when they run on within
	 * 30 degrees of straight (arms 150+ degrees apart) - what JudgeSegment's exemption walks
	 * along. Past a sharper bend the next leg is another line to meet, square, in its own right.
	 */
	inline constexpr double ChainStraightMinDegrees = 150.0;

	/**
	 * May a road or taxiway of this shape be laid with these ends? Refuses when its pavement
	 * enters the strip of a taxiway it does not MEET (share a node / split, at an allowed angle),
	 * or - bIsTaxiway - when its own strip would contain an existing stand, depot, road or other
	 * taxiway's pavement it does not meet. Ignore lists the segments a caller is replacing (a
	 * moved node's own incident segments, a heal's two stubs).
	 *
	 * ONE JUDGE FOR EVERY PAVEMENT PATH - the road tool's preview and click, the facade's
	 * ConnectNodes, MoveNode and the delete heal - so the readout cannot approve what the commit
	 * refuses (the WhyStandRefused pattern). Aprons and runways never reach it (plan rulings 1-2).
	 */
	AIRSIDE_API FStripVerdict JudgeSegment(const URoadNetwork& Network, const FSegmentShape& Shape,
		bool bIsTaxiway, const FSegmentEnd& AtA, const FSegmentEnd& AtB,
		TConstArrayView<FRoadSegmentId> Ignore = {});
}
