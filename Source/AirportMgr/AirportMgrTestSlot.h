#pragma once

#include "CoreMinimal.h"
#include "Kismet/GameplayStatics.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A SAVE SLOT THAT DIES WITH THE TEST, however the test ends (RAII) - the game tests' twin of AirportOpsTests' OpsSaveTest::FScopedSlot, which
 * lives in that test module's private header and cannot be reached from here. A SaveToSlot in a test writes a real file under Saved/SaveGames,
 * and a test that does not take it back leaves state its NEXT run did not make (AirportMgrTest_BarBalanceLoad and AirportMgrTest_LoadRetires sat
 * there for weeks, #462).
 *
 * DELETED AT CONSTRUCTION TOO, not only at the end: a crashed or killed run never reaches a destructor, so the first thing the next run must
 * see is no slot at all. THE SAME DELETE IS WHY THE NAME IS CHECKED: an FScopedSlot on "QuickSave" would wipe the player's real quicksave
 * (ARoadBuildController::QuickSave) when a test began.
 *
 * CONVERTS TO FString so it drops in where the bare `const FString Slot` was: SaveToSlot and LoadFromSlot take const FString&.
 * ENFORCED BY: Check-Architecture rule 102 (ops-test-slots-scoped) - an "AirportMgrTest_..." slot literal that is not the argument of an
 * FScopedSlot fails the lint, and so does an FScopedSlot whose name does not start with AirportOpsTest_ or AirportMgrTest_.
 */
namespace AirportMgrTest
{
	struct FScopedSlot
	{
		FString Name;

		explicit FScopedSlot(const TCHAR* InName) : Name(InName) { UGameplayStatics::DeleteGameInSlot(Name, 0); }
		~FScopedSlot() { UGameplayStatics::DeleteGameInSlot(Name, 0); }
		FScopedSlot(const FScopedSlot&) = delete;
		FScopedSlot& operator=(const FScopedSlot&) = delete;

		operator const FString&() const { return Name; }
	};
}

#endif
