#include "Model/RoadNetwork.h"

#include "Algo/Reverse.h"
#include "AirsideLog.h"
#include "Model/TaxiwayStrip.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"
#include "Solve/TaxiwayLetters.h"

// TAXIWAY NAMES (spec docs/superpowers/specs/2026-10-02-taxiway-naming-design.md): URoadNetwork's members for them, in a
// file of their own. RoadNetwork.cpp is held to its line figure by Check-Architecture rule 77, and rule 103 holds every
// write of FRoadSegment::TaxiwayId to THIS file - which a shared file could not be scoped to.

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

void URoadNetwork::WriteTaxiwayId(FRoadSegmentId Segment, int32 TaxiwayId)
{
	if (FRoadSegment* Found = GetSegmentMutable(Segment))
	{
		Found->TaxiwayId = TaxiwayId;
	}
}

void FRoadNetworkTestAccess::ClearTaxiwayNamesForTest()
{
	for (FRoadSegment& Segment : Network.Segments)
	{
		Segment.TaxiwayId = INDEX_NONE;
	}
	Network.Taxiways.Reset();
}
