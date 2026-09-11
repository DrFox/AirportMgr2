#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "OpsSave.generated.h"

class USimClock;
class URoadNetwork;
class UFlightBoard;

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
	 * 2 since flights. A v1 snapshot is a game from before the flight board and loads with
	 * an empty inbox rather than being refused - an old save must still open.
	 */
	UPROPERTY() int32 Version = 2;
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

	AIRPORTOPS_API void Capture(const USimClock& Clock, const URoadNetwork& Network,
		const UFlightBoard& Board, FOpsSnapshot& Out);

	/** False only when a blob is present and fails to deserialise. Missing blobs leave the target untouched. */
	AIRPORTOPS_API bool Restore(const FOpsSnapshot& In, USimClock& Clock, URoadNetwork& Network,
		UFlightBoard& Board);

	AIRPORTOPS_API bool WriteSlot(const FString& SlotName, const FOpsSnapshot& Snapshot);
	AIRPORTOPS_API bool ReadSlot(const FString& SlotName, FOpsSnapshot& Out);
}
