#pragma once

#include "CoreMinimal.h"
#include "Model/AirlineRoster.h"
#include "Model/OpsSave.h"
#include "UObject/Object.h"
#include "AirlineHistory.generated.h"

/** How often one cause moved an airline on one day, and by how much in all. */
USTRUCT()
struct AIRPORTOPS_API FAirlineCauseTally
{
	GENERATED_BODY()

	UPROPERTY() EAirlineSatisfactionCause Kind = EAirlineSatisfactionCause::OnTime;
	UPROPERTY() int32 Count = 0;
	/** Sum of the CLAMPED deltas, so a cause that hit the ceiling tallies what it really moved. */
	UPROPERTY() double SumDelta = 0.0;
};

/** One game day of one airline: the day's causes and where satisfaction stood at its end. */
USTRUCT()
struct AIRPORTOPS_API FAirlineDay
{
	GENERATED_BODY()

	/** The clock's day number (USimClock::Day) - the number the day began under. */
	UPROPERTY() int32 Day = 0;
	/** The running value while the day is open, the close once it is closed. */
	UPROPERTY() double CloseSatisfaction = 0.0;
	UPROPERTY() TArray<FAirlineCauseTally> Tallies;
};

/** One airline's kept days, oldest first; the last is today and still open. */
USTRUCT()
struct AIRPORTOPS_API FAirlineDays
{
	GENERATED_BODY()

	UPROPERTY() FName AirlineId;
	UPROPERTY() TArray<FAirlineDay> Days;
};

/**
 * A week of WHY each airline's satisfaction moved: per day, per cause, a count and a summed delta. The airlines panel's
 * trend line and cause list read it.
 *
 * FED BY THE ROSTER, NOT SUBSCRIBED TO THE BUS (a deviation from the spec's first draft, 2026-10-02-airlines-panel section 1.2).
 * The day's own drift is applied by the roster's FDayEndedEvent handler, and that is the same event a bus subscriber of this
 * class would close the day on: two Reaction handlers of one event run in subscription order, so the drift would land in
 * whichever day the order happened to favour. The roster calls Record from Apply - after the clamp, only when the value
 * moved, so the history cannot disagree with the satisfaction event - and CloseDay after its own drift loop, so the drift
 * belongs to the day it forgives.
 * ENFORCED BY: AirportOps.Model.AirlineHistory.DriftBelongsToTheDayItCloses
 *
 * KEEPS ITS OWN DAY NUMBER rather than being handed the clock's on every Record: the roster would need a clock it has never
 * had, and FDayEndedEvent::Day is the day that BEGINS at the midnight beat (the beat fires when the clock has already rolled
 * over), so CloseDay counts forward from CurrentDay instead of trusting a label. UOpsRuntime sets the start day from the
 * clock at a new game, and a load restores it from the save.
 *
 * A ROW IS ADDED ON THE FIRST Record FOR AN UNSEEN ID: the roster is the only caller and only calls for rows it was seeded
 * with, so an unknown id here is a roster row this object has not heard of yet, never a debug flight.
 * ENFORCED BY: AirportOps.Model.AirlineHistory.UnknownAirlineGrowsARowOnlyFromTheRoster
 */
UCLASS()
class AIRPORTOPS_API UAirlineHistory : public UObject, public IOpsPersistent
{
	GENERATED_BODY()

public:
	/** Days kept per airline, today included: the trend's seven points. The eighth closes and the oldest drops. */
	static constexpr int32 DaysKept = 7;

	/** A new game: every row gone, today is StartDay (the clock's). */
	void ResetForNewGame(int32 StartDay = 0);

	/** A load of a save with no "AirlineHistory" blob restores nothing and keeps the day number of the reset; with no rows
	 *  there is nothing to mislabel, so today becomes the loaded clock's. A no-op once any row exists. */
	void AdoptDayIfEmpty(int32 Today);

	/** One change, after the clamp: Delta is the real move, NewSatisfaction the value after it. Today's tally, today's close. */
	void Record(FName AirlineId, EAirlineSatisfactionCause Kind, double Delta, double NewSatisfaction);

	/**
	 * Midnight: stamp every row's open day with its standing's satisfaction, open the next day carrying it, trim to
	 * DaysKept. Standings with no row get one; a row with no standing (an airline since removed) carries its last value.
	 */
	void CloseDay(TArrayView<const FAirlineStanding> Standings);

	/** The row, or null for an airline nothing was ever recorded for. */
	const FAirlineDays* Find(FName AirlineId) const;

	/** Every kept day's tallies added together, by cause, in the order causes were first seen. */
	TArray<FAirlineCauseTally> SummedTallies(FName AirlineId) const;

	/**
	 * The kept days as a line: the OLDEST kept day's OPENING value, then each day's close (today's running value last), oldest
	 * first - one point more than there are days. Empty for an unknown id.
	 *
	 * OPENING, NOT ITS CLOSE, so the line spans exactly what SummedTallies sums: last - first == the summed deltas. Starting at the
	 * oldest close left that day's moves in the tally and out of the line ("late off stand -10%" beside a flat line). The opening is
	 * DERIVED (close minus the day's tallied deltas) rather than stored: exact, because Record tallies the CLAMPED delta, and it costs
	 * no saved field.
	 * ENFORCED BY: AirportMgr.Airlines.Detail.TrendSpansTheTally
	 */
	TArray<double> Trend(FName AirlineId) const;

	int32 GetCurrentDay() const { return CurrentDay; }

	// --- IOpsPersistent ---------------------------------------------------------------
	virtual FName SaveBlobName() const override { return TEXT("AirlineHistory"); }
	virtual UObject& AsPersistentObject() override { return *this; }
	virtual void OnBeforeRestore() override { ResetForNewGame(); }

private:
	/** Saved: the open day's number. Without it a mid-day load would reopen under the wrong label. */
	UPROPERTY() int32 CurrentDay = 0;
	UPROPERTY() TArray<FAirlineDays> Airlines;

	FAirlineDays& RowFor(FName AirlineId, double OpeningSatisfaction);
	/** The row's open day, made if the row's last day is not Today (a row born mid-week, or a missed close). */
	static FAirlineDay& OpenDayOf(FAirlineDays& Row, int32 Today, double CarrySatisfaction);
	static void Trim(FAirlineDays& Row);
};
