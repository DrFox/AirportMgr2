#pragma once

#include "CoreMinimal.h"

#include "AirlineViewModels.generated.h"

class UAirlineHistory;
class UAirlineRoster;
class UFlightBoard;
class UOfferGenerator;
class UOpsRuntime;
class USimClock;
struct FAirlineDays;
struct FAirlineOffers;
enum class EAirlineSatisfactionCause : uint8;

/** Which way an airline's satisfaction has gone today against yesterday's close. */
enum class EAirlineTrend : uint8 { Up, Down, Flat };

/** One row of the airlines list. Built only from roster standings (UAirlineListViewModel::BuildRows), so every row HAS one - no
 *  bHasStanding here, unlike FAirlineDetail, which is asked for an id. */
struct FAirlineListRow
{
	FName AirlineId;
	FText Name;
	int32 SatisfactionPct = 0;
	EAirlineTrend Trend = EAirlineTrend::Flat;
	bool bFloor = false;
};

/** One cause over the kept week: how often it moved the airline, and by how much in all ("+18%"). */
struct FAirlineTallyRow
{
	FText Label;
	int32 Count = 0;
	double SumDelta = 0.0;
	FText DeltaText;
};

/** One fleet type: can it come here, and the plan's sentence when it cannot. */
struct FAirlineFleetRow
{
	FText TypeName;
	bool bAdmitted = false;
	/** Empty when admitted. */
	FText Reason;
};

/** One of the airline's offers awaiting an answer - read-only: the inbox is the one place that acts on an offer. */
struct FAirlineOfferRow
{
	FText Callsign;
	FText TypeName;
	FText Countdown;
};

/** One of the airline's flights between accept and airborne. */
struct FAirlineFlightRow
{
	FText Callsign;
	FText TypeName;
	FText Phase;
	FText Contract;
	bool bLate = false;
};

/** Everything the detail pane draws for one airline. */
struct FAirlineDetail
{
	FText Name;
	/** The roster has a standing for this airline. FALSE: SatisfactionPct is meaningless (0) and the widget prints "—", never "0%"
	 *  (review focus 1) - an id the roster was never seeded with, or a detail built before the seed. */
	bool bHasStanding = false;
	/** Meaningful only when bHasStanding. */
	int32 SatisfactionPct = 0;
	/** "~3.1 offers/h now"; empty with no generator or clock, or when the airline has no catalog definition. */
	FText RateLine;
	/** "mood x0.9 · 3 of 5 types can come", or "mood x1.0 · fleet not judged yet"; empty with no generator, or when the airline has
	 *  no catalog definition. */
	FText FactorLine;
	/** Each kept day's close (today's running value last), 0..1, oldest first - missing days omitted. Empty when !bHasHistory. */
	TArray<double> Trend;
	/** At least one day has CLOSED. False on a fresh game's first day: the pane says "no history yet" and shows today's tally only. */
	bool bHasHistory = false;
	TArray<FAirlineTallyRow> Tallies;
	/** The generator has judged this airline's fleet. False before its first admission check - "not judged yet", never "all crossed". */
	bool bJudged = false;
	TArray<FAirlineFleetRow> Fleet;
	TArray<FAirlineOfferRow> Offers;
	TArray<FAirlineFlightRow> Flights;
};

/**
 * WHAT THE AIRLINES PANEL READS, as the model objects themselves - ONE STRUCT, so the pure builders below take one argument a test
 * fills from NewObject fixtures and the runtime overloads fill from UOpsRuntime (From). Every pointer may be null - a bare fixture
 * that does not care about one section leaves it out, and that section comes back empty. Raw and short-lived: built, read, dropped
 * within one Build call; the runtime owns every object it points at.
 */
struct AIRPORTMGR_API FAirlinePanelSources
{
	const UAirlineRoster* Roster = nullptr;
	const UAirlineHistory* History = nullptr;
	/** The catalog's airlines - for the display name and bIsFloor (UOpsRuntime::GetAirlineOffers, the list the generator ticks). */
	TArrayView<const FAirlineOffers> Airlines;
	const UOfferGenerator* Generator = nullptr;
	const UFlightBoard* Board = nullptr;
	const USimClock* Clock = nullptr;

	/** The runtime's own objects. */
	static FAirlinePanelSources From(const UOpsRuntime& Runtime);
};

/**
 * The airlines LIST: one row per roster standing, floor airlines first (the ones that keep the airport alive), then by name.
 *
 * A VIEWMODEL for UOfferViewModel's reason - the roster and history are AirportOps Model/ and must not learn about the UI - and a
 * PLAIN UObject for the same one: nothing binds a field (see UOfferViewModel's header). The builder is static and pure so a test
 * hands it fixtures; the panel polls it.
 */
UCLASS()
class AIRPORTMGR_API UAirlineListViewModel : public UObject
{
	GENERATED_BODY()

public:
	/** The rows from the runtime's own objects - forwards to the static builder. */
	TArray<FAirlineListRow> BuildRows(const UOpsRuntime& Runtime) const;

	static TArray<FAirlineListRow> BuildRows(const FAirlinePanelSources& Sources);

	/**
	 * Today's RUNNING value (the open day's CloseSatisfaction) against YESTERDAY's close; Flat within FlatWithin, and Flat for an
	 * airline with no closed day yet - there is no yesterday to compare with.
	 */
	static EAirlineTrend TrendOf(const FAirlineDays* Days);

	/** The rule: a change of at most half a percentage point (0.005) is Flat - no direction (spec 2026-10-02 section 2). */
	static constexpr double FlatWithin = 0.005;
};

/**
 * The airlines DETAIL pane for one airline: header, week's trend and causes, fleet verdicts, its offers and its flights.
 *
 * EVERY SENTENCE IS ANOTHER SITE'S: the countdown is the inbox's (UOfferViewModel::DescribeSecondsLeft), the phase word and the contract
 * the arrivals row's (UArrivalRowViewModel::DescribeStatus / DescribeDetail), the fleet reasons the generator's verdicts - so the panel
 * cannot word a flight differently from the window that acts on it. Plain UObject, as UAirlineListViewModel.
 */
UCLASS()
class AIRPORTMGR_API UAirlineDetailViewModel : public UObject
{
	GENERATED_BODY()

public:
	/** The detail from the runtime's own objects - forwards to the static builder. */
	FAirlineDetail Build(const UOpsRuntime& Runtime, FName AirlineId, double Now) const;

	static FAirlineDetail Build(const FAirlinePanelSources& Sources, FName AirlineId, double Now);

	/** The cause's label - EVERY CAUSE BY NAME, so a new one is a build error here rather than a blank row. */
	static FText CauseLabel(EAirlineSatisfactionCause Cause);

	/** "+18%", "-6%", "0%": a summed delta in whole percentage points, signed. */
	static FText DescribeDelta(double SumDelta);
};
