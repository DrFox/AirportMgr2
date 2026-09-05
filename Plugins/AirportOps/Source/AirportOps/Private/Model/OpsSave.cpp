#include "Model/OpsSave.h"
#include "AirportOpsLog.h"
#include "Kismet/GameplayStatics.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"

void OpsSave::SerializeObject(UObject& Object, TArray<uint8>& OutBytes)
{
	OutBytes.Reset();
	FMemoryWriter Writer(OutBytes, /*bIsPersistent*/ true);
	FObjectAndNameAsStringProxyArchive Ar(Writer, /*bInLoadIfFindFails*/ false);
	// ArIsSaveGame deliberately left false - see the namespace comment in the header.
	Object.Serialize(Ar);
}

void OpsSave::DeserializeObject(UObject& Object, const TArray<uint8>& Bytes)
{
	FMemoryReader Reader(Bytes, /*bIsPersistent*/ true);
	// bLoadIfFindFails: an asset referenced by path that is not yet in memory gets loaded,
	// which is the case for a profile or a stand definition on a cold start.
	FObjectAndNameAsStringProxyArchive Ar(Reader, /*bInLoadIfFindFails*/ true);
	Object.Serialize(Ar);
}

void OpsSave::Capture(const USimClock& Clock, const URoadNetwork& Network, FOpsSnapshot& Out)
{
	// Serialize is non-const on UObject; the archive is saving, so nothing is written to them.
	SerializeObject(const_cast<USimClock&>(Clock), Out.Clock);
	SerializeObject(const_cast<URoadNetwork&>(Network), Out.Network);
	UE_LOG(LogAirportOps, Log, TEXT("Captured snapshot: clock %d bytes, network %d bytes"), Out.Clock.Num(), Out.Network.Num());
}

bool OpsSave::Restore(const FOpsSnapshot& In, USimClock& Clock, URoadNetwork& Network)
{
	if (In.Clock.Num() > 0)
	{
		DeserializeObject(Clock, In.Clock);
	}
	if (In.Network.Num() > 0)
	{
		DeserializeObject(Network, In.Network);
	}
	UE_LOG(LogAirportOps, Log, TEXT("Restored snapshot v%d: game time %.1f, %d nodes"),
		In.Version, Clock.Now(), Network.GetNodes().Num());
	return true;
}

bool OpsSave::WriteSlot(const FString& SlotName, const FOpsSnapshot& Snapshot)
{
	UOpsSaveGame* Save = Cast<UOpsSaveGame>(UGameplayStatics::CreateSaveGameObject(UOpsSaveGame::StaticClass()));
	if (Save == nullptr)
	{
		return false;
	}
	Save->Snapshot = Snapshot;
	const bool bOk = UGameplayStatics::SaveGameToSlot(Save, SlotName, 0);
	UE_LOG(LogAirportOps, Log, TEXT("Save to slot '%s': %s"), *SlotName, bOk ? TEXT("ok") : TEXT("FAILED"));
	return bOk;
}

bool OpsSave::ReadSlot(const FString& SlotName, FOpsSnapshot& Out)
{
	if (!UGameplayStatics::DoesSaveGameExist(SlotName, 0))
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Load from slot '%s': no such slot"), *SlotName);
		return false;
	}
	UOpsSaveGame* Save = Cast<UOpsSaveGame>(UGameplayStatics::LoadGameFromSlot(SlotName, 0));
	if (Save == nullptr)
	{
		UE_LOG(LogAirportOps, Error, TEXT("Load from slot '%s': not an AirportOps save"), *SlotName);
		return false;
	}
	Out = Save->Snapshot;
	return true;
}
