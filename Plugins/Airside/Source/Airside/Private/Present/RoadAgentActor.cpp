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

}

ARoadAgentActor::ARoadAgentActor()
{
	// The network actor drives this one, so it must not tick on its own account. Two
	// things moving one actor is how a pose ends up a frame behind itself.
	PrimaryActorTick.bCanEverTick = false;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	RootComponent = Mesh;

	// SPAWNS AS THE PLACEHOLDER, and is given its airframe by whoever spawned it - see
	// ARoadNetworkActor::DispatchAgent and SetAirframe below.
	//
	// It used to resolve /Game/.../SM_PiperMeridian here by path. That is a reference the
	// editor cannot see, so moving the content folder left every agent as a cube with only
	// a startup log line to say why. The cube is still the fallback, and it is now the
	// fallback for a missing ASSIGNMENT rather than for a missing path.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (Cube.Succeeded())
	{
		Mesh->SetStaticMesh(Cube.Object);
	}
	Mesh->SetRelativeScale3D(FVector(ScaleXY, ScaleXY, ScaleZ));

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

void ARoadAgentActor::SetPose(const FVector2D& Position, double Heading, double SurfaceZ,
	double Altitude, double PitchDegrees)
{
	// The airframe's origin is already ON the ground - the export puts the wheels at Z=0 -
	// so it needs no lift. The CUBE does, because its pivot is at its centre, and applying
	// the cube's lift to the aircraft flies it a metre above the taxiway: a mistake that
	// reads as a physics or Z-order problem rather than as the arithmetic it is.
	const double Lift = bHasAirframe ? 0.0 : (CubeUnits * ScaleZ) * 0.5;
	const FVector At(Position.X, Position.Y, SurfaceZ + Lift + Altitude);

	// Pitch is FRotator's FIRST argument and yaw its second, which is the opposite order to
	// the way they are spoken of. Getting them the wrong way round yaws the aircraft by its
	// climb attitude and pitches it by its heading - a Piper lying on its side, pointing north.
	SetActorLocationAndRotation(
		At, FRotator(PitchDegrees, FMath::RadiansToDegrees(Heading), 0.0));
}

void ARoadAgentActor::SetAirframe(UStaticMesh* Airframe)
{
	if (Airframe == nullptr || Mesh == nullptr)
	{
		// The cube stands, deliberately. A missing airframe should look like the box this
		// used to be rather than like an agent that failed to spawn - one of those reads as
		// a content problem and the other as a routing bug.
		return;
	}

	Mesh->SetStaticMesh(Airframe);

	// Scale 1: the mesh is already 13.11 m across, the published wingspan. Scaling an
	// airframe here would put the mesh and UAircraftType::Footprint::Wingspan - which every
	// clearance decision uses - quietly out of step.
	Mesh->SetRelativeScale3D(FVector::OneVector);

	// Changes what SetPose must do: the airframe's origin is on the ground, the cube's is at
	// its centre. Applying the cube's lift to an aircraft flies it a metre above the taxiway.
	bHasAirframe = true;
}
