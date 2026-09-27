#include "LandChoices.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Entities/AircraftType.h"
#include "Model/RoadNetwork.h"
#include "Model/RunwayAdmission.h"

#define LOCTEXT_NAMESPACE "LandChoices"

TArray<UAircraftType*> LandChoices::EveryMeshedType()
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
		TEXT("AssetRegistry")).Get();

	TArray<FAssetData> Assets;
	Registry.GetAssetsByClass(UAircraftType::StaticClass()->GetClassPathName(), Assets);

	TArray<UAircraftType*> Out;
	for (const FAssetData& Data : Assets)
	{
		// Loaded here, once per open of the panel: eighteen small data assets on 2026-09-27,
		// already resident in any session that has dispatched an arrival.
		if (UAircraftType* Type = Cast<UAircraftType>(Data.GetAsset()))
		{
			if (!Type->Mesh.IsNull())
			{
				Out.Add(Type);
			}
		}
	}
	return Out;
}

namespace
{
	/**
	 * One short phrase for the row. TooShort is said in METRES, not RunwayAdmission::
	 * Describe's uu - "the runway is 60000 uu" is a sentence for the log, not for a player
	 * choosing an aeroplane. Every other refusal is Describe's own words.
	 */
	FString LandChoiceRefusal(const FRunwayAdmission& Admission)
	{
		if (Admission.Why == ERunwayRefusal::TooShort)
		{
			const double NeedM = Admission.FieldLength / 100.0;
			const double HaveM = Admission.RunwayLength / 100.0;
			return Admission.bForDeparture
				? FString::Printf(TEXT("could not take off again: needs %.0f m, longest runway %.0f m"), NeedM, HaveM)
				: FString::Printf(TEXT("too short: needs %.0f m, runway %.0f m"), NeedM, HaveM);
		}
		return RunwayAdmission::Describe(Admission);
	}
}

TArray<FLandChoice> LandChoices::Build(const URoadNetwork* Network, const FVector2D& Near,
	const TArray<UAircraftType*>& Types)
{
	// The runway ArrivalPlanner::Plan would pick from here - see the header for why only it.
	FRunwayEnd End;
	const bool bHasRunway = Network != nullptr && Network->NearestRunwayThreshold(Near, End);

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

		if (!bHasRunway)
		{
			Choice.Refusal = TEXT("no runway");
		}
		else
		{
			// CheckArrival, the planner's own question: it must be able to leave, too.
			const FRunwayAdmission Admission =
				RunwayAdmission::CheckArrival(*Network, End.Seed, Type->Airframe());
			Choice.bAdmitted = Admission.Why == ERunwayRefusal::None;
			if (!Choice.bAdmitted)
			{
				Choice.Refusal = LandChoiceRefusal(Admission);
			}
		}
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

#undef LOCTEXT_NAMESPACE
