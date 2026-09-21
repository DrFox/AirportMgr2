#pragma once

#include "CoreMinimal.h"
#include "AnimYardMotion.h"
#include "GameFramework/Actor.h"
#include "Model/Airframe.h"
#include "AnimYard.generated.h"

class ARoadAgentActor;
class ASkeletalMeshActor;
class USkeletalMesh;

/**
 * What the bench needs to know to drive one rig: which Animation Blueprint, and the gear
 * figures that turn the normalised gear channel into this airframe's own cycle.
 *
 * A PLAIN STRUCT AND NOT A USTRUCT, because it is an out-parameter that lives for the length
 * of one resolve call - nothing stores one. What IS stored is FYardSubject below, which is
 * reflected because it holds the agent and the source actor and those must not be collected.
 */
struct FYardRig
{
	/** Built on UAirsideAgentAnim, which is what computes every angle it applies. */
	UClass* AnimClass = nullptr;

	/** Unset for a fixed-gear airframe and for every ground vehicle - see FGearPerformance::IsSet. */
	FGearPerformance Gear;

	/**
	 * A ground vehicle rather than an aircraft, which decides WHICH dressing call is made:
	 * ARoadAgentActor keeps SetAirframe and SetVehicleAirframe apart deliberately, and its
	 * header gives the reason (an overload pair split only by mesh type resolves ambiguously
	 * on a nullptr literal).
	 */
	bool bIsVehicle = false;

	/** The vehicle fallback box, uu, full size and X forward. Ignored unless bIsVehicle. */
	FVector BoxSizeUu = FVector(600.0, 250.0, 300.0);
};

/**
 * One model in the yard, and the agent standing on its mark - or no agent, if nothing could
 * drive it.
 */
USTRUCT()
struct FYardSubject
{
	GENERATED_BODY()

	/** What build_model_yard.py placed. Hidden once an agent takes its mark. */
	UPROPERTY() TObjectPtr<ASkeletalMeshActor> Source;

	/**
	 * Null when the catalogue had no Animation Blueprint for this mesh.
	 *
	 * NULL IS A REPORTABLE STATE, not a failure to record: three of the four ground vehicles
	 * have no ABP at all today, and the bench says so on screen rather than leaving a still
	 * model looking like a broken one.
	 */
	UPROPERTY() TObjectPtr<ARoadAgentActor> Agent;

	/** This airframe's own gear figures. Unset for everything in the yard but plane4. */
	UPROPERTY() FGearPerformance Gear;

	/**
	 * The mark this model stands on: road-plane XY, taken off the placed actor.
	 *
	 * CARRIED PER SUBJECT BECAUSE ARoadAgentActor::SetMotion PLACES THE ACTOR. It reads
	 * FAgentMotion::Position every call, so pushing the bench's motion in unaltered would move
	 * all ten agents to the world origin and stack them there - the yard's layout would survive
	 * only until the first frame. FYardMotion still sets no pose at all (see its
	 * StaysOnItsMark test); the mark is the YARD's business, and this is where it lives.
	 */
	UPROPERTY() FVector2D Mark = FVector2D::ZeroVector;

	/** The mark's height, uu. Read off the placed actor rather than assumed to be the floor. */
	UPROPERTY() double MarkZ = 0.0;

	/** Radians, yaw from +X - the placed actor's own facing, kept so the row stays as authored. */
	UPROPERTY() double Heading = 0.0;
};

/**
 * The model yard's animation bench: one placed actor that makes every rigged model in
 * M_ModelYard move, and lets you drive any channel by hand.
 *
 * WHY THIS EXISTS AS AN ACTOR AND NOT A MODE. UAirsideAgentAnim reads its values from the
 * ARoadAgentActor that owns it - NativeUpdateAnimation casts GetOwningActor and returns early
 * on anything else - so nothing animates in the yard until real agents stand there. This
 * spawns them and pushes an FAgentMotion in, which is precisely what ARoadNetworkActor does at
 * the airport. The bench therefore exercises the SHIPPING path: a defect seen here is a defect
 * the game has.
 *
 * IT ADOPTS RATHER THAN LAYS OUT. build_model_yard.py already measures each model's bounds to
 * decide where it stands, and re-deriving that here would be a second authority on the yard's
 * layout. This walks the level, finds what the script placed, and takes each model's own
 * transform.
 *
 * IN THE GAME MODULE AND NOT THE PLUGIN, beside ARoadBuildController, because it is a runtime
 * DRIVER - it decides what the models are told to do. The Airside plugin holds the model and
 * the view; a debug bench that invents motions is neither.
 */
UCLASS()
class AIRPORTMGR_API AAnimYard : public AActor
{
	GENERATED_BODY()

public:
	AAnimYard();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/**
	 * How a placed mesh is matched to the rig that drives it. Returns false for a mesh nothing
	 * can drive, which is a subject the bench reports rather than one it skips.
	 *
	 * A SEAM RATHER THAN A DIRECT CALL, so the adoption above can be tested without the
	 * content catalogue - and so the catalogue can be replaced the day the yard wants to show
	 * something the fleet assets do not describe. Defaulted in the constructor to
	 * AnimYardCatalogue::FindRigFor, which is the production answer.
	 */
	TFunction<bool(USkeletalMesh*, FYardRig&)> RigResolver;

	/**
	 * Find every model the yard script placed and give the rigged ones an agent. Returns how
	 * many were newly DRIVEN, which is not the same as how many were found.
	 *
	 * IDEMPOTENT. BeginPlay calls it; anything that calls it again gets nothing new rather than
	 * a second agent stacked on every mark.
	 */
	int32 AdoptSubjects();

	/** Every model found, driven or not. */
	const TArray<FYardSubject>& Subjects() const { return SubjectList; }

	/** The agent standing on this model's mark, or null if nothing drives it. */
	ARoadAgentActor* AgentFor(const AActor* Source) const;

	/**
	 * What to call a model on screen and in the log.
	 *
	 * THE EDITOR LABEL, which is the name build_model_yard.py actually set - "Plane5 (King Air
	 * 350i)", "Tug1 (Goldhofer D 620)". GetName() gives SkeletalMeshActor_13, which is what the
	 * first run of the bench printed for the three undriven vehicles: a line that names three
	 * models and identifies none of them, in a yard where being undriven is usually correct and
	 * the whole point of the line is saying WHICH.
	 *
	 * GUARDED, because GetActorLabel is editor-only. A packaged build has no labels and falls
	 * back to the object name, which is the best that exists there.
	 */
	static FString NameOf(const AActor* Actor);

	/** What every rig is being told to do. */
	const FYardMotion& GetMotion() const { return Motion; }
	FYardMotion& EditMotion() { return Motion; }

	/**
	 * Drive only this model; park the rest. Null drives them all again.
	 *
	 * PARKED AND NOT FROZEN. The un-soloed models go to FYardMotion's resting state rather
	 * than holding whatever pose they were in when the key was pressed: a row frozen mid
	 * steering-sweep is a row of wrong answers to compare the soloed rig against, and
	 * comparison is the only reason the row is there.
	 */
	void SetSolo(const AActor* Source);

	/** What is soloed, or null. */
	const AActor* Solo() const { return SoloSource; }

	/**
	 * The subject nearest this road-plane point, or null if the yard is empty.
	 *
	 * THE SOLO KEY'S DECISION, minus the camera. The controller hands it
	 * UBuildCameraComponent::ViewFocus, which is what "the model you are looking at" means for
	 * an orbiting top-down rig; keeping the geometry here is what lets it be measured.
	 */
	const AActor* NearestSubject(FVector2D Point) const;

private:
	/**
	 * The bench's whole simulation. See FYardMotion - one producer, written by the demo loop
	 * and by the scrub alike.
	 *
	 * NOT A UPROPERTY, because FYardMotion is a plain struct holding only doubles and flags -
	 * nothing here can be garbage collected, and reflecting it would mean a USTRUCT and a
	 * UENUM for two enums that no Blueprint will ever name.
	 */
	FYardMotion Motion;

	UPROPERTY() TArray<FYardSubject> SubjectList;

	/** See SetSolo. Null means the whole row runs in lockstep, which is the default. */
	UPROPERTY() TObjectPtr<const AActor> SoloSource;

	/** Push the current motion into every agent. Split out because Tick is not the only caller
	 *  that wants it - adoption pushes once, so a newly adopted model is not in bind pose for
	 *  a frame. */
	void PushMotion();
};
