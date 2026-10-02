#include "Model/RoadNetwork.h"

#include "AirsideLog.h"
#include "Model/TaxiwayStrip.h"
#include "Solve/TaxiwayLetters.h"

// TAXIWAY NAMES - THE TABLE (spec docs/superpowers/specs/2026-10-02-taxiway-naming-design.md): URoadNetwork's members
// that read or keep URoadNetwork::Taxiways - lookups, display names, the letters and connector numbers it issues,
// retirement, and the load backfill's count (EnsureTaxiwayNames, whose naming is NormaliseTaxiways'). None writes
// FRoadSegment::TaxiwayId: those stay in RoadNetworkTaxiways.cpp, rule 103's single writer, with the passes that judge
// geometry. Split out of it 2026-10-02 (PR #524) when it reached rule 77's 800-line Model budget.
// ENFORCED BY: Check-Architecture rule 103 (a TaxiwayId write here fails the lint)

const FTaxiway* URoadNetwork::GetTaxiway(int32 TaxiwayId) const
{
	return Taxiways.IsValidIndex(TaxiwayId) && Taxiways[TaxiwayId].bAlive ? &Taxiways[TaxiwayId] : nullptr;
}

FTaxiway* URoadNetwork::FindTaxiwayMutable(int32 TaxiwayId)
{
	return Taxiways.IsValidIndex(TaxiwayId) && Taxiways[TaxiwayId].bAlive ? &Taxiways[TaxiwayId] : nullptr;
}

int32 URoadNetwork::TaxiwayOf(FRoadSegmentId Segment) const
{
	const FRoadSegment* Found = GetSegment(Segment);
	return Found != nullptr && GetTaxiway(Found->TaxiwayId) != nullptr ? Found->TaxiwayId : INDEX_NONE;
}

FString URoadNetwork::TaxiwayDisplayName(int32 TaxiwayId) const
{
	const FTaxiway* Taxiway = GetTaxiway(TaxiwayId);
	if (Taxiway == nullptr)
	{
		return FString();
	}
	if (!Taxiway->Name.IsEmpty())
	{
		return Taxiway->Name;
	}
	// ONE LEVEL: a connector's parent is always lettered - AssignTaxiway parents a connector on a ROOT, and only a
	// taxiway with no connectors may become one (RejudgeTaxiway) - so this never recurses.
	const FTaxiway* Parent = Taxiway->IsConnector() ? GetTaxiway(Taxiway->ParentId) : nullptr;
	return Parent != nullptr && !Parent->Name.IsEmpty() ? Parent->Name + FString::FromInt(Taxiway->ConnectorNumber) : FString();
}

bool URoadNetwork::IsTaxiwayNameTaken(const FString& Name, int32 Except) const
{
	// A LINEAR SCAN per question: under 200 taxiways ever minted on a 40-stand airport (2026-10-02 estimate), and asked
	// only while naming or renaming - a click, never a frame.
	for (const FTaxiway& Each : Taxiways)
	{
		if (Each.bAlive && Each.Id != Except && TaxiwayDisplayName(Each.Id).Equals(Name, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

bool URoadNetwork::HasTaxiwayConnectors(int32 TaxiwayId) const
{
	return TaxiwayConnectorCount(TaxiwayId) > 0;
}

int32 URoadNetwork::TaxiwayConnectorCount(int32 TaxiwayId) const
{
	int32 Count = 0;
	for (const FTaxiway& Each : Taxiways)
	{
		Count += Each.bAlive && Each.ParentId == TaxiwayId && TaxiwayId != INDEX_NONE ? 1 : 0;
	}
	return Count;
}

FString URoadNetwork::NextFreeTaxiwayLetter(int32 Except) const
{
	// A LETTER IS FREE ONLY WHEN NOTHING DISPLAYS IT (spec): an empty parent whose connectors survive is still alive, so
	// its letter stays taken and a reused "A" can never mint a second "A1".
	for (int32 Index = 0;; ++Index)
	{
		const FString Letter = TaxiwayLetters::LetterAt(Index);
		if (!IsTaxiwayNameTaken(Letter, Except))
		{
			return Letter;
		}
	}
}

int32 URoadNetwork::IssueConnectorNumber(int32 ParentId)
{
	FTaxiway* Parent = FindTaxiwayMutable(ParentId);
	if (Parent == nullptr)
	{
		return 0;
	}
	// NEVER REUSED while the parent lives (NextConnectorNumber only advances), and never a number a player's override
	// already shows ("K7" renamed onto a connector of B would collide with K's seventh).
	const FString Root = TaxiwayDisplayName(ParentId);
	int32 Number = FMath::Max(Parent->NextConnectorNumber, 1);
	while (IsTaxiwayNameTaken(Root + FString::FromInt(Number), INDEX_NONE))
	{
		++Number;
	}
	Parent->NextConnectorNumber = Number + 1;
	return Number;
}

int32 URoadNetwork::MintTaxiway(int32 ParentId)
{
	FTaxiway Fresh;
	Fresh.Id = Taxiways.Num();
	Fresh.bAlive = true;
	if (GetTaxiway(ParentId) != nullptr)
	{
		Fresh.ParentId = ParentId;
		Fresh.ConnectorNumber = IssueConnectorNumber(ParentId);
	}
	else
	{
		Fresh.Name = NextFreeTaxiwayLetter(INDEX_NONE);
	}
	Taxiways.Add(MoveTemp(Fresh));
	return Taxiways.Last().Id;
}

FString URoadNetwork::JunctionName(FRoadNodeId Node) const
{
	TArray<FString> Names;
	if (const FRoadNode* At = GetNode(Node))
	{
		for (const FRoadSegmentId& Each : At->Incident)
		{
			const FString Name = TaxiwayDisplayName(TaxiwayOf(Each));
			if (!Name.IsEmpty())
			{
				Names.AddUnique(Name);
			}
		}
	}
	Names.Sort();
	return FString::Join(Names, TEXT("/"));
}

int32 URoadNetwork::RetireEmptyTaxiways()
{
	TArray<int32> Held;
	Held.Init(0, Taxiways.Num());
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const int32 Owner = TaxiwayOf(SegmentIdAt(Index));
		if (Owner != INDEX_NONE)
		{
			++Held[Owner];
		}
	}
	// UNTIL NOTHING MOVES: retiring the last connector of an empty parent frees the parent in the next round.
	int32 Retired = 0;
	for (bool bChanged = true; bChanged;)
	{
		bChanged = false;
		for (FTaxiway& Each : Taxiways)
		{
			if (!Each.bAlive || Held[Each.Id] > 0 || HasTaxiwayConnectors(Each.Id))
			{
				continue;
			}
			UE_LOG(LogAirside, Log, TEXT("TaxiwayNames: %s retired - no segment and no connector left"),
				*TaxiwayDisplayName(Each.Id));
			Each.bAlive = false;
			++Retired;
			bChanged = true;
		}
	}
	return Retired;
}

bool FRoadNetworkTestAccess::RenameTaxiwayForTest(int32 TaxiwayId, const FString& Name)
{
	FTaxiway* Taxiway = Network.FindTaxiwayMutable(TaxiwayId);
	if (Taxiway == nullptr)
	{
		return false;
	}
	Taxiway->Name = Name;
	Taxiway->bPlayerNamed = true;
	return true;
}

int32 URoadNetwork::EnsureTaxiwayNames(const FTaxiwayNamingRules& Rules)
{
	int32 Unnamed = 0;
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = SegmentIdAt(Index);
		Unnamed += Id.IsSet() && TaxiwayStrip::HasStrip(*this, Id) && TaxiwayOf(Id) == INDEX_NONE ? 1 : 0;
	}
	if (Unnamed == 0)
	{
		return 0;
	}
	const int32 Before = Taxiways.Num();
	NormaliseTaxiways(Rules);
	int32 Lettered = 0;
	int32 Connectors = 0;
	for (int32 Index = Before; Index < Taxiways.Num(); ++Index)
	{
		if (Taxiways[Index].bAlive)
		{
			(Taxiways[Index].IsConnector() ? Connectors : Lettered) += 1;
		}
	}
	UE_LOG(LogAirside, Log, TEXT("TaxiwayNames: backfilled %d taxiway(s), %d connector(s)"), Lettered, Connectors);
	return Lettered + Connectors;
}
