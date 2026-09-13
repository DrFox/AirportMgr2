#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "RunwayFacts.generated.h"

/**
 * What a runway is paved with. ORDERED: strength as well as look, so an aircraft names
 * the weakest surface it may use and admission compares with <.
 *
 * Pavement strength is the surface scale itself rather than a separate PCN figure
 * (spec 2026-09-07 §1): four steps is what a player can read off the ground, and a
 * classification number would be a second axis nothing in the game varies independently.
 */
UENUM(BlueprintType)
enum class ERunwaySurface : uint8
{
	Grass,
	Tarmac,
	Concrete,
	Reinforced,
};

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
};

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

	UPROPERTY(EditAnywhere) ERunwaySurface Surface = ERunwaySurface::Tarmac;
	UPROPERTY(EditAnywhere) ERunwayApproach Approach = ERunwayApproach::Visual;

	bool operator==(const FRunwayFacts& Other) const
	{
		return Surface == Other.Surface && Approach == Other.Approach;
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

	/** The weakest surface this aircraft may use. */
	UPROPERTY(EditAnywhere) ERunwaySurface MinimumSurface = ERunwaySurface::Grass;

	/** The least approach it needs. Visual means any runway. */
	UPROPERTY(EditAnywhere) ERunwayApproach ApproachNeeded = ERunwayApproach::Visual;

	/** Published take-off field length, uu. 0 means no claim, and no length refusal. */
	UPROPERTY(EditAnywhere) double TakeoffFieldLength = 0.0;

	/** Published landing field length, uu. 0 means no claim. */
	UPROPERTY(EditAnywhere) double LandingFieldLength = 0.0;
};

/**
 * The words for the two scales, lower case, for a refusal sentence or a tool label.
 * One spelling each, here, so the admission text and the runway tool's bar agree.
 */
AIRSIDE_API const TCHAR* RunwaySurfaceName(ERunwaySurface Surface);
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
USTRUCT(BlueprintType)
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

	/** The strip's other end: Threshold walked the whole Length along Direction. */
	FVector2D FarEnd() const { return Threshold + Direction * Length; }

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
