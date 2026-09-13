#include "Model/OpsSave.h"

#include "Model/FlightBoard.h"
#include "Model/FuelService.h"
#include "AirportOpsLog.h"
#include "Kismet/GameplayStatics.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"

namespace
{
	// URoadNetwork cannot implement IOpsPersistent (Airside may not depend on AirportOps) so
	// its blob is keyed by a literal name instead - see IOpsPersistent's class comment.
	const FName NetworkBlobName(TEXT("Network"));
}

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

void OpsSave::CaptureBlob(IOpsPersistent& Persistent, FOpsSnapshot& Out)
{
	SerializeObject(Persistent.AsPersistentObject(), Out.Blobs.FindOrAdd(Persistent.SaveBlobName()).Bytes);
}

void OpsSave::RestoreBlob(const FOpsSnapshot& In, IOpsPersistent& Persistent)
{
	// UNCONDITIONAL: even a snapshot with no blob at all for Persistent (an old save, or one
	// from before this object existed) must not leave stale runtime state behind - see the
	// interface method's own comment for the leak this fixes.
	Persistent.OnBeforeRestore();
	if (const FOpsBlob* Blob = In.Blobs.Find(Persistent.SaveBlobName()))
	{
		if (Blob->Bytes.Num() > 0)
		{
			DeserializeObject(Persistent.AsPersistentObject(), Blob->Bytes);
		}
	}
}

void OpsSave::Capture(const USimClock& Clock, const URoadNetwork& Network,
	const UFlightBoard& Board, const UFuelService& Fuel, FOpsSnapshot& Out)
{
	Out.Blobs.Reset();
	Out.Clock.Reset();
	Out.Network.Reset();
	Out.Flights.Reset();

	// Serialize is non-const on UObject; the archive is saving, so nothing is written to them.
	SerializeObject(const_cast<URoadNetwork&>(Network), Out.Blobs.FindOrAdd(NetworkBlobName).Bytes);
	CaptureBlob(const_cast<USimClock&>(Clock), Out);
	CaptureBlob(const_cast<UFlightBoard&>(Board), Out);
	CaptureBlob(const_cast<UFuelService&>(Fuel), Out);

	UE_LOG(LogAirportOps, Log, TEXT("Captured snapshot: %d blob(s)"), Out.Blobs.Num());
}

bool OpsSave::Restore(const FOpsSnapshot& In, USimClock& Clock, URoadNetwork& Network,
	UFlightBoard& Board, UFuelService& Fuel)
{
	FOpsSnapshot Shimmed = In;
	if (Shimmed.Version < 4)
	{
		// v3-AND-EARLIER SHIM: bytes under the old named fields, not yet in Blobs - see
		// FOpsSnapshot::Version's own comment. A field the old save never wrote (e.g. no
		// Flights blob at all, v1) stays absent from Blobs too, same as today.
		if (Shimmed.Clock.Num() > 0)   { Shimmed.Blobs.FindOrAdd(TEXT("Clock")).Bytes = Shimmed.Clock; }
		if (Shimmed.Network.Num() > 0) { Shimmed.Blobs.FindOrAdd(NetworkBlobName).Bytes = Shimmed.Network; }
		if (Shimmed.Flights.Num() > 0) { Shimmed.Blobs.FindOrAdd(TEXT("Flights")).Bytes = Shimmed.Flights; }
	}

	if (const FOpsBlob* NetworkBlob = Shimmed.Blobs.Find(NetworkBlobName))
	{
		if (NetworkBlob->Bytes.Num() > 0)
		{
			DeserializeObject(Network, NetworkBlob->Bytes);
		}
	}

	RestoreBlob(Shimmed, Clock);
	RestoreBlob(Shimmed, Fuel);

	const bool bHadFlights = Shimmed.Blobs.Contains(TEXT("Flights"));
	RestoreBlob(Shimmed, Board);
	if (bHadFlights && Shimmed.Version < 3)
	{
		// A blob from before UFlight::ApproachFocus (issue #96): every flight in it
		// shared the board's one focus, which DID deserialise (it is older than the
		// flights themselves) - so recreate the per-flight field from it rather than
		// leave each restored flight's new field at its default, the world origin.
		Board.AimUnaimedFlightsAtBoardFocus();
	}

	// A v1 snapshot has no Flights blob at all, and the branch above leaves the board alone -
	// which is the right answer: a game saved before the board existed had no flights.
	UE_LOG(LogAirportOps, Log,
		TEXT("Restored snapshot v%d: game time %.1f, %d nodes, %d live flight(s)"),
		In.Version, Clock.Now(), Network.GetNodes().Num(), Board.Live().Num());
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
