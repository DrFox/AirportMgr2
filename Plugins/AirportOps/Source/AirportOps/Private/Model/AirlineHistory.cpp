#include "Model/AirlineHistory.h"

void UAirlineHistory::ResetForNewGame(int32 StartDay)
{
	Airlines.Reset();
	CurrentDay = StartDay;
}

void UAirlineHistory::AdoptDayIfEmpty(int32 Today)
{
	if (Airlines.IsEmpty())
	{
		CurrentDay = Today;
	}
}

FAirlineDays& UAirlineHistory::RowFor(FName AirlineId, double OpeningSatisfaction)
{
	for (FAirlineDays& Row : Airlines)
	{
		if (Row.AirlineId == AirlineId)
		{
			return Row;
		}
	}
	FAirlineDays& Row = Airlines.AddDefaulted_GetRef();
	Row.AirlineId = AirlineId;
	FAirlineDay& Today = Row.Days.AddDefaulted_GetRef();
	Today.Day = CurrentDay;
	Today.CloseSatisfaction = OpeningSatisfaction;
	return Row;
}

FAirlineDay& UAirlineHistory::OpenDayOf(FAirlineDays& Row, int32 Today, double CarrySatisfaction)
{
	if (Row.Days.IsEmpty() || Row.Days.Last().Day != Today)
	{
		FAirlineDay& Fresh = Row.Days.AddDefaulted_GetRef();
		Fresh.Day = Today;
		Fresh.CloseSatisfaction = CarrySatisfaction;
		Trim(Row);
	}
	return Row.Days.Last();
}

void UAirlineHistory::Trim(FAirlineDays& Row)
{
	if (Row.Days.Num() > DaysKept)
	{
		Row.Days.RemoveAt(0, Row.Days.Num() - DaysKept);
	}
}

void UAirlineHistory::Record(FName AirlineId, EAirlineSatisfactionCause Kind, double Delta, double NewSatisfaction)
{
	// A NEW ROW OPENS AT THE VALUE BEFORE THIS CHANGE, so a day's first point is where the day began, not where it ended.
	const double Before = NewSatisfaction - Delta;
	FAirlineDays& Row = RowFor(AirlineId, Before);
	FAirlineDay& Today = OpenDayOf(Row, CurrentDay, Before);
	Today.CloseSatisfaction = NewSatisfaction;
	FAirlineCauseTally* Tally = Today.Tallies.FindByPredicate([Kind](const FAirlineCauseTally& T) { return T.Kind == Kind; });
	if (Tally == nullptr)
	{
		Tally = &Today.Tallies.AddDefaulted_GetRef();
		Tally->Kind = Kind;
	}
	++Tally->Count;
	Tally->SumDelta += Delta;
}

void UAirlineHistory::CloseDay(TArrayView<const FAirlineStanding> Standings)
{
	const int32 Closing = CurrentDay;
	// ROWS FIRST, UNDER THE CLOSING DAY: RowFor stamps a new row's first day with CurrentDay, so after the increment an airline
	// with no Record today (zero drift records nothing) came out as [C+1, C, C+1] - not monotonic, today twice.
	// ENFORCED BY: AirportOps.Model.AirlineHistory.QuietAirlineGetsTwoIncreasingDays
	for (const FAirlineStanding& Standing : Standings)
	{
		RowFor(Standing.AirlineId, Standing.Satisfaction);
	}
	++CurrentDay;
	for (FAirlineDays& Row : Airlines)
	{
		const FAirlineStanding* Standing = Standings.FindByPredicate([&Row](const FAirlineStanding& S) { return S.AirlineId == Row.AirlineId; });
		const double Carry = Standing != nullptr ? Standing->Satisfaction : (Row.Days.IsEmpty() ? 0.0 : Row.Days.Last().CloseSatisfaction);
		// A row whose open day is not the one closing is brought up to it first, so the stamp lands on a day that exists.
		OpenDayOf(Row, Closing, Carry).CloseSatisfaction = Carry;
		FAirlineDay& Next = Row.Days.AddDefaulted_GetRef();
		Next.Day = CurrentDay;
		Next.CloseSatisfaction = Carry;
		Trim(Row);
	}
}

const FAirlineDays* UAirlineHistory::Find(FName AirlineId) const
{
	return Airlines.FindByPredicate([AirlineId](const FAirlineDays& Row) { return Row.AirlineId == AirlineId; });
}

TArray<FAirlineCauseTally> UAirlineHistory::SummedTallies(FName AirlineId) const
{
	TArray<FAirlineCauseTally> Out;
	const FAirlineDays* Row = Find(AirlineId);
	if (Row == nullptr)
	{
		return Out;
	}
	for (const FAirlineDay& Day : Row->Days)
	{
		for (const FAirlineCauseTally& T : Day.Tallies)
		{
			FAirlineCauseTally* Sum = Out.FindByPredicate([&T](const FAirlineCauseTally& S) { return S.Kind == T.Kind; });
			if (Sum == nullptr)
			{
				Sum = &Out.AddDefaulted_GetRef();
				Sum->Kind = T.Kind;
			}
			Sum->Count += T.Count;
			Sum->SumDelta += T.SumDelta;
		}
	}
	return Out;
}

TArray<double> UAirlineHistory::Trend(FName AirlineId) const
{
	TArray<double> Out;
	const FAirlineDays* Row = Find(AirlineId);
	if (Row == nullptr || Row->Days.IsEmpty())
	{
		return Out;
	}
	double Opening = Row->Days[0].CloseSatisfaction;
	for (const FAirlineCauseTally& T : Row->Days[0].Tallies)
	{
		Opening -= T.SumDelta;
	}
	Out.Reserve(Row->Days.Num() + 1);
	Out.Add(Opening);
	for (const FAirlineDay& Day : Row->Days)
	{
		Out.Add(Day.CloseSatisfaction);
	}
	return Out;
}
