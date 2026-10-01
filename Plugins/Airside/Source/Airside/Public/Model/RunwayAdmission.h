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
	/**
	 * Arrival verdicts only, never from Judge (#433): every runway is set to arrivals only, so nothing on the
	 * field could take the landing aircraft's departure. About the FIELD's setting, not any one strip's figures,
	 * so it carries none. Last, because the order above is the order Judge checks its refusals in.
	 */
	NoDepartureRunway,
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

	/** The surface comparison - see FPavementCheck. Written by Judge whatever the verdict. */
	UPROPERTY() FPavementCheck Pavement;

	/** What the aircraft needed of it. */
	UPROPERTY() FRunwayRequirements Required;

	/** The strip's length and the field length the operation was judged against, uu. */
	UPROPERTY() double RunwayLength = 0.0;
	UPROPERTY() double FieldLength = 0.0;

	/** The aircraft's wingspan and the widest the strip admits, uu. */
	UPROPERTY() double Wingspan = 0.0;
	UPROPERTY() double MaxWingspan = 0.0;

	/**
	 * The verdict is about LEAVING: an arrival whose landing fits, refused because no runway
	 * takes its departure - see CheckArrival. RunwayLength and FieldLength are then the
	 * longest DEPARTURE runway's and the take-off figure - or none, for NoDepartureRunway.
	 * False for every Check.
	 */
	UPROPERTY() bool bForDeparture = false;

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

	/**
	 * May this aircraft ARRIVE here: land on LandingSeed, and later leave from SOME runway.
	 *
	 * THE LANDING FIRST, on its own runway, and its refusal is returned as it is. Then the
	 * departure, against every runway that TAKES DEPARTURES (RunwayQuery::DepartureRunways -
	 * the same enumeration DeparturePlanner::PlanAny walks, so the two cannot disagree about
	 * where a departure may go), because PlanAny skips a refused runway - so a short landing
	 * strip beside a long departure one is an airport the type can use. Refused when none
	 * takes it, with bForDeparture set and the LONGEST departure runway's verdict, the one
	 * most nearly able to; or NoDepartureRunway when the field has runways and none takes
	 * departures (#433: it used to ask every runway, an arrivals-only one included, and
	 * admitted aircraft that landed and could never leave their stand).
	 * ENFORCED BY: Check-Architecture.ps1 rule 39 (RunwayAdmission.cpp must call DepartureRunways and may not walk the runways or read a use setting itself).
	 *
	 * WHY IT EXISTS (2026-09-27): once field lengths became the model's roll x 1.1, landing asked
	 * less than take-off for most types, and an SR22 landed on a 404 m strip it could never
	 * leave (Airside.Model.ArrivalAdmitsOnlyWhatCanLeave). Every arrival admission asks this,
	 * not Check - ArrivalPlanner::Plan (offers, the board, the Land key) and the Land panel.
	 */
	AIRSIDE_API FRunwayAdmission CheckArrival(const URoadNetwork& Network, FRoadSegmentId LandingSeed,
		const FAirframe& Airframe);

	/** The widest wingspan a strip of TotalWidth admits by ICAO code, uu. */
	AIRSIDE_API double MaxWingspanForWidth(double TotalWidth);

	/** The sentence for a refusal: "the surface is grass; this aircraft needs tarmac". Empty when admitted. In metres. */
	AIRSIDE_API FString Describe(const FRunwayAdmission& Admission);

	/**
	 * A length the airport HAS, uu, as whole metres for a refusal's sentence - rounded DOWN; and one the aircraft NEEDS -
	 * rounded UP (#497 review). A refusal says have < need, and rounding both to nearest printed "900 m; 900 m" for 899.6
	 * against 900.2 - a refusal that reads as a pass. Floor and ceiling keep the printed pair in the order of the real one.
	 * The arrival and departure sentences use these, so every player-facing length rounds the same way.
	 * ENFORCED BY: Airside.Model.RunwayAdmission ("never a tie")
	 */
	AIRSIDE_API double HaveMetres(double Uu);
	AIRSIDE_API double NeedMetres(double Uu);
}
