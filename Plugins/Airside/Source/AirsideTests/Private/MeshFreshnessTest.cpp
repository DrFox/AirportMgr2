#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeshFreshnessTest,
	"Airside.Present.MeshIsFreshAfterLoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMeshFreshnessTest::RunTest(const FString& Parameters)
{
	// A DERIVED MESH THAT IS SAVED WILL GO STALE, SO A LOAD MUST REBUILD IT.
	//
	// UDynamicMeshComponent declares its mesh UPROPERTY(Instanced) with no Transient flag,
	// so the surface is serialised into the level whether we want it or not - and there is
	// no engine switch to decline. The mesh is DERIVED: the model is the truth and the
	// surface is a function of it. Persisting a derived value is a cache, and this one had
	// no invalidation at all.
	//
	// What that looked like to the user: open the level, the roads look right; run the
	// game, draw one road, and roads drawn in a PREVIOUS session change width and material.
	// Nothing had touched them. They had been wrong since the moment the level opened - the
	// picture on screen was the saved cache - and the first rebuild for any reason replaced
	// it with what the model actually said. Measured on the real level: 276 triangles saved
	// against 194 rebuilt. It read as "the road tool corrupts my roads" and cost six wrong
	// diagnoses, every one of them aimed at materials, because the change SHOWED as colour.
	//
	// The property below is the one that makes that impossible to reproduce: once loading
	// rebuilds, a later rebuild has nothing left to change.
	// A REAL WORLD, because the thing under test is a REGISTRATION hook. Every other test
	// here builds its actor with NewObject and no world, which is cheaper and enough for
	// them - but AActor::PostRegisterAllComponents dereferences the world, and calling the
	// override directly would only prove the body works, not that anything invokes it. That
	// is the mistake this codebase has now shipped three times: testing a list where it is
	// declared rather than where it is consumed.
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world to register components in"), World))
	{
		return false;
	}
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);

	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("actor spawned"), Actor))
	{
		return false;
	}

	URoadProfile* Taxiway = URoadProfile::MakeTransient(400.0, 200.0, 40.0);

	// Built through the NETWORK rather than the actor's PlaceNode/ConnectNodes, so every
	// handle in this test comes from one numbering. The actor's node indices are its own,
	// and mixing the two silently produced a segment between nodes that did not exist -
	// which showed up as a mesh that correctly did not change.
	//
	// The network is created on demand, so one call through the actor is what brings it
	// into being. The node it leaves behind is isolated and contributes no surface.
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));

	if (!TestNotNull(TEXT("the actor has a network"), Actor->Network.Get()))
	{
		return false;
	}
	URoadNetwork& Net = *Actor->Network;

	// 1. A BUILT SURFACE. This is the state a level is saved in.
	const FRoadNodeId A = Net.AddNode(FVector2D::ZeroVector);
	const FRoadNodeId B = Net.AddNode(FVector2D(5000.0, 0.0));
	Net.AddStraightSegment(A, B, Taxiway);
	Actor->RebuildMesh();

	const int32 Saved = Actor->SurfaceTriangleCountForTest();
	if (!TestTrue(TEXT("a connected pair of nodes builds a surface"), Saved > 0))
	{
		return false;
	}

	// 2. THE MODEL MOVES ON WITHOUT THE MESH. Adding straight to the network is how a load
	//    arrives: the graph is deserialised, and nothing has run the builder over it. Going
	//    through the actor would rebuild and hide exactly what is being measured.
	const FRoadNodeId C = Net.AddNode(FVector2D(5000.0, 5000.0));
	if (!TestTrue(TEXT("a second segment really was added to the model"),
		Net.AddStraightSegment(B, C, Taxiway).IsSet()))
	{
		return false;
	}

	TestEqual(TEXT("the mesh is now stale - it still shows the old model"),
		Actor->SurfaceTriangleCountForTest(), Saved);

	// 3. THE MEASUREMENT. Re-registering runs the same path a level load does, and it must
	//    leave the surface agreeing with the model.
	Actor->ReregisterAllComponents();

	const int32 AfterLoad = Actor->SurfaceTriangleCountForTest();
	TestNotEqual(TEXT("loading rebuilds the surface from the model, not the saved cache"),
		AfterLoad, Saved);

	// 4. AND THE PROPERTY THE USER ACTUALLY NEEDS: having loaded, drawing a road cannot
	//    change any road already there. This is the bug report, written as an assertion -
	//    a rebuild after a load is a no-op, so there is no moment at which old roads move.
	Actor->RebuildMesh();
	TestEqual(TEXT("and a later rebuild then changes nothing at all"),
		Actor->SurfaceTriangleCountForTest(), AfterLoad);

	return true;
}

#endif
