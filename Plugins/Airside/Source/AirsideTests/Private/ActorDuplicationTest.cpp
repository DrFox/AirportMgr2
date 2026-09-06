#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActorDuplicationTest,
	"Airside.Present.DuplicatedActorOwnsItsSubobjects",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FActorDuplicationTest::RunTest(const FString& Parameters)
{
	// THE PIE BUG OF 2026-09-06. Play-in-editor DUPLICATES the level, and StaticDuplicateObjectEx
	// constructs every duplicate with bCopyTransientsFromClassDefaults, so
	// FObjectInitializer::InitProperties overwrites each Transient, non-instanced property
	// with the CLASS DEFAULT OBJECT's value. Presenter, Facade and Traffic are exactly that
	// kind of property. Result: a PIE actor whose facade belonged to Default__RoadNetworkActor,
	// every click landing on the CDO's private network, the CDO rebuilding a mesh nobody can
	// see, and the real actor's Network staying null until the second click crashed on it.
	//
	// SpawnActor does not take that path, which is why every other actor test passed while
	// PIE was broken. This one duplicates, the way PIE does.
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world to spawn into"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Source = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("source actor spawned"), Source)) { return false; }
	const int32 A = Source->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Source->PlaceNode(FVector2D(20000.0, 0.0));
	if (!TestTrue(TEXT("the source has a road"), Source->ConnectNodes(A, B))) { return false; }
	const int32 SourceNodes = Source->Network->GetNodes().Num();

	ARoadNetworkActor* Dup = DuplicateObject<ARoadNetworkActor>(Source, World->PersistentLevel);
	if (!TestNotNull(TEXT("the actor duplicates"), Dup)) { return false; }

	TestEqual(TEXT("the duplicate's facade belongs to the duplicate, not the CDO"),
		Dup->FacadeOuterForTest(), static_cast<UObject*>(Dup));
	TestEqual(TEXT("the duplicate's presenter belongs to the duplicate"),
		Dup->PresenterOuterForTest(), static_cast<UObject*>(Dup));
	TestEqual(TEXT("the duplicate's traffic belongs to the duplicate"),
		Dup->GetTraffic() ? Dup->GetTraffic()->GetOuter() : nullptr, static_cast<UObject*>(Dup));

	if (!TestNotNull(TEXT("the duplicate has its own network"), Dup->Network.Get())) { return false; }
	TestNotEqual(TEXT("which is a different object from the source's"), Dup->Network.Get(), Source->Network.Get());
	TestEqual(TEXT("with the source's nodes"), Dup->Network->GetNodes().Num(), SourceNodes);

	// The edit that used to go to the CDO: a click on the duplicate must grow the
	// duplicate's own graph, and nothing else's.
	const int32 CdoNodesBefore = GetDefault<ARoadNetworkActor>()->Network
		? GetDefault<ARoadNetworkActor>()->Network->GetNodes().Num() : 0;
	TestTrue(TEXT("placing on the duplicate succeeds"), Dup->PlaceNode(FVector2D(0.0, 5000.0)) != INDEX_NONE);
	TestEqual(TEXT("and lands in the duplicate's network"), Dup->Network->GetNodes().Num(), SourceNodes + 1);
	TestEqual(TEXT("not the source's"), Source->Network->GetNodes().Num(), SourceNodes);
	const int32 CdoNodesAfter = GetDefault<ARoadNetworkActor>()->Network
		? GetDefault<ARoadNetworkActor>()->Network->GetNodes().Num() : 0;
	TestEqual(TEXT("and never the class default object's"), CdoNodesAfter, CdoNodesBefore);
	return true;
}

#endif
