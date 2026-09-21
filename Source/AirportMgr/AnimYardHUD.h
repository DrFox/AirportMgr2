#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "AnimYardHUD.generated.h"

/**
 * The bench's readout: what every rig is being told, which channel the caret is on, and what
 * the keys do.
 *
 * AN AHUD AND NOT UMG, which is a deliberate departure from everything else this game draws.
 * The reasons are specific to a debug bench: UMG scales by the shortest side, so a docked PIE
 * viewport renders it at about 56% and the figures would be judged at the wrong size; a widget
 * needs a Blueprint asset authored before the tool works at all, where this one compiles and
 * runs; and nothing here is a game surface anybody styles. UAirportMgrUISettings and the bar's
 * whole style stack are correctly not involved.
 *
 * IT READS AAnimYardController AND AAnimYard AND DECIDES NOTHING. Which channel the caret is
 * on is the controller's, what the rigs are doing is the yard's; this prints them.
 */
UCLASS()
class AIRPORTMGR_API AAnimYardHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;
};
