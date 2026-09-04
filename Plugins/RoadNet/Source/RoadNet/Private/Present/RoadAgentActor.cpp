#include "Present/RoadAgentActor.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	/** Engine's unit cube is 100 uu, so the FALLBACK box is 4 m x 4 m x 2 m. */
	constexpr double CubeUnits = 100.0;
	constexpr double ScaleXY = 4.0;
	constexpr double ScaleZ = 2.0;

	const TCHAR* AirframePath = TEXT("/Game/RoadNet/Aircraft/SM_PiperMeridian.SM_PiperMeridian");
}

ARoadAgentActor::ARoadAgentActor()
{
	// The network actor drives this one, so it must not tick on its own account. Two
	// things moving one actor is how a pose ends up a frame behind itself.
	PrimaryActorTick.bCanEverTick = false;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	RootComponent = Mesh;

	// The real airframe, imported to scale with its origin at the MAIN-GEAR AXLE - which is
	// the point an aircraft pivots about while taxiing, so a follower's pose needs no offset
	// of its own. Nose along +X, matching the yaw SetPose applies.
	//
	// Scale 1: the mesh is already 13.11 m across, the published wingspan. Scaling an
	// airframe here would put the mesh and UAircraftType::Footprint::Wingspan - which every
	// clearance decision uses - quietly out of step.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Airframe(AirframePath);
	if (Airframe.Succeeded())
	{
		Mesh->SetStaticMesh(Airframe.Object);
		bHasAirframe = true;
	}
	else
	{
		// The placeholder, deliberately kept. A missing airframe should look like the box
		// this used to be rather than like an agent that failed to spawn - one of those is
		// obviously a content problem and the other reads as a routing bug.
		static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (Cube.Succeeded())
		{
			Mesh->SetStaticMesh(Cube.Object);
		}
		Mesh->SetRelativeScale3D(FVector(ScaleXY, ScaleXY, ScaleZ));
	}

	// The world is flat and every pick is exact maths against the road plane, exactly as
	// the road and apron meshes have it. An agent that collided would also be something
	// the build tools could trace against by accident.
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);

#if WITH_EDITORONLY_DATA
	// Spawned in numbers and never authored, so it has no business in the outliner's
	// sprite layer - and see ARoadNetworkActor, where a sprite at the origin already read
	// as a node the build tool had drawn.
	Mesh->bVisualizeComponent = false;
#endif
}

void ARoadAgentActor::SetPose(const FVector2D& Position, double Heading, double SurfaceZ)
{
	// The airframe's origin is already ON the ground - the export puts the wheels at Z=0 -
	// so it needs no lift. The CUBE does, because its pivot is at its centre, and applying
	// the cube's lift to the aircraft flies it a metre above the taxiway: a mistake that
	// reads as a physics or Z-order problem rather than as the arithmetic it is.
	const double Lift = bHasAirframe ? 0.0 : (CubeUnits * ScaleZ) * 0.5;
	const FVector At(Position.X, Position.Y, SurfaceZ + Lift);

	SetActorLocationAndRotation(
		At, FRotator(0.0, FMath::RadiansToDegrees(Heading), 0.0));
}
