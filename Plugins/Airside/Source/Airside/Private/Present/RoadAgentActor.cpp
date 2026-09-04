#include "Present/RoadAgentActor.h"

#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	/** Engine's unit cube is 100 uu, so the FALLBACK box is 4 m x 4 m x 2 m. */
	constexpr double CubeUnits = 100.0;
	constexpr double ScaleXY = 4.0;
	constexpr double ScaleZ = 2.0;

}

ARoadAgentActor::ARoadAgentActor()
{
	// The network actor drives this one, so it must not tick on its own account. Two
	// things moving one actor is how a pose ends up a frame behind itself.
	PrimaryActorTick.bCanEverTick = false;

	// The aircraft, and the root. Empty until SetAirframe dresses it - see the header.
	Airframe = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Airframe"));
	RootComponent = Airframe;

	// The placeholder hangs off it, visible only while there is no airframe.
	Placeholder = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Placeholder"));
	Placeholder->SetupAttachment(Airframe);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (Cube.Succeeded())
	{
		Placeholder->SetStaticMesh(Cube.Object);
	}
	Placeholder->SetRelativeScale3D(FVector(ScaleXY, ScaleXY, ScaleZ));

	// The world is flat and every pick is exact maths against the road plane, exactly as
	// the road and apron meshes have it. An agent that collided would also be something
	// the build tools could trace against by accident.
	for (UPrimitiveComponent* Component : { static_cast<UPrimitiveComponent*>(Airframe),
	                                        static_cast<UPrimitiveComponent*>(Placeholder) })
	{
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetCastShadow(false);
	}

#if WITH_EDITORONLY_DATA
	// Spawned in numbers and never authored, so it has no business in the outliner's
	// sprite layer - and see ARoadNetworkActor, where a sprite at the origin already read
	// as a node the build tool had drawn.
	Airframe->bVisualizeComponent = false;
	Placeholder->bVisualizeComponent = false;
#endif
}

void ARoadAgentActor::SetMotion(const FAgentMotion& Motion, double SurfaceZ)
{
	// The airframe's origin is already ON the ground - the export puts the wheels at Z=0 -
	// so it needs no lift. The CUBE does, because its pivot is at its centre, and applying
	// the cube's lift to the aircraft flies it a metre above the taxiway: a mistake that
	// reads as a physics or Z-order problem rather than as the arithmetic it is.
	const double Lift = bHasAirframe ? 0.0 : (CubeUnits * ScaleZ) * 0.5;
	const FVector At(Motion.Position.X, Motion.Position.Y,
		SurfaceZ + Lift + Motion.Altitude);

	// Pitch is FRotator's FIRST argument and yaw its second, which is the opposite order to
	// the way they are spoken of. Getting them the wrong way round yaws the aircraft by its
	// climb attitude and pitches it by its heading - a Piper lying on its side, pointing north.
	SetActorLocationAndRotation(
		At, FRotator(Motion.PitchDegrees, FMath::RadiansToDegrees(Motion.Heading), 0.0));

	// KEPT, NOT CONSUMED, until the mesh is skeletal. The animation reads these - wheel rate
	// is GroundSpeed over the wheel radius, the propeller turns while the engine does - and
	// storing them now means the model side is finished and testable before the asset lands.
	LastMotion = Motion;
}

void ARoadAgentActor::SetAirframe(USkeletalMesh* InAirframe, UClass* AnimClass)
{
	if (InAirframe == nullptr || Airframe == nullptr)
	{
		// The cube stands, deliberately. A missing airframe should look like the box this
		// used to be rather than like an agent that failed to spawn - one of those reads as
		// a content problem and the other as a routing bug.
		return;
	}

	// A component that has ever had an override keeps it across a mesh swap, and an override
	// left pointing at nothing renders as the grey default - "clay". Cleared before the mesh
	// goes in so the slots come from the asset and only from the asset.
	Airframe->EmptyOverrideMaterials();
	Airframe->SetSkeletalMeshAsset(InAirframe);

	// Scale 1: the mesh is already 13.11 m across, the published wingspan. Scaling an
	// airframe here would put the mesh and UAircraftType::Footprint::Wingspan - which every
	// clearance decision uses - quietly out of step.
	Airframe->SetRelativeScale3D(FVector::OneVector);

	// AFTER the mesh, because an anim instance binds to the skeleton it finds when it is
	// created - set first, it would bind to nothing and every bone would sit in its
	// reference pose with no error to say why.
	if (AnimClass != nullptr)
	{
		Airframe->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		Airframe->SetAnimInstanceClass(AnimClass);
	}

	// SAID OUT LOUD, because a mesh with correct slots drawn as grey default is otherwise
	// indistinguishable from a missing material, a missing mesh, or a failed spawn - and the
	// asset having the right materials proves nothing about what the COMPONENT resolved.
	const int32 SlotCount = Airframe->GetNumMaterials();
	FString Slots;
	for (int32 Slot = 0; Slot < SlotCount; ++Slot)
	{
		const UMaterialInterface* Applied = Airframe->GetMaterial(Slot);
		Slots += FString::Printf(TEXT("[%d]=%s "), Slot,
			Applied != nullptr ? *Applied->GetName() : TEXT("null"));
	}
	UE_LOG(LogTemp, Log, TEXT("Airframe '%s': %d material slots  %s"),
		*InAirframe->GetName(), SlotCount, *Slots);

	// The box has done its job.
	if (Placeholder != nullptr)
	{
		Placeholder->SetVisibility(false);
	}

	// Changes what SetPose must do: the airframe's origin is on the ground, the cube's is at
	// its centre. Applying the cube's lift to an aircraft flies it a metre above the taxiway.
	bHasAirframe = true;
}
