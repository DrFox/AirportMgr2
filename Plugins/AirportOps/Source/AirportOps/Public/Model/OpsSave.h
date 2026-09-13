#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "OpsSave.generated.h"

class USimClock;
class URoadNetwork;
class UFlightBoard;
class UFuelService;

/**
 * A model object OpsSave::Capture/Restore treats generically: one blob in
 * FOpsSnapshot::Blobs keyed by its own name, rather than a named FOpsSnapshot field and a
 * positional Capture/Restore parameter apiece (issue #105 item 8). URoadNetwork does NOT
 * implement this despite being persisted the same way: it lives in Airside, which may not
 * depend on AirportOps (Check-Architecture.ps1), so OpsSave keys its blob by a literal name
 * instead - the one exception, documented at that call site.
 *
 * A plain abstract class, not a UINTERFACE, for the same reason IRoadEditTarget is one - see
 * that header's comment: nothing in Blueprint needs to see this seam.
 */
class AIRPORTOPS_API IOpsPersistent
{
public:
	virtual ~IOpsPersistent() = default;

	/** The key this object's blob lives under in FOpsSnapshot::Blobs. Stable across versions:
	 *  it is also where OpsSave::Restore's v3-and-earlier shim copies legacy bytes to. */
	virtual FName SaveBlobName() const = 0;

	/**
	 * Called on EVERY registered persistent object before ANY blob is deserialised into any
	 * of them - even one with no blob in this snapshot at all (an old save, or one from
	 * before this object existed). UFuelService::OnBeforeRestore clears Demands and GoingHome
	 * for exactly that reason: UOpsRuntime::LoadFromSlot always clears agents before calling
	 * OpsSave::Restore, so every TruckId/AircraftId either map holds is about to go stale
	 * regardless of what this snapshot contains - the leak this issue traced was GoingHome
	 * surviving a load because nothing ever reset it at all, fuel having no blob and no hook.
	 */
	virtual void OnBeforeRestore() {}

	/**
	 * This object as a UObject, for OpsSave's (de)serialisation. A plain interface is not
	 * itself a UObject and Cast<> cannot reach one without the UINTERFACE reflection this
	 * class deliberately does not carry (see the class comment) - implementers hand back
	 * their own `*this` typed as the UObject half of the multiple inheritance.
	 */
	virtual UObject& AsPersistentObject() = 0;
};

/**
 * One blob, wrapped: UHT refuses a TArray as a TMap value directly ("TArray<uint8> can not
 * be used as a value in a TMap"), so the map holds one of these instead of the bytes bare.
 */
USTRUCT()
struct AIRPORTOPS_API FOpsBlob
{
	GENERATED_BODY()

	UPROPERTY() TArray<uint8> Bytes;
};

/**
 * Everything a save holds, as opaque byte blobs per model object.
 *
 * ONE BLOB PER OBJECT rather than one archive for all of them, so a save from before a
 * new system existed still loads: a missing blob means "that system starts fresh", and
 * a blob for an object the build no longer has is skipped. Version is for the day a blob's
 * own layout changes incompatibly, which tagged-property serialisation mostly absorbs.
 */
USTRUCT()
struct AIRPORTOPS_API FOpsSnapshot
{
	GENERATED_BODY()

	/**
	 * 4: blobs move from three named fields (Clock/Network/Flights) to one map keyed by
	 * IOpsPersistent::SaveBlobName() (issue #105 item 8) - so UFuelService (added here to fix
	 * the GoingHome leak IOpsPersistent::OnBeforeRestore's comment describes) is one more
	 * blob rather than a fourth named field and a fifth positional Capture/Restore parameter.
	 *
	 * Clock/Network/Flights below are the v3-AND-EARLIER SHIM: a save from before Blobs
	 * existed still has bytes under those OLD tagged-property names - UE's tagged
	 * serialisation matches by property name, so removing them would silently drop an old
	 * save's bytes on load - and OpsSave::Restore copies them into Blobs under the matching
	 * key before the generic restore pass runs. A v4 Capture never writes to them.
	 *
	 * 3 since UFlight::ApproachFocus (issue #96, and where the version last bumped - #113).
	 * Before it, every flight shared the board's one ApproachFocus; a v1 or v2 blob's flights
	 * therefore have no per-flight focus at all, and OpsSave::Restore recreates one via
	 * UFlightBoard::AimUnaimedFlightsAtBoardFocus rather than leave every restored flight
	 * aimed at the world origin.
	 *
	 * 2 since flights. A v1 snapshot is a game from before the flight board and loads with
	 * an empty inbox rather than being refused - an old save must still open.
	 */
	UPROPERTY() int32 Version = 4;

	UPROPERTY() TMap<FName, FOpsBlob> Blobs;

	UPROPERTY() TArray<uint8> Clock;
	UPROPERTY() TArray<uint8> Network;

	/**
	 * The flight board's non-Transient UPROPERTYs.
	 *
	 * The board's CLOCK HANDLES are not among them, deliberately: USimClock does not save its
	 * queue either, so a restored handle would name a callback that no longer exists and
	 * cancelling it would take somebody else's. UFlight::ArrivesAt is the saved truth, and
	 * UFlightBoard::RearmSchedules rebuilds the handles from it.
	 */
	UPROPERTY() TArray<uint8> Flights;
};

/** The USaveGame wrapper UGameplayStatics needs for a slot on disk. Holds a snapshot and nothing else. */
UCLASS()
class AIRPORTOPS_API UOpsSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY() FOpsSnapshot Snapshot;
};

/**
 * Save and load of the model.
 *
 * THE RULE: a model object's non-Transient UPROPERTYs ARE its saved state. Spec §2.3 said
 * "tag every field SaveGame"; that was written before reading FProperty::ShouldSerializeValue
 * (Property.cpp:1052), which with ArIsSaveGame set skips EVERY untagged property including
 * the members of nested structs - so honouring it would mean tagging every field of every
 * Airside struct, and the first forgotten tag would silently drop a field from every save.
 * Not setting ArIsSaveGame and marking the exceptions Transient inverts the default to the
 * safe side: forgetting a Transient saves one field too many, which is visible; forgetting
 * a SaveGame loses one, which is not.
 *
 * Object references (profiles, definitions) go through FObjectAndNameAsStringProxyArchive
 * as path names and are re-found by path on load, which is what content assets support and
 * transient objects do not. Views are never saved; Present/ rebuilds from the model.
 */
namespace OpsSave
{
	AIRPORTOPS_API void SerializeObject(UObject& Object, TArray<uint8>& OutBytes);
	AIRPORTOPS_API void DeserializeObject(UObject& Object, const TArray<uint8>& Bytes);

	/** One IOpsPersistent's blob, keyed by its own SaveBlobName(). */
	AIRPORTOPS_API void CaptureBlob(IOpsPersistent& Persistent, FOpsSnapshot& Out);

	/** OnBeforeRestore ALWAYS runs, whether or not In has a blob for Persistent - see the
	 *  interface method's own comment for why that matters. */
	AIRPORTOPS_API void RestoreBlob(const FOpsSnapshot& In, IOpsPersistent& Persistent);

	AIRPORTOPS_API void Capture(const USimClock& Clock, const URoadNetwork& Network,
		const UFlightBoard& Board, const UFuelService& Fuel, FOpsSnapshot& Out);

	/** False only when a blob is present and fails to deserialise. Missing blobs leave the target untouched. */
	AIRPORTOPS_API bool Restore(const FOpsSnapshot& In, USimClock& Clock, URoadNetwork& Network,
		UFlightBoard& Board, UFuelService& Fuel);

	AIRPORTOPS_API bool WriteSlot(const FString& SlotName, const FOpsSnapshot& Snapshot);
	AIRPORTOPS_API bool ReadSlot(const FString& SlotName, FOpsSnapshot& Out);
}
