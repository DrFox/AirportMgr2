#include "Model/OpsSave.h"

#include "Model/FlightBoard.h"
#include "Model/JobBoard.h"
#include "AirportOpsLog.h"
#include "Kismet/GameplayStatics.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Profiles/RoadProfile.h"   // FSaveArchive asks a URoadProfile whether it is an actor's fallback
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"

namespace
{
	// URoadNetwork cannot implement IOpsPersistent (Airside may not depend on AirportOps) so
	// its blob is keyed by a literal name instead - see IOpsPersistent's class comment.
	const FName NetworkBlobName(TEXT("Network"));

	/**
	 * THE SAVE ARCHIVE, WRITING EVERY ACTOR FALLBACK PROFILE AS NONE (#459). ARoadNetworkActor::ResolveProfile's
	 * fallback (URoadProfile::bActorFallback) lives in the transient package: its path names nothing in another process,
	 * and in this one may name something else. Written as null, it loads as "no profile of its own", which
	 * URoadNetwork::ProfileFor reads as the default and the loading actor re-resolves (RepairLoadedNetwork): the stable
	 * id the issue asked for is "the default" itself. A LEVEL save has always written it so.
	 *
	 * BY THE MARKER, not by identity with the network's current DefaultProfile - roads the EDITOR laid name the editor
	 * actor's fallback, which a PIE copy keeps while its DefaultProfile becomes the PIE actor's (see the field) - and not
	 * "every transient object": a test's transient runway round-trips in the same process only because its path is
	 * written. EVERY BLOB goes through it, not only the network's: only the network holds a profile, and the rule is
	 * true of any reference to one. Every reference type the proxy archive serialises comes through
	 * operator<<(UObject*&) - FObjectPtr's too (FArchiveUObject::SerializeObjectPtr) - so this one override covers
	 * TObjectPtr members.
	 */
	class FSaveArchive : public FObjectAndNameAsStringProxyArchive
	{
	public:
		explicit FSaveArchive(FArchive& Inner)
			: FObjectAndNameAsStringProxyArchive(Inner, /*bInLoadIfFindFails*/ false)
		{
		}

		using FObjectAndNameAsStringProxyArchive::operator<<;
		virtual FArchive& operator<<(UObject*& Obj) override
		{
			if (const URoadProfile* Profile = Cast<URoadProfile>(Obj); Profile != nullptr && Profile->bActorFallback)
			{
				UObject* None = nullptr;
				return FObjectAndNameAsStringProxyArchive::operator<<(None);
			}
			return FObjectAndNameAsStringProxyArchive::operator<<(Obj);
		}
	};
}

void OpsSave::SerializeObject(UObject& Object, TArray<uint8>& OutBytes)
{
	OutBytes.Reset();
	FMemoryWriter Writer(OutBytes, /*bIsPersistent*/ true);
	// THE ONE SAVE ARCHIVE - see FSaveArchive: an actor's fallback road profile is written as none, whichever blob.
	FSaveArchive Ar(Writer);
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

void OpsSave::Capture(TArrayView<IOpsPersistent* const> Persistents,
	const URoadNetwork& Network, FOpsSnapshot& Out)
{
	Out.Blobs.Reset();
	Out.Clock.Reset();
	Out.Network.Reset();
	Out.Flights.Reset();

	// Serialize is non-const on UObject; the archive is saving, so nothing is written to them. THROUGH SerializeObject's
	// FSaveArchive like every blob, which writes an actor's fallback road profile as none (#459).
	// ENFORCED BY: AirportOps.Model.Save.FallbackProfileIsSavedAsTheDefault
	SerializeObject(const_cast<URoadNetwork&>(Network), Out.Blobs.FindOrAdd(NetworkBlobName).Bytes);

	for (IOpsPersistent* Persistent : Persistents)
	{
		// NULL IS SKIPPED RATHER THAN CHECKED AWAY at every call site: UOpsRuntime builds this
		// list from its own subobjects, and one that failed to construct should cost that
		// system's blob, not the whole save.
		if (Persistent != nullptr)
		{
			CaptureBlob(*Persistent, Out);
		}
	}

	UE_LOG(LogAirportOps, Log, TEXT("Captured snapshot: %d blob(s)"), Out.Blobs.Num());
}

bool OpsSave::Restore(const FOpsSnapshot& In, TArrayView<IOpsPersistent* const> Persistents,
	URoadNetwork& Network)
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

	// ONE UNIFORM PASS. Anything a particular system must do about an OLD snapshot is that
	// system's own OnAfterRestore - see IOpsPersistent. The board's pre-v3 ApproachFocus
	// migration used to be special-cased here, wrapped around the board's blob specifically
	// and forcing Restore to name the board as a parameter; it moved into the board's
	// override, and went altogether on 2026-09-30 (a pre-v6 blob restores no flights).
	for (IOpsPersistent* Persistent : Persistents)
	{
		if (Persistent != nullptr)
		{
			RestoreBlob(Shimmed, *Persistent);
			Persistent->OnAfterRestore(Shimmed.Version);
		}
	}

	// A v1 snapshot has no Flights blob at all, and the loop above restores no flights for it -
	// which is the right answer: a game saved before the board existed had no flights. The
	// board's OnBeforeRestore retires the REPLACED session's (#426 (b)); this loop used to leave
	// them in place, which only looked right when loading into an empty board.
	UE_LOG(LogAirportOps, Log,
		TEXT("Restored snapshot v%d: %d nodes, %d persistent object(s)"),
		In.Version, Network.GetNodes().Num(), Persistents.Num());
	return true;
}

bool OpsSave::WriteSlot(const FString& SlotName, const FOpsSnapshot& Snapshot)
{
	UOpsSaveGame* Save = Cast<UOpsSaveGame>(UGameplayStatics::CreateSaveGameObject(UOpsSaveGame::StaticClass()));
	// A SAVE THAT DID NOT HAPPEN IS A WARNING, both ways it can fail (#499 review): it used to return false silently here, and
	// log FAILED at Log below - so the one record of a lost save sat among the routine lines. UOpsEvents::NotifySaveSlot logs
	// what the player was told at Log, relying on this.
	if (Save == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Save to slot '%s': FAILED - no save game object could be created"), *SlotName);
		return false;
	}
	Save->Snapshot = Snapshot;
	const bool bOk = UGameplayStatics::SaveGameToSlot(Save, SlotName, 0);
	if (bOk)
	{
		UE_LOG(LogAirportOps, Log, TEXT("Save to slot '%s': ok"), *SlotName);
	}
	else
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Save to slot '%s': FAILED"), *SlotName);
	}
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
