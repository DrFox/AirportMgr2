#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Solve/IcaoCode.h"

class URoadNetwork;
class URoadProfile;

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
		/** The letter the taxiway OPERATES at - what set the strip (StripLetterOf). */
		EIcaoCode Letter = EIcaoCode::A;
		/** The strip's width, uu - StripWidthOf, at the effective letter. */
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

	/** IsAircraftOnly's rule on a profile alone, for a profile not yet on any segment - the
	 *  Upgrade mode's "does this width keep the segment's kind?" (URoadNetwork::SetSegmentProfile).
	 *  IsAircraftOnly forwards here, so the two cannot disagree. False for null. */
	AIRSIDE_API bool IsAircraftOnlyProfile(const URoadProfile* Profile);

	/** A taxiway with a strip: aircraft only, and not a runway (runways have their own rules). */
	AIRSIDE_API bool HasStrip(const URoadNetwork& Network, FRoadSegmentId Id);

	/**
	 * THE strip each side of this segment, uu - at its EFFECTIVE letter (the pavement's, lowered
	 * by a restriction: TaxiwayRestriction::EffectiveLetterOf); 0 for anything HasStrip refuses.
	 *
	 * ORCHESTRATOR RULING, 2026-09-29 (owner asleep): the strip at the effective letter governs
	 * EVERYTHING - stand closure, placement refusal, the inspector card, the Upgrade outline. The
	 * spec says a restricted taxiway "operates at the largest letter whose strip is clear", and
	 * no aircraft wider than that letter may use it, so no wing sweeps the ground between that
	 * letter's strip and the pavement's; a stand there is OPEN. Two answers to "what is this
	 * taxiway's strip" (the pavement's for closure, the restricted one for routing) was the review
	 * finding this replaced. The ONE exception is TaxiwayRestriction::RestrictionOf, which must
	 * try candidate letters over the pavement (IcaoCode::TaxiwayStripFor(L, width)) to FIND the
	 * effective one - it reads the stored letter of no segment, so there is no circle.
	 * ENFORCED BY: Airside.Tool.UpgradeMode (card, closure, placement and outline state one figure)
	 */
	AIRSIDE_API double StripWidthOf(const URoadNetwork& Network, FRoadSegmentId Id);

	/** The letter StripWidthOf is at - the one that set the strip, for a refusal's words. Unset
	 *  for anything HasStrip refuses. */
	AIRSIDE_API TOptional<EIcaoCode> StripLetterOf(const URoadNetwork& Network, FRoadSegmentId Id);

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
	 * (two opposite arms) that IS the 60..120 band; at a split, it is asked of each of the two
	 * straight chords the split will make (final review 4).
	 *
	 * NO SEPARATE "STRAIGHT ON" BAND, unlike the plan's 150-degree ruling 3 (ruled 2026-09-29,
	 * while implementing): at a taxiway's DEAD END the only arm is behind the new segment, so
	 * 120..150 is a bend heading AWAY from it, not a diagonal along its strip - and the plan's
	 * bands refused a taxiway chained on with a 45-degree bend while admitting a 90-degree one.
	 * Straight on (180) passes this rule as it passed ruling 3's.
	 */
	inline constexpr double MeetMinDegrees = 60.0;

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
	 * ENFORCED BY: Airside.Tool.RegistryRulingsAreDeclaredForEveryEntry (every registered tool
	 * has a row: Judged, ExemptApron, ExemptRunway or PlacesNoPavement) and
	 * Airside.Tool.EveryPlacementToolHonoursTheStrip (every Judged row refuses a footprint in a strip)
	 */
	AIRSIDE_API FStripVerdict JudgeSegment(const URoadNetwork& Network, const FSegmentShape& Shape,
		bool bIsTaxiway, const FSegmentEnd& AtA, const FSegmentEnd& AtB,
		TConstArrayView<FRoadSegmentId> Ignore = {});

	/**
	 * JudgeSegment of a segment that already exists, as if it were laid now: its own shape and
	 * nodes, itself (and Ignore) left out. For an edit that re-points a live segment rather than
	 * making one - MergeNodes' Verify asks it of every arm the merge moved.
	 */
	AIRSIDE_API FStripVerdict JudgeExisting(const URoadNetwork& Network, FRoadSegmentId Id,
		TConstArrayView<FRoadSegmentId> Ignore = {});

	/** Something a strip is over: what JudgeSegment's own-strip step found, by kind and index. */
	struct FSwallowed
	{
		enum class EKind : uint8 { Stand, Depot, Road, Taxiway };
		EKind Kind = EKind::Road;
		/** An entity index for Stand and Depot, a segment index for Road and Taxiway. */
		int32 Index = INDEX_NONE;
	};

	/**
	 * JudgeSegment's own-strip step asked of a LIVE taxiway at a strip width of the caller's
	 * choosing - the restriction pass's question, "is the strip at letter L clear?" (stage 6).
	 * Ends and exemption as JudgeExisting's (its own nodes, itself ignored, the met taxiway's
	 * straight chain walked), but no angle refusal: a badly met arm is Met, not an obstruction,
	 * as step 3 has always treated it. ONE MACHINERY with JudgeSegment (factored, not copied),
	 * so the restriction and the placement refusal cannot disagree about what a strip is over.
	 * bCountStands false for the restriction: a stand closes instead (plan ruling 3).
	 * Unset for anything HasStrip refuses.
	 * ENFORCED BY: Airside.Model.TaxiwayRestriction, Airside.Model.TaxiwayStrip.SegmentJudge
	 */
	AIRSIDE_API TOptional<FSwallowed> StripSwallows(const URoadNetwork& Network, FRoadSegmentId Taxiway,
		double StripWidth, bool bCountStands);

	/** The meeting-angle rule on one arm: at least MeetMinDegrees from it. JudgeSegment's own
	 *  test, public for MoveNode, which also judges a node's moved arms against each other. */
	AIRSIDE_API bool MeetsAtAllowedAngle(double Degrees);

	/** The refusal for an arm met at Degrees, in the one wording every caller shows. */
	AIRSIDE_API FString MeetingRefusal(const URoadNetwork& Network, FRoadSegmentId Taxiway, double Degrees);
}
