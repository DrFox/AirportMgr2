#include "AirlineViewModels.h"

#include "ArrivalViewModels.h"
#include "Model/AirlineDefinition.h"
#include "Model/AirlineHistory.h"
#include "Model/AirlineRoster.h"
#include "Model/ExhaustiveSwitch.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/OfferGenerator.h"
#include "Model/SimClock.h"
#include "OfferViewModels.h"
#include "Present/OpsRuntime.h"

namespace
{
	/** The catalog entry for AirlineId, or null - a standing whose airline left the catalog still lists, under its id. Prefixed for the unity build. */
	const UAirlineDefinition* AirlineVmDefinitionOf(TArrayView<const FAirlineOffers> Airlines, FName AirlineId)
	{
		for (const FAirlineOffers& Each : Airlines)
		{
			if (Each.Airline != nullptr && Each.Airline->GetFName() == AirlineId)
			{
				return Each.Airline;
			}
		}
		return nullptr;
	}

	/**
	 * The definition's DisplayName as it stands - what UOpsRuntime copies onto every offer candidate, and so onto every flight the
	 * inbox shows, so the panel and the inbox print one name - and the id only for a standing with no catalog entry. NO EMPTY-NAME
	 * FALLBACK: that would be a second naming rule beside the offers' (and Check-Architecture's display-name-fallback row keeps such
	 * rules to one per kind).
	 */
	FText AirlineVmNameOf(const UAirlineDefinition* Airline, FName AirlineId)
	{
		return Airline != nullptr ? Airline->DisplayName : FText::FromName(AirlineId);
	}

	/** The percentage the inbox row prints (UOfferViewModel::MoodKeyOf rounds the same way), so the two windows show one number. */
	int32 AirlineVmPercent(double Satisfaction)
	{
		return static_cast<int32>(FMath::RoundToInt(Satisfaction * 100.0));
	}

	FText AirlineVmOneDecimal(double Value)
	{
		FNumberFormattingOptions Options;
		Options.MinimumFractionalDigits = 1;
		Options.MaximumFractionalDigits = 1;
		return FText::AsNumber(Value, &Options);
	}
}

FAirlinePanelSources FAirlinePanelSources::From(const UOpsRuntime& Runtime)
{
	FAirlinePanelSources Out;
	Out.Roster = Runtime.GetAirlines();
	Out.History = Runtime.GetAirlineHistory();
	// THE GENERATOR'S OWN LIST (UOpsRuntime::GetAirlineOffers), so the panel cannot name an airline the generator does not tick.
	Out.Airlines = Runtime.GetAirlineOffers();
	Out.Generator = Runtime.GetOfferGenerator();
	Out.Board = Runtime.GetFlightBoard();
	Out.Clock = Runtime.GetClock();
	return Out;
}

TArray<FAirlineListRow> UAirlineListViewModel::BuildRows(const UOpsRuntime& Runtime) const
{
	return BuildRows(FAirlinePanelSources::From(Runtime));
}

TArray<FAirlineListRow> UAirlineListViewModel::BuildRows(const FAirlinePanelSources& Sources)
{
	TArray<FAirlineListRow> Rows;
	if (Sources.Roster == nullptr)
	{
		return Rows;
	}
	// ONE ROW PER STANDING, not per catalog entry: the roster is what has a satisfaction to show, and it is seeded from the catalog
	// (UOpsRuntime::SeedAirlines), so the two name the same airlines in a running game.
	for (const FAirlineStanding& Standing : Sources.Roster->GetStandings())
	{
		const UAirlineDefinition* Airline = AirlineVmDefinitionOf(Sources.Airlines, Standing.AirlineId);
		FAirlineListRow& Row = Rows.AddDefaulted_GetRef();
		Row.AirlineId = Standing.AirlineId;
		Row.Name = AirlineVmNameOf(Airline, Standing.AirlineId);
		Row.SatisfactionPct = AirlineVmPercent(Standing.Satisfaction);
		Row.Trend = TrendOf(Sources.History != nullptr ? Sources.History->Find(Standing.AirlineId) : nullptr);
		Row.bFloor = Airline != nullptr && Airline->bIsFloor;
	}
	// FLOOR FIRST, then by name: the floor airlines are the ones that keep the airport from going quiet, and the name is what the
	// player scans for. The roster's own order is seeding order, which means nothing to them.
	Rows.StableSort([](const FAirlineListRow& A, const FAirlineListRow& B)
	{
		if (A.bFloor != B.bFloor)
		{
			return A.bFloor;
		}
		return A.Name.CompareTo(B.Name) < 0;
	});
	return Rows;
}

EAirlineTrend UAirlineListViewModel::TrendOf(const FAirlineDays* Days)
{
	// TODAY'S RUNNING VALUE AGAINST YESTERDAY'S CLOSE, both from the history: the roster feeds it after the clamp (UAirlineHistory's
	// class comment), so the open day's value is the standing's. A row with one day has no yesterday - Flat, not a guess.
	if (Days == nullptr || Days->Days.Num() < 2)
	{
		return EAirlineTrend::Flat;
	}
	const double Change = Days->Days.Last().CloseSatisfaction - Days->Days.Last(1).CloseSatisfaction;
	if (FMath::Abs(Change) <= FlatWithin)
	{
		return EAirlineTrend::Flat;
	}
	return Change > 0.0 ? EAirlineTrend::Up : EAirlineTrend::Down;
}

FAirlineDetail UAirlineDetailViewModel::Build(const UOpsRuntime& Runtime, FName AirlineId, double Now) const
{
	return Build(FAirlinePanelSources::From(Runtime), AirlineId, Now);
}

FAirlineDetail UAirlineDetailViewModel::Build(const FAirlinePanelSources& Sources, FName AirlineId, double Now)
{
	FAirlineDetail Out;
	const UAirlineDefinition* Airline = AirlineVmDefinitionOf(Sources.Airlines, AirlineId);
	Out.Name = AirlineVmNameOf(Airline, AirlineId);
	const FAirlineStanding* Standing = Sources.Roster != nullptr ? Sources.Roster->Find(AirlineId) : nullptr;
	Out.SatisfactionPct = Standing != nullptr ? AirlineVmPercent(Standing->Satisfaction) : 0;

	// THE FLEET FIRST: the factor line counts its ticks. Empty before the generator's first admission check - "not judged yet", which
	// must not read as "nothing can come" (GetFleetAdmission's own comment).
	const TArray<FFleetAdmission> Verdicts = Sources.Generator != nullptr ? Sources.Generator->GetFleetAdmission(AirlineId) : TArray<FFleetAdmission>();
	Out.bJudged = Verdicts.Num() > 0;
	int32 Admitted = 0;
	for (const FFleetAdmission& Verdict : Verdicts)
	{
		FAirlineFleetRow& Row = Out.Fleet.AddDefaulted_GetRef();
		Row.TypeName = Verdict.TypeName;
		Row.bAdmitted = Verdict.bAdmitted;
		// THE PLAN'S OWN SENTENCE, figures and all - the one DescribeWhyNot and the cannot-come alert print.
		Row.Reason = Verdict.bAdmitted ? FText::GetEmpty() : FText::FromString(Verdict.Sentence);
		Admitted += Verdict.bAdmitted ? 1 : 0;
	}

	if (Sources.Generator != nullptr && Airline != nullptr)
	{
		if (Sources.Clock != nullptr)
		{
			// CurrentRate IS what TickMinute accrues (its ENFORCED BY), so "~N offers/h now" is the number actually running.
			Out.RateLine = FText::Format(NSLOCTEXT("AirportMgr", "AirlineRateLine", "~{0} offers/h now"),
				AirlineVmOneDecimal(Sources.Generator->CurrentRate(*Airline, *Sources.Clock)));
		}
		const FText Mood = AirlineVmOneDecimal(Sources.Generator->MoodFactor(*Airline));
		Out.FactorLine = Out.bJudged
			? FText::Format(NSLOCTEXT("AirportMgr", "AirlineFactorLine", "mood x{0} · {1} of {2} types can come"),
				Mood, FText::AsNumber(Admitted), FText::AsNumber(Verdicts.Num()))
			: FText::Format(NSLOCTEXT("AirportMgr", "AirlineFactorLineUnjudged", "mood x{0} · fleet not judged yet"), Mood);
	}

	// THE WEEK. History is a CLOSED day: before the first midnight the pane says "no history yet" and lists today's partial tally,
	// and a single point would draw a sparkline of nothing.
	const FAirlineDays* Days = Sources.History != nullptr ? Sources.History->Find(AirlineId) : nullptr;
	Out.bHasHistory = Days != nullptr && Days->Days.Num() >= 2;
	if (Out.bHasHistory)
	{
		for (const FAirlineDay& Day : Days->Days)
		{
			Out.Trend.Add(Day.CloseSatisfaction);
		}
	}
	if (Sources.History != nullptr)
	{
		TArray<FAirlineCauseTally> Summed = Sources.History->SummedTallies(AirlineId);
		// LARGEST |DELTA| FIRST - what moved the airline most is what the player should read first - and the DAILY DRIFT LAST,
		// whatever its size: it is the airline forgiving, not something the player did. Stable, so ties keep first-seen order.
		Summed.StableSort([](const FAirlineCauseTally& A, const FAirlineCauseTally& B)
		{
			const bool bADrift = A.Kind == EAirlineSatisfactionCause::DailyDrift;
			const bool bBDrift = B.Kind == EAirlineSatisfactionCause::DailyDrift;
			if (bADrift != bBDrift)
			{
				return bBDrift;
			}
			return FMath::Abs(A.SumDelta) > FMath::Abs(B.SumDelta);
		});
		for (const FAirlineCauseTally& Tally : Summed)
		{
			FAirlineTallyRow& Row = Out.Tallies.AddDefaulted_GetRef();
			Row.Label = CauseLabel(Tally.Kind);
			Row.Count = Tally.Count;
			Row.SumDelta = Tally.SumDelta;
			Row.DeltaText = DescribeDelta(Tally.SumDelta);
		}
	}

	if (Sources.Board != nullptr)
	{
		// OFFERING NOW, read-only: the inbox is the one place that acts on an offer. The countdown in the inbox's own words.
		for (const UFlight* Offer : Sources.Board->Offers())
		{
			if (Offer == nullptr || Offer->AirlineId != AirlineId)
			{
				continue;
			}
			FAirlineOfferRow& Row = Out.Offers.AddDefaulted_GetRef();
			Row.Callsign = FText::FromString(Offer->Callsign);
			Row.TypeName = Offer->TypeName;
			Row.Countdown = UOfferViewModel::DescribeSecondsLeft(UOfferViewModel::SecondsLeftOf(*Offer));
		}
		// FLIGHTS WITH YOU: UFlightBoard::Live, the accept-to-airborne set the arrivals list reads - not a phase range of this file's
		// own (Check-Architecture's flight-phase-groupings rule keeps those in Flight.h). Phase and contract in the arrivals row's words.
		for (const UFlight* Flight : Sources.Board->Live())
		{
			if (Flight == nullptr || Flight->AirlineId != AirlineId)
			{
				continue;
			}
			FAirlineFlightRow& Row = Out.Flights.AddDefaulted_GetRef();
			Row.Callsign = FText::FromString(Flight->Callsign);
			Row.TypeName = Flight->TypeName;
			Row.Phase = UArrivalRowViewModel::DescribeStatus(*Flight, Now);
			Row.Contract = UArrivalRowViewModel::DescribeDetail(*Flight, Now, Row.bLate);
		}
	}
	return Out;
}

// EVERY CAUSE BY NAME, NO default: a new cause is a BUILD ERROR here rather than a blank tally row (spec 2026-10-02-airlines-panel
// section 1.1).
// ENFORCED BY: C4062 as an error, AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN; AirportMgr.Airlines.Detail.TallyOrderAndText (every cause has its own label)
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
FText UAirlineDetailViewModel::CauseLabel(EAirlineSatisfactionCause Cause)
{
	switch (Cause)
	{
	case EAirlineSatisfactionCause::OnTime:                 return NSLOCTEXT("AirportMgr", "AirlineCauseOnTime", "on time");
	case EAirlineSatisfactionCause::LateOffStand:           return NSLOCTEXT("AirportMgr", "AirlineCauseLate", "late off stand");
	case EAirlineSatisfactionCause::OfferIgnored:           return NSLOCTEXT("AirportMgr", "AirlineCauseIgnored", "offers ignored");
	case EAirlineSatisfactionCause::OfferNeverAcceptable:   return NSLOCTEXT("AirportMgr", "AirlineCauseNeverAcceptable", "offers we had no stand for");
	case EAirlineSatisfactionCause::LeftShortOfFuel:        return NSLOCTEXT("AirportMgr", "AirlineCauseShortOfFuel", "left short of fuel");
	case EAirlineSatisfactionCause::CancelledAirportClosed: return NSLOCTEXT("AirportMgr", "AirlineCauseClosed", "cancelled: airport closed");
	case EAirlineSatisfactionCause::CancelledByPlayer:      return NSLOCTEXT("AirportMgr", "AirlineCauseCancelled", "cancelled by you");
	case EAirlineSatisfactionCause::DailyDrift:             return NSLOCTEXT("AirportMgr", "AirlineCauseDrift", "daily forgiveness");
	}
	return FText::GetEmpty();
}
AIRSIDE_EXHAUSTIVE_SWITCH_END

FText UAirlineDetailViewModel::DescribeDelta(double SumDelta)
{
	// WHOLE PERCENTAGE POINTS, as the list's percentage prints. A sum that rounds to nothing reads "0%", never "-0%" or "+0%": the
	// sign belongs to a move the player can see.
	const int32 Points = static_cast<int32>(FMath::RoundToInt(SumDelta * 100.0));
	if (Points == 0)
	{
		return NSLOCTEXT("AirportMgr", "AirlineDeltaZero", "0%");
	}
	return FText::Format(Points > 0
			? NSLOCTEXT("AirportMgr", "AirlineDeltaUp", "+{0}%")
			: NSLOCTEXT("AirportMgr", "AirlineDeltaDown", "-{0}%"),
		FText::AsNumber(FMath::Abs(Points)));
}
