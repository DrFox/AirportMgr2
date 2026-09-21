#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "AnimYardGameMode.generated.h"

/**
 * What M_ModelYard plays as: the bench's controller and its readout, and no pawn.
 *
 * A C++ GAME MODE AND NOT A BLUEPRINT, unlike BP_RoadBuildGameMode. This one holds three class
 * pointers and no graph, and a Blueprint would add the failure this project already knows by
 * name - a stale BP caching its C++ parent's layout, which needs recompiling and resaving
 * before a changed class takes. A level's World Settings can point at a C++ class directly.
 *
 * NO DEFAULT PAWN. UBuildCameraComponent spawns an ACameraActor and makes it the view target;
 * a pawn as well would be an invisible body standing in the yard, taking the input the bench
 * wants and fighting the cursor for it.
 */
UCLASS()
class AIRPORTMGR_API AAnimYardGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AAnimYardGameMode();
};
