#include "Model/AirlineRoster.h"
#include "AirportOpsLog.h"
#include "Model/AirlineHistory.h"
#include "Model/ExhaustiveSwitch.h"
#include "Model/FlightBoard.h"
#include "Model/OpsEventBus.h"

void UAirlineRoster::Ensure(FName AirlineId)
{
	if (AirlineId.IsNone() || FindMutable(AirlineId) != nullptr)
	{
		return;
	}
	FAirlineStanding& Row = Standings.AddDefaulted_GetRef();
	Row.AirlineId = AirlineId;
	Row.Satisfaction = FMath::Clamp(Tuning.Start, 0.0, 1.0);
}

const FAirlineStanding* UAirlineRoster::Find(FName AirlineId) const
{
	return Standings.FindByPredicate([AirlineId](const FAirlineStanding& Row) { return Row.AirlineId == AirlineId; });
}

FAirlineStanding* UAirlineRoster::FindMutable(FName AirlineId)
{
	return Standings.FindByPredicate([AirlineId](const FAirlineStanding& Row) { return Row.AirlineId == AirlineId; });
}

double UAirlineRoster::RateMultiplier(FName AirlineId, bool bIsFloor) const
{
	const FAirlineStanding* Row = Find(AirlineId);
	if (Row == nullptr)
	{
		return 1.0;
	}
	const double Multiplier = FMath::Lerp(Tuning.MinRateMultiplier, Tuning.MaxRateMultiplier, Row->Satisfaction);
	return bIsFloor ? FMath::Max(Multiplier, 1.0) : Multiplier;
}

void UAirlineRoster::OnFlightOffBlocks(const FFlightOffBlocksEvent& Event)
{
	FAirlineStanding* Row = FindMutable(Event.AirlineId);
	if (Row == nullptr)
	{
		UE_LOG(LogAirportOps, Verbose, TEXT("Airline '%s' (flight %d) has no standing - debug flight or removed airline"),
			*Event.AirlineId.ToString(), Event.FlightId);
		return;
	}
	if (Event.LateBySeconds <= 0.0)
	{
		Apply(*Row, Tuning.OnTimeBonus, EAirlineSatisfactionCause::OnTime, TEXT("on time"));
		return;
	}
	// PER TEN GAME MINUTES, CAPPED: a flight two hours late is not twelve times worse to an airline
	// than one ten minutes late - it has already missed its slot either way.
	//
	// LATE OFF STAND, NOT LATE DEPARTURE (#398): the lateness is time on stand over the contract, measured at off-blocks. Time
	// spent holding for the runway, taxiing in and taxiing out is NOT scored - out of scope for the ruling of 2026-10-02, and a
	// candidate for its own penalty later; if one comes, it is a second line here with its own cause, not a change to this one.
	const double Penalty = FMath::Min(Tuning.LatePenaltyCap, Tuning.LatePenaltyPerTenMinutes * (Event.LateBySeconds / 600.0));
	Apply(*Row, -Penalty, EAirlineSatisfactionCause::LateOffStand, FString::Printf(TEXT("late off stand (%d min)"), FMath::CeilToInt(Event.LateBySeconds / 60.0)));
}

void UAirlineRoster::OnOfferExpired(const FOfferExpiredEvent& Event)
{
	FAirlineStanding* Row = FindMutable(Event.AirlineId);
	if (Row == nullptr)
	{
		UE_LOG(LogAirportOps, Verbose, TEXT("Airline '%s' (offer %d) has no standing - debug flight or removed airline"),
			*Event.AirlineId.ToString(), Event.FlightId);
		return;
	}
	// THE FLOOR AIRLINE'S LAPSES NEVER COST THE PLAYER (spec 2026-09-28 rulings 7 and 8, UFlight::
	// bFloorAirline): the flying club is always there and its offers are small change - letting one
	// lapse is not an insult to anyone.
	if (Event.bFloorAirline)
	{
		UE_LOG(LogAirportOps, Verbose, TEXT("Airline %s: floor-airline offer %d lapsed - no effect"),
			*Event.AirlineId.ToString(), Event.FlightId);
		return;
	}
	// IGNORED COSTS MORE THAN NEVER-ACCEPTABLE: the first is the player not answering an offer they
	// could have taken; the second is an airport with no stand free for it the whole window, which
	// the airline still minds, but less.
	if (Event.Reason == ELapseReason::NeverAcceptable)
	{
		Apply(*Row, -Tuning.NeverAcceptablePenalty, EAirlineSatisfactionCause::OfferNeverAcceptable, TEXT("offer never acceptable"));
	}
	else
	{
		Apply(*Row, -Tuning.IgnoredPenalty, EAirlineSatisfactionCause::OfferIgnored, TEXT("offer ignored"));
	}
}

void UAirlineRoster::OnOfferDeclined(const FOfferDeclinedEvent& Event)
{
	UE_LOG(LogAirportOps, Verbose, TEXT("Airline '%s': flight %d declined - no effect on satisfaction"),
		*Event.AirlineId.ToString(), Event.FlightId);
}

void UAirlineRoster::OnTurnaroundEnded(const FTurnaroundEndedEvent& Event, const UFlightBoard* Flights)
{
	// WANTED NOTHING IS FUELLED: an aircraft that asked for no fuel was not let down, whatever the outcome
	// field says. And FUELLED SCORES 0 - the on-time bonus already rewards the turnaround that went right.
	if (Event.Outcome == EFuelOutcome::Fuelled || Event.Wanted <= 0.0)
	{
		return;
	}
	const UFlight* Flight = Flights != nullptr ? Flights->FlightForAgent(Event.AircraftAgentId) : nullptr;
	if (Flight == nullptr)
	{
		UE_LOG(LogAirportOps, Verbose, TEXT("Turnaround of agent %d ended short of fuel, but it flies no flight - no airline to tell"),
			Event.AircraftAgentId);
		return;
	}
	FAirlineStanding* Row = FindMutable(Flight->AirlineId);
	if (Row == nullptr)
	{
		UE_LOG(LogAirportOps, Verbose, TEXT("Airline '%s' (flight %d) has no standing - debug flight or removed airline"),
			*Flight->AirlineId.ToString(), Flight->Id);
		return;
	}
	// PROPORTIONAL, ONE KNOB (user ruling 2026-09-29): the fraction NOT delivered, so a truck that got most of
	// the way there costs less than one that never came, and unfuelled is simply the whole of it.
	const double Short = 1.0 - FMath::Clamp(Event.Delivered / Event.Wanted, 0.0, 1.0);
	Apply(*Row, -Tuning.ShortfallPenalty * Short, EAirlineSatisfactionCause::LeftShortOfFuel,
		Event.Outcome == EFuelOutcome::PartFuelled ? TEXT("left part-fuelled") : TEXT("left unfuelled"));
}

// A MISSING REASON IN THE SWITCH BELOW IS A BUILD ERROR (#442) - see ExhaustiveSwitch.h: a new ECancelReason must say what it costs.
// ENFORCED BY: C4062 as an error, AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
void UAirlineRoster::OnFlightCancelled(const FFlightCancelledEvent& Event)
{
	// ONLY THE PLAYER'S CHOICE COSTS (user ruling 2026-09-29): losing the last runway is a loophole the user
	// accepted, and a despawn is the player rescuing a stuck aeroplane, not letting an airline down. The player's
	// CLOSURE costs, and since #442 so does the player's CANCEL of a flight that had not arrived - at the SAME rate, the
	// closure's per-flight ClosureCancelPenalty: each lost the airline the flight to the player's own hand.
	const TCHAR* Cause = nullptr;
	EAirlineSatisfactionCause Kind = EAirlineSatisfactionCause::CancelledAirportClosed;
	switch (Event.Reason)
	{
	case ECancelReason::AirportClosed:
		Cause = TEXT("cancelled: airport closed");
		Kind = EAirlineSatisfactionCause::CancelledAirportClosed;
		break;
	case ECancelReason::PlayerCancelled:
		Cause = TEXT("cancelled by the player");
		Kind = EAirlineSatisfactionCause::CancelledByPlayer;
		break;
	case ECancelReason::NoRunway:
	case ECancelReason::Unstuck:
		break;
	}
	if (Cause == nullptr)
	{
		UE_LOG(LogAirportOps, Verbose, TEXT("Airline '%s': flight %d cancelled (%s) - no effect on satisfaction"),
			*Event.AirlineId.ToString(), Event.FlightId, *UEnum::GetValueAsString(Event.Reason));
		return;
	}
	FAirlineStanding* Row = FindMutable(Event.AirlineId);
	if (Row == nullptr)
	{
		UE_LOG(LogAirportOps, Verbose, TEXT("Airline '%s' (flight %d) has no standing - debug flight or removed airline"),
			*Event.AirlineId.ToString(), Event.FlightId);
		return;
	}
	Apply(*Row, -Tuning.ClosureCancelPenalty, Kind, Cause);
}
AIRSIDE_EXHAUSTIVE_SWITCH_END

void UAirlineRoster::OnDayEnded(const FDayEndedEvent& Event)
{
	for (FAirlineStanding& Row : Standings)
	{
		// SNAPPED HOME within half a percent: a fraction of the gap each day never reaches zero, and an
		// airline 0.001 from its start would otherwise announce "0.50 -> 0.50" every day for months.
		const double Gap = Tuning.Start - Row.Satisfaction;
		const double Drift = FMath::Abs(Gap) < DriftSnap ? Gap : Gap * FMath::Clamp(Tuning.DailyDriftFraction, 0.0, 1.0);
		// NOT REMEMBERED as the row's cause: a day's forgiveness is not something the player did, and
		// letting it replace "late off stand" would hide the one reason they could act on.
		Apply(Row, Drift, EAirlineSatisfactionCause::DailyDrift, TEXT("a day's forgiveness"), /*bRemember=*/false);
	}
	// AFTER THE LOOP, never before: the drift is recorded into the day it forgives, and only then is that day closed.
	// ENFORCED BY: AirportOps.Model.AirlineHistory.DriftBelongsToTheDayItCloses
	if (History != nullptr)
	{
		History->CloseDay(Standings);
	}
}

void UAirlineRoster::Apply(FAirlineStanding& Standing, double Delta, EAirlineSatisfactionCause Kind, const FString& Cause, bool bRemember)
{
	const double Old = Standing.Satisfaction;
	Standing.Satisfaction = FMath::Clamp(Old + Delta, 0.0, 1.0);
	const double Moved = Standing.Satisfaction - Old;
	// NOTHING MOVED, NOTHING SAID: a penalty at 0.0 or a drift at the resting point is not a change,
	// and an event for it would put "0% ▼" on the inbox row for no reason.
	// The clamped value is KEPT either way, so floating-point dust above 0.0 settles on 0.0.
	if (FMath::IsNearlyZero(Moved, 1e-9))
	{
		return;
	}
	// THE HISTORY HEARS EVERY CHANGE, drift included: bRemember only decides what the inbox row says, and the history's
	// tally is of what moved the number. Moved is the clamped delta, so it matches the published event.
	if (History != nullptr)
	{
		History->Record(Standing.AirlineId, Kind, Moved, Standing.Satisfaction);
	}
	if (bRemember)
	{
		Standing.Recent.Add({ Moved, Cause, Kind });
		if (Standing.Recent.Num() > RecentCap)
		{
			Standing.Recent.RemoveAt(0, Standing.Recent.Num() - RecentCap);
		}
	}
	// VERBOSE: the bus's Presentation "Log" subscriber prints every change at Log already (the line the
	// PIE check greps), and two lines per change is noise.
	UE_LOG(LogAirportOps, Verbose, TEXT("Airline %s: satisfaction %.2f -> %.2f (%s)"),
		*Standing.AirlineId.ToString(), Old, Standing.Satisfaction, *Cause);
	if (Bus != nullptr)
	{
		Bus->Publish(FAirlineSatisfactionEvent{ Standing.AirlineId, Old, Standing.Satisfaction, Cause, Kind });
	}
}
