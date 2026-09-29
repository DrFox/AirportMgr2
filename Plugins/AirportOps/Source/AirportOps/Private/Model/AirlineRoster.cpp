#include "Model/AirlineRoster.h"
#include "AirportOpsLog.h"
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

void UAirlineRoster::OnFlightAirborne(const FFlightAirborneEvent& Event)
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
		Apply(*Row, Tuning.OnTimeBonus, TEXT("on time"));
		return;
	}
	// PER TEN GAME MINUTES, CAPPED: a flight two hours late is not twelve times worse to an airline
	// than one ten minutes late - it has already missed its slot either way.
	const double Penalty = FMath::Min(Tuning.LatePenaltyCap, Tuning.LatePenaltyPerTenMinutes * (Event.LateBySeconds / 600.0));
	Apply(*Row, -Penalty, FString::Printf(TEXT("late departure (%d min)"), FMath::CeilToInt(Event.LateBySeconds / 60.0)));
}

void UAirlineRoster::OnOfferExpired(const FOfferExpiredEvent& Event)
{
	FAirlineStanding* Row = FindMutable(Event.AirlineId);
	if (Row == nullptr)
	{
		return;
	}
	// IGNORED COSTS MORE THAN NEVER-ACCEPTABLE: the first is the player not answering an offer they
	// could have taken; the second is an airport with no stand free for it the whole window, which
	// the airline still minds, but less.
	if (Event.Reason == ELapseReason::NeverAcceptable)
	{
		Apply(*Row, -Tuning.NeverAcceptablePenalty, TEXT("offer never acceptable"));
	}
	else
	{
		Apply(*Row, -Tuning.IgnoredPenalty, TEXT("offer ignored"));
	}
}

void UAirlineRoster::OnOfferDeclined(const FOfferDeclinedEvent& Event)
{
	UE_LOG(LogAirportOps, Verbose, TEXT("Airline '%s': flight %d declined - no effect on satisfaction"),
		*Event.AirlineId.ToString(), Event.FlightId);
}

void UAirlineRoster::OnDayEnded(const FDayEndedEvent& Event)
{
	for (FAirlineStanding& Row : Standings)
	{
		const double Drift = (Tuning.Start - Row.Satisfaction) * FMath::Clamp(Tuning.DailyDriftFraction, 0.0, 1.0);
		Apply(Row, Drift, TEXT("a day's forgiveness"));
	}
}

void UAirlineRoster::Apply(FAirlineStanding& Standing, double Delta, const FString& Cause)
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
	Standing.Recent.Add({ Moved, Cause });
	if (Standing.Recent.Num() > RecentCap)
	{
		Standing.Recent.RemoveAt(0, Standing.Recent.Num() - RecentCap);
	}
	UE_LOG(LogAirportOps, Log, TEXT("Airline %s: satisfaction %.2f -> %.2f (%s)"),
		*Standing.AirlineId.ToString(), Old, Standing.Satisfaction, *Cause);
	if (Bus != nullptr)
	{
		Bus->Publish(FAirlineSatisfactionEvent{ Standing.AirlineId, Old, Standing.Satisfaction, Cause });
	}
}
