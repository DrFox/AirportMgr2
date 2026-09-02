#include "Present/RoadAgentActor.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	/** Engine's unit cube is 100 uu, so this is a 4 m x 4 m x 2 m box. */
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

void ARoadAgentActor::SetPose(const FVector2D& Position, double Heading, double SurfaceZ)
{
	// Lifted by half its own height so it stands ON the road rather than half sunk into
	// it: the cube's pivot is at its centre.
	const FVector At(Position.X, Position.Y, SurfaceZ + (CubeUnits * ScaleZ) * 0.5);

	SetActorLocationAndRotation(
		At, FRotator(0.0, FMath::RadiansToDegrees(Heading), 0.0));
}
