#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Model/RunwayFacts.h"
#include "RunwayAdmission.generated.h"

class URoadNetwork;
struct FAirframe;

/** Why an aircraft may not use a runway. None means it may. */
UENUM()
enum class ERunwayRefusal : uint8
{
	None,
	/** The pavement is weaker than the aircraft's minimum. */
	Surface,
	/** The approach aids are less than the aircraft declares it needs. */
	Approach,
	/** The strip is shorter than the published field length for the operation. */
	TooShort,
	/** The wingspan exceeds what the strip's width admits. */
	TooNarrow,
};

/**
 * One admission decision with the figures it was made from, so the sentence that
 * explains it can be written from the decision rather than by re-deriving it - the rule
 * FArrivalPlan already follows for DescribeRefusal.
 *
 * A USTRUCT because both planners carry it in their plan, and the plans are USTRUCTs.
 */
USTRUCT()
struct AIRSIDE_API FRunwayAdmission
{
	GENERATED_BODY()

	UPROPERTY() ERunwayRefusal Why = ERunwayRefusal::None;

	/** What the runway is. */
	UPROPERTY() FRunwayFacts Facts;

	/** What the aircraft needed of it. */
	UPROPERTY() FRunwayRequirements Required;

	/** The strip's length and the field length the operation was judged against, uu. */
	UPROPERTY() double RunwayLength = 0.0;
	UPROPERTY() double FieldLength = 0.0;

	/** The aircraft's wingspan and the widest the strip admits, uu. */
	UPROPERTY() double Wingspan = 0.0;
	UPROPERTY() double MaxWingspan = 0.0;

	bool IsAdmitted() const { return Why == ERunwayRefusal::None; }
};

/**
 * May this aircraft use this runway? Model/, no world: the planners ask it first, and
 * M3's offers will ask it before an offer is made, so the answer lives in one place.
 *
 * Free functions over a const URoadNetwork&, RouteSearch's shape - nothing to own.
 */
namespace RunwayAdmission
{
	/**
	 * The comparison itself, over figures rather than a graph.
	 *
	 * Split out from Check so that displaced thresholds and declared distances (out of
	 * scope, spec §1) can be added per end later by handing this a different length
	 * without touching either planner: Check derives the figures from the chain today and
	 * would derive them from the end tomorrow.
	 *
	 * The refusals are checked in the order listed on ERunwayRefusal, and the FIRST wins:
	 * a grass strip too short for a jet is refused for its grass, because that is the
	 * fact a player cannot fix by drawing it longer.
	 */
	AIRSIDE_API FRunwayAdmission Judge(const FRunwayFacts& Facts, double RunwayLength,
		double MaxWingspan, const FAirframe& Airframe, bool bLanding);

	/**
	 * Judge the runway Seed belongs to. Seed that is not a live runway is ADMITTED: not
	 * being a runway is the planners' NoRunway, not an admission refusal, and they ask
	 * that first.
	 *
	 * Length is the chain's (RunwayExtentAt). Width is the profile's first guideline
	 * MaxWingspan where declared, else the ICAO code letter the total width implies -
	 * see Solve/IcaoCode.h for the table and its provenance; nearest width wins, so a
	 * 36 m airliner is refused a 23 m strip.
	 */
	AIRSIDE_API FRunwayAdmission Check(const URoadNetwork& Network, FRoadSegmentId Seed,
		const FAirframe& Airframe, bool bLanding);

	/** The widest wingspan a strip of TotalWidth admits by ICAO code, uu. */
	AIRSIDE_API double MaxWingspanForWidth(double TotalWidth);

	/** The sentence for a refusal: "the surface is grass; this aircraft needs tarmac". Empty when admitted. */
	AIRSIDE_API FString Describe(const FRunwayAdmission& Admission);
}
