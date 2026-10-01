#include "LandChoices.h"

#include "Content/AirsideSettings.h"
#include "Entities/AircraftType.h"
#include "Model/ArrivalPlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadNetwork.h"

#define LOCTEXT_NAMESPACE "LandChoices"

TArray<UAircraftType*> LandChoices::EveryMeshedType()
{
	// THE ONE SCAN (#432) - this had its own registry walk, which copied the test helper's rule by hand and skipped the
	// synchronous search the envelope's scan knew it needed.
	return UAirsideSettings::EveryAircraftType(/*bMeshedOnly*/ true);
}

TArray<FLandChoice> LandChoices::Build(const TArray<UAircraftType*>& Types,
	TFunctionRef<FArrivalQuote(const FAirframe&)> Quote)
{
	TArray<FLandChoice> Out;
	Out.Reserve(Types.Num());
	for (UAircraftType* Type : Types)
	{
		if (Type == nullptr)
		{
			continue;
		}
		FLandChoice Choice;
		Choice.Type = Type;
		Choice.Label = FText::Format(LOCTEXT("Row", "{0} · {1}"),
			FText::FromName(Type->Code), Type->DisplayName);

		// THE MODEL'S VERDICT, RENDERED (#432) - never a second opinion about admission. This judged the nearest runway
		// alone (NearestRunwayThreshold, then CheckArrival) until #432, which #412 had made stale: the planner lands on
		// whichever runway takes the arrival, so the panel greyed an A380 the long far runway would take, judged a
		// departures-only runway the planner never lands on, and lit rows no exit or route could serve.
		const FArrivalQuote Answer = Quote(Type->Airframe());
		Choice.bAdmitted = Answer.IsAccepted();
		Choice.Refusal = Answer.IsAccepted() ? FString() : Answer.Sentence;
		Choice.Why = Answer.Why;
		Out.Add(MoveTemp(Choice));
	}

	// LETTER, THEN NAME - small to large, the order a player scanning for "what fits my
	// strip" reads in. Compared as strings: FName's operator< is not alphabetical.
	Out.Sort([](const FLandChoice& A, const FLandChoice& B)
	{
		const FString LetterA = A.Type->Code.ToString();
		const FString LetterB = B.Type->Code.ToString();
		if (LetterA != LetterB)
		{
			return LetterA < LetterB;
		}
		return A.Type->DisplayName.ToString() < B.Type->DisplayName.ToString();
	});
	return Out;
}

int32 LandChoices::RequoteForOccupancy(TArray<FLandChoice>& Rows, TFunctionRef<FArrivalQuote(const FAirframe&)> Quote)
{
	int32 Quoted = 0;
	for (FLandChoice& Row : Rows)
	{
		// THE PLANNER'S OWN TEST of "only a build clears it" - the offer generator and the FlightCannotLand alert ask it too, so
		// the three cannot disagree about which refusals an occupancy change can lift.
		if (Row.Type == nullptr || ArrivalPlanner::IsPermanentRefusal(Row.Why))
		{
			continue;
		}
		const FArrivalQuote Answer = Quote(Row.Type->Airframe());
		Row.bAdmitted = Answer.IsAccepted();
		Row.Refusal = Answer.IsAccepted() ? FString() : Answer.Sentence;
		Row.Why = Answer.Why;
		++Quoted;
	}
	return Quoted;
}

FLandChoicesKey LandChoices::KeyFor(const URoadNetwork* Network, const UGroundTraffic* Traffic, const FVector2D& Near,
	bool bAdmits, bool bQuotes)
{
	FLandChoicesKey Key;
	Key.Network = Network;
	Key.bAdmits = bAdmits;
	Key.bQuotes = bQuotes;
	Key.OccupancyRevision = Traffic != nullptr ? Traffic->OccupancyRevision() : 0;
	if (Network == nullptr)
	{
		return Key;
	}
	Key.EditRevision = Network->GetEditRevision();
	Key.GuidelineRevision = Network->GetGuidelineRevision();
	// THE PLANNER'S OWN ORDER, asked of the planner - not a runway chosen by proximity here (Check-Architecture rule 28).
	Key.FirstRunway = ArrivalPlanner::FirstLandingRunway(*Network, Near).Index;
	return Key;
}

#undef LOCTEXT_NAMESPACE
