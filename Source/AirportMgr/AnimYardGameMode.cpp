#include "AnimYardGameMode.h"

#include "AnimYardController.h"
#include "AnimYardHUD.h"

AAnimYardGameMode::AAnimYardGameMode()
{
	PlayerControllerClass = AAnimYardController::StaticClass();
	HUDClass = AAnimYardHUD::StaticClass();

	// See the header: the camera actor is the view target, so there is nothing for a pawn to do
	// but take input away from the controller.
	DefaultPawnClass = nullptr;
}
