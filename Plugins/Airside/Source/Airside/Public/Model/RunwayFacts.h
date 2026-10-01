#pragma once

#include "CoreMinimal.h"
#include "Model/Pavement.h"
#include "Model/RoadHandles.h"
#include "RunwayFacts.generated.h"

/**
 * What the approach aids support. ORDERED: an aircraft names the least it needs.
 *
 * Until M5's weather exists this refuses only an aircraft that declares a need; it is
 * kept as a fact on the runway now so the paint (aiming point, touchdown zone) and the
 * later minima read one classification rather than two.
 */
UENUM(BlueprintType)
enum class ERunwayApproach : uint8
{
	Visual,
	NonPrecision,
	Precision,
	/** Sentinel, never a real approach - sizes % cycling instead of retyping 3. */
	Count UMETA(Hidden),
};

/**
 * What traffic a runway takes - the player's choice, on the runway's card (2026-09-29).
 *
 * WHY IT EXISTS: with two runways every arrival went to the one nearest the longest strip's
 * threshold and every departure to the shortest taxi, so the second runway sat empty
 * (samples/2runways.png). Mixed now means "free first" in both planners; the two dedicated
 * modes let a player segregate parallels the way a real field does.
 *
 * Unset = 0 FOR THE WRITE, InUse's own reason (FRunwayFacts::InUse): every caller that builds a
 * fresh FRunwayFacts to reclassify a surface would otherwise reset the player's choice - so
 * Unset on a write KEEPS what the strip has, and Unset on a read is Mixed (RunwayUse::Resolve),
 * which is also what a runway saved before the field loads as.
 */
UENUM(BlueprintType)
enum class ERunwayUse : uint8
{
	Unset UMETA(Hidden),
	Mixed,
	ArrivalsOnly,
	DeparturesOnly,
};

namespace RunwayUse
{
	/** Unset reads as Mixed - see ERunwayUse. */
	inline ERunwayUse Resolve(ERunwayUse Use) { return Use == ERunwayUse::Unset ? ERunwayUse::Mixed : Use; }
	inline bool Lands(ERunwayUse Use) { return Resolve(Use) != ERunwayUse::DeparturesOnly; }
	inline bool Departs(ERunwayUse Use) { return Resolve(Use) != ERunwayUse::ArrivalsOnly; }
	/** The one after Use, for the card's button: Mixed -> Arrivals only -> Departures only -> Mixed. */
	inline ERunwayUse Next(ERunwayUse Use)
	{
		switch (Resolve(Use))
		{
		case ERunwayUse::Mixed:        return ERunwayUse::ArrivalsOnly;
		case ERunwayUse::ArrivalsOnly: return ERunwayUse::DeparturesOnly;
		default:                       return ERunwayUse::Mixed;
		}
	}
	/** For logs and the card. */
	inline const TCHAR* Name(ERunwayUse Use)
	{
		switch (Resolve(Use))
		{
		case ERunwayUse::ArrivalsOnly:   return TEXT("arrivals only");
		case ERunwayUse::DeparturesOnly: return TEXT("departures only");
		default:                         return TEXT("mixed");
		}
	}
}

/**
 * The facts about ONE runway that are neither its cross-section nor its length.
 *
 * On the SEGMENT (FRoadSegment::Runway), not on a profile asset: four surfaces by three
 * approaches by five widths is sixty assets, and a runway whose profile is not an asset
 * reloads as a taxiway (the null-profile fallback). The profile stays the cross-section;
 * width is the profile's and length is the chain's. Every segment of a strip carries the
 * same facts - the tool writes the whole chain and a split copies them.
 *
 * Defaults are what an existing level loads as: the runways the project has today are
 * tarmac with no approach aids.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FRunwayFacts
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere) EPavement Surface = EPavement::Tarmac;
	UPROPERTY(EditAnywhere) ERunwayApproach Approach = ERunwayApproach::Visual;

	/**
	 * THE RUNWAY IN USE: the designator (1-36) of the end both landings and take-offs use -
	 * land over that threshold, take off from it, the same heading (spec
	 * 2026-09-28-runway-in-use). The player's choice, standing in for the wind the game does
	 * not model; one direction per strip is what keeps arrivals and departures from meeting
	 * nose to nose on a connector (samples/deadlock.png).
	 *
	 * A DESIGNATOR, not a vector or a chain-end flag: splits, heals and drags reorder which
	 * node a chain's walk finds first, and a flag relative to that order would silently
	 * reverse the runway. A number is a heading, and RunwayQuery::InUseEnd resolves it to
	 * whichever end is nearer it - so a strip dragged round 30 degrees keeps its direction.
	 *
	 * 0 = UNSET: a runway saved before this field. Resolves to the LOWER designator
	 * (RunwayQuery::InUseEnd), deterministic rather than whatever end a query was nearest.
	 * A new runway segment is given its drawn direction by URoadNetwork::AddSegment.
	 */
	UPROPERTY(EditAnywhere) int32 InUse = 0;

	/** What traffic this strip takes - see ERunwayUse. Unset keeps on a write, reads Mixed. */
	UPROPERTY(EditAnywhere) ERunwayUse Use = ERunwayUse::Unset;

	bool operator==(const FRunwayFacts& Other) const
	{
		return Surface == Other.Surface && Approach == Other.Approach && InUse == Other.InUse && Use == Other.Use;
	}
	bool operator!=(const FRunwayFacts& Other) const { return !(*this == Other); }
};

/**
 * What an aircraft needs of a runway, on the type and copied into FAirframe at dispatch.
 *
 * The field lengths are PUBLISHED figures for ADMISSION, not the physics' rolls: the
 * follower keeps deriving FTakeoffRun::RequiredRoll and FLandingRun's distance for
 * MOTION. Two numbers for one thing is deliberate and the test
 * Airside.Model.FieldLengthsCoverTheRoll pins their relation - a published figure may be
 * generous but may never be shorter than what the model actually needs, or admission
 * would accept a strip the aircraft then runs off the end of.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FRunwayRequirements
{
	GENERATED_BODY()

	/** The least approach it needs. Visual means any runway. */
	UPROPERTY(EditAnywhere) ERunwayApproach ApproachNeeded = ERunwayApproach::Visual;

	/** Published take-off field length, uu. 0 means no claim, and no length refusal. */
	UPROPERTY(EditAnywhere) double TakeoffFieldLength = 0.0;

	/** Published landing field length, uu. 0 means no claim. */
	UPROPERTY(EditAnywhere) double LandingFieldLength = 0.0;
};

/**
 * The approach scale's word, lower case, for a refusal sentence or a tool label. One
 * spelling, here, so the admission text and the runway tool's bar agree. The pavement
 * scale's word is Pavement::Name (Model/Pavement.h) - moved there with the enum.
 */
AIRSIDE_API const TCHAR* RunwayApproachName(ERunwayApproach Approach);

/**
 * One end of a runway strip: the threshold an aircraft crosses it at, the direction it
 * points from there, how much strip lies beyond, and the segment the query actually named
 * or landed nearest.
 *
 * ONE STRUCT for a triple that used to travel loose through nine places (issue #88):
 * URoadNetwork::RunwayExtentAt/NearestRunwayThreshold each filled four out-params:
 * FArrivalPlan, FDeparturePlan, FDepartureOrder, FRunwaySummary, FTakeoffRun and
 * FLandingRun each carried their own copy of Threshold/Direction/Length (RunwaySegment
 * travelling as a fifth, separate field alongside); the far end was recomputed as
 * Threshold + Direction * Length at four more call sites, and an along-strip offset as
 * Dot(P - Threshold, Direction) at four others. Bundling the fields is what
 * FSpeedProfile::Build(const FGroundPerformance&) already does for a performance figure;
 * this is the same rule for a place on the strip.
 */
USTRUCT()
struct AIRSIDE_API FRunwayEnd
{
	GENERATED_BODY()

	/** Where the aircraft crosses onto (or departs from) the strip. */
	UPROPERTY() FVector2D Threshold = FVector2D::ZeroVector;

	/** Unit vector from Threshold toward the far end. */
	UPROPERTY() FVector2D Direction = FVector2D(1.0, 0.0);

	/** Runway available beyond Threshold, uu. */
	UPROPERTY() double Length = 0.0;

	/** The runway segment the query actually named, or landed nearest. */
	UPROPERTY() FRoadSegmentId Seed;

	/** The point Along uu down the strip from Threshold (negative is short of it) - the one spelling of
	 *  "Threshold + Direction * x", which FLandingRun and FTakeoffRun each wrote into their pose (issue #444). */
	FVector2D PointAt(double Along) const { return Threshold + Direction * Along; }

	/** The strip's other end: Threshold walked the whole Length along Direction. */
	FVector2D FarEnd() const { return PointAt(Length); }

	/** How far along the strip, from Threshold, Position projects. Negative is short of it. */
	double OffsetOf(const FVector2D& Position) const
	{
		return FVector2D::DotProduct(Position - Threshold, Direction);
	}

	/**
	 * The SAME strip, described from its other end: Threshold becomes FarEnd(), Direction
	 * reverses, Length is unchanged. Seed is unchanged too - Reversed() renames which end
	 * this struct is measured from, not which segment it was asked about.
	 */
	FRunwayEnd Reversed() const
	{
		FRunwayEnd Out;
		Out.Threshold = FarEnd();
		Out.Direction = -Direction;
		Out.Length = Length;
		Out.Seed = Seed;
		return Out;
	}
};
