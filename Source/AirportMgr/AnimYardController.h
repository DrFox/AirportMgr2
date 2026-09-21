#pragma once

#include "CoreMinimal.h"
#include "AnimYardMotion.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "AnimYardController.generated.h"

class AAnimYard;
class UBuildCameraComponent;

/** Everything a key can do on the bench. */
enum class EYardAction : uint8
{
	TogglePause,
	NextChannel,
	ScrubUp,
	ScrubDown,
	ToggleAirborne,
	ToggleSolo,
	Reset,

	/**
	 * THE CAMERA ACTIONS ARE IN THE SAME LIST as the bench ones, though they touch no rig.
	 * The list's job is that every key the bench answers to is written down exactly once and
	 * the on-screen help cannot omit one; a zoom bound outside it would be a key with no help
	 * line, which is the shape of the bug this table exists to prevent.
	 */
	ZoomIn,
	ZoomOut,
};

/** A key, what it does, and the line the HUD prints for it. */
struct FYardActionBinding
{
	FKey Key;
	EYardAction Action = EYardAction::TogglePause;

	/** What the HUD prints beside the key. Present tense, lower case, no full stop. */
	const TCHAR* Help = TEXT("");
};

/**
 * THE KEY TABLE, and the only place a bench key is written down.
 *
 * ONE LIST BECAUSE THREE THINGS MUST AGREE: what is bound, what the HUD offers, and what
 * AAnimYardController::Do knows how to run. This project has shipped a list that nothing read
 * three times - UEdMode::ToolCommandList, GetModeCommands(), ARoadBuildController::Tools - and
 * ARoadBuildController's own BuildActions() is the fix applied to the build driver. This is
 * the same fix on the bench, and AirportMgr.View.AnimYard.EveryActionHasAKey is what checks
 * the binding loop actually ran.
 */
AIRPORTMGR_API TArrayView<const FYardActionBinding> YardActions();

/**
 * The bench's driver: a camera to fly round the model yard with, and the keys that drive
 * every rig in it.
 *
 * SEPARATE FROM ARoadBuildController rather than a mode inside it. That class already carries
 * the whole build gesture stack - tools, snapping, the bar, the inspector - and none of it
 * means anything in a yard with no road network; inheriting it would bring a dependency on
 * ARoadNetworkActor that the yard has no actor to satisfy.
 *
 * THE CAMERA IS THE GAME'S OWN, though: UBuildCameraComponent, the same orbiting rig the
 * airport is built with, so the bench is flown the way the game is and there is no second
 * camera to tune. It needed one seam to get here - see UBuildCameraComponent::UpdateFreeView -
 * because it used to take an ARoadNetworkActor for a single double.
 */
UCLASS()
class AIRPORTMGR_API AAnimYardController : public APlayerController
{
	GENERATED_BODY()

public:
	AAnimYardController();

	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float DeltaTime) override;

	/**
	 * Run one action. THE ONE ENTRY POINT - the key handler resolves a key to an action and
	 * calls this, and so does anything else that ever wants to drive the bench.
	 */
	void Do(EYardAction Action);

	/**
	 * Point the opening view at the row of aircraft, from in front of their noses.
	 *
	 * SETS THE CAMERA'S LIMITS AND MUST RUN BEFORE CreateBuildCamera, because that is what
	 * calls FBuildCameraRig::Reset, and Reset is what reads StartFocus/StartYaw/StartDistance.
	 *
	 * WHY THIS EXISTS AT ALL: moving the level's PlayerStart does nothing here, which was
	 * reported from play and is correct rather than broken. AAnimYardGameMode sets
	 * DefaultPawnClass to null - the camera actor is the view target, and a pawn would only
	 * take input away - so nothing is ever spawned at the PlayerStart, and the rig's own reset
	 * put the focus on the world origin, which in this level is the empty strip BETWEEN the
	 * two rows.
	 */
	void AimAtTheAircraft();

	/** Which channel the caret is on, and therefore which one a scrub moves. */
	EYardChannel Caret() const { return CaretChannel; }

	/** The yard this controller drives, found in the level at BeginPlay. */
	AAnimYard* Yard() const { return CachedYard; }

	UBuildCameraComponent* Camera() const { return CameraComponent; }

	/**
	 * BYPASSES BeginPlay's level search - the same precedent as
	 * ARoadBuildController::SetTargetForTest, and for the same reason: a test world has no
	 * level for TActorIterator to find the yard in.
	 */
	void SetYardForTest(AAnimYard* InYard) { CachedYard = InYard; }

private:
	/**
	 * The orbiting view, held as a component exactly as ARoadBuildController holds it - see
	 * UBuildCameraComponent's header for why the rigs live there rather than on a controller.
	 */
	UPROPERTY() TObjectPtr<UBuildCameraComponent> CameraComponent;

	UPROPERTY() TObjectPtr<AAnimYard> CachedYard;

	/**
	 * Which channel a scrub moves. ON THE CONTROLLER AND NOT THE YARD, because it is a fact
	 * about who is looking rather than about what the models are doing - the yard would
	 * happily drive itself with nobody watching.
	 */
	EYardChannel CaretChannel = EYardChannel::GroundSpeed;

	/** A chord binding hands its handler no key; a plain one does. See ARoadBuildController. */
	void OnActionKey(FKey Key);

	/** How much of a step this frame's scrub is worth. Shift is the fine drag. */
	double ScrubScale() const;
};
