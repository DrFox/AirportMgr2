#include "Present/RoadAgentActor.h"

#include "AirsideLog.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	/** Engine's unit cube is 100 uu, so a scale of 1 is a 1 m box. */
	constexpr double CubeUnits = 100.0;

	/** The AIRCRAFT fallback box: 4 m x 4 m x 2 m. A vehicle's is sized from its own
	 *  footprint instead - see ARoadAgentActor::SetVehicleBody. */
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

	// The engine's own primitive, not an authored asset: a placeholder that shows only
	// until SetAirframe dresses the actor has no business owning content of its own.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (Cube.Succeeded())
	{
		Placeholder->SetStaticMesh(Cube.Object);
	}
	// The member follows the scale, because SetMotion's lift is read off it and a van
	// re-scales this component - see PlaceholderSizeUu.
	PlaceholderSizeUu = FVector(CubeUnits * ScaleXY, CubeUnits * ScaleXY, CubeUnits * ScaleZ);
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
	//
	// HALF THE BOX'S OWN HEIGHT, read off PlaceholderSizeUu rather than the aircraft
	// constants: the box is no longer one size, and a van lifted by an aeroplane's half
	// height floats above the road by exactly the difference.
	const double Lift = bHasAirframe ? 0.0 : PlaceholderSizeUu.Z * 0.5;
	const FVector At(Motion.Position.X, Motion.Position.Y,
		SurfaceZ + Lift + Motion.Altitude);

	// Pitch is FRotator's FIRST argument and yaw its second, which is the opposite order to
	// the way they are spoken of. Getting them the wrong way round yaws the aircraft by its
	// climb attitude and pitches it by its heading - a Piper lying on its side, pointing north.
	const FRotator Rotation(Motion.PitchDegrees, FMath::RadiansToDegrees(Motion.Heading), 0.0);

	// PITCHED ABOUT THE MAIN GEAR, not about the origin.
	//
	// SetActorLocationAndRotation rotates about the ACTOR ORIGIN, which was harmless while
	// every airframe's origin sat mid-fuselage. plane2's origin is its NOSE GEAR - the
	// local space UAircraftType documents - so a nose-up attitude about it swings everything
	// aft downwards: 8 degrees of flare put the mains 63 uu under the tarmac, reported from
	// play as the rear wheels pushing into the ground on landing.
	//
	// The correction holds the pivot still while the body pitches about it. Zero pivot is
	// exactly zero correction, so an unmeasured airframe - every vehicle, both airliners -
	// is untouched by this.
	//
	// THE MERIDIAN JOINED THE CORRECTED SIDE ON 2026-09-21, and it is worth saying because it
	// is the one BEHAVIOUR change in plane7's import rather than a change of numbers. Its
	// origin used to BE its main-gear axle, so FixedAxleX was zero and this did nothing to
	// it; plane7 is exported about the nose gear, FixedAxleX is -237.8, and the Meridian now
	// pitches about its mains like every other measured airframe. That is the correct
	// behaviour and always was - an aeroplane rotates about where the rubber touches - but it
	// was reached by the type's datum moving, not by anyone editing this function. See
	// Airside.Present.MeridianPitchesAboutItsMains.
	//
	// IN THE VIEW rather than in the model, because it is a fact about drawing a rigid body
	// at an attitude, not about where the aircraft is: FAgentMotion::Position still means
	// the origin, and the model still decides it alone.
	//
	// THE CONTACT PATCH, NOT THE HUB, which is why the pivot is a scalar X and this Z is a
	// hard zero rather than anything read off the airframe. The main-gear BONE sits at the
	// axle - on plane2 at z = 68.6, which is exactly the wheel's radius - and pitching about
	// the hub would drag the tyre through the tarmac by that radius. An aeroplane rotates
	// about where the rubber touches, one radius below.
	//
	// Zero IS the ground here because the export puts the wheels at z = 0 and the import
	// fails if they are not - see import_plane2.py's ground check, and the comment above
	// about the origin needing no lift. That assertion is what this line rests on.
	//
	// AGAINST THE YAWED PIVOT, NOT THE UNROTATED ONE, and this is the correction's whole
	// subtlety. The pivot is a point on the BODY, so it turns with the heading whether the
	// aircraft is pitched or not; only the PITCH is supposed to move it. Differencing
	// against the raw offset instead made the correction non-zero at zero pitch - it
	// displaced every agent sideways by up to twice the wheelbase depending on which way it
	// was pointing, reported from play as tracking way off the centre line.
	//
	// So: where the pivot would be with this heading and no pitch, minus where the full
	// rotation puts it. Identical rotations cancel exactly, so level flight and every
	// taxiing aircraft get a correction of precisely zero.
	const FVector Pivot(Motion.PitchPivotX, 0.0, 0.0);
	const FRotator YawOnly(0.0, Rotation.Yaw, 0.0);
	const FVector Correction = YawOnly.RotateVector(Pivot) - Rotation.RotateVector(Pivot);

	SetActorLocationAndRotation(At + Correction, Rotation);

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
	UE_LOG(LogAirside, Log, TEXT("Airframe '%s': %d material slots  %s"),
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

void ARoadAgentActor::SetVehicleBody(UStaticMesh* Mesh, const FVector& BoxSizeUu)
{
	bIsVehicle = true;

	// SIZED FIRST, so a null mesh still leaves a correctly-sized box rather than an
	// aircraft-sized one. The engine's unit cube is 100 uu, so the scale is the size over it.
	PlaceholderSizeUu = BoxSizeUu;
	if (Placeholder != nullptr)
	{
		Placeholder->SetRelativeScale3D(BoxSizeUu / CubeUnits);
	}

	if (Mesh == nullptr || Placeholder == nullptr)
	{
		// The box stands, deliberately, exactly as SetAirframe leaves the cube: a missing
		// asset must look like a placeholder rather than like an agent that failed to spawn -
		// one of those reads as a content problem and the other as a routing bug.
		return;
	}

	// The mesh goes ON the placeholder component, not on the skeletal root: a static mesh
	// cannot live in a skeletal one (see the header), and this is the component that already
	// exists for exactly this shape.
	Placeholder->EmptyOverrideMaterials();
	Placeholder->SetStaticMesh(Mesh);

	// SCALE 1 for a real asset, and the size taken FROM it. A truck modelled at its own size
	// and then scaled to the footprint would put the mesh and FTrafficRules::VehicleFootprint
	// - which the arbiter reserves line by - quietly out of step, the same trap SetAirframe
	// records for the airframe and its wingspan.
	Placeholder->SetRelativeScale3D(FVector::OneVector);
	PlaceholderSizeUu = Mesh->GetBounds().BoxExtent * 2.0;

	// NO LIFT ONCE A REAL MESH IS ON, which the cube still needs and this no longer does.
	//
	// SetMotion lifts by half PlaceholderSizeUu.Z because the engine cube's pivot is at its
	// CENTRE. An authored vehicle's is on the ground - the import asserts it, see
	// import_fueltruck.py's "wheels are on the ground" check - so the same lift floats it by
	// half its own height, which for a 299 uu truck is a metre and a half of clear air. The
	// flag reads "airframe" and means "the visible body's origin is on the ground"; it is
	// the same fact for a truck as for an aeroplane.
	bHasAirframe = true;
}

void ARoadAgentActor::SetVehicleAirframe(USkeletalMesh* Mesh, UClass* AnimClass,
	const FVector& BoxSizeUu)
{
	bIsVehicle = true;

	// SIZED FIRST for the reason SetVehicleBody is: a null mesh must still leave a
	// correctly-sized box rather than an aircraft-sized one.
	PlaceholderSizeUu = BoxSizeUu;
	if (Placeholder != nullptr)
	{
		Placeholder->SetRelativeScale3D(BoxSizeUu / CubeUnits);
	}

	if (Mesh == nullptr)
	{
		// The box stands, exactly as the static path leaves it.
		return;
	}

	// THROUGH SetAirframe, not a copy of it. A rigged truck and a rigged aeroplane want the
	// identical treatment - skeletal mesh on the root, anim instance class, material slots
	// logged, box hidden, lift cleared - and the only thing this adds is that the arbiter's
	// footprint was recorded above. A second implementation would be that list again, free
	// to drift.
	SetAirframe(Mesh, AnimClass);
}
