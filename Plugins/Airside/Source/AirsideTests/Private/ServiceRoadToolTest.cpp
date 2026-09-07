#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "InputCoreTypes.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "Model/RoadTraffic.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Tool/BuildSession.h"
#include "Tool/RoadDrawTool.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceRoadToolTest,
	"Airside.Tool.ServiceRoadLaid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceRoadToolTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("the actor"), Actor)) { return false; }

	// Assigned by hand on the ACTOR, which is the per-level override and takes precedence
	// over the content set - so this half of the test says nothing about what is authored.
	URoadProfile* RoadProfile = URoadProfile::MakeServiceRoadTransient();
	Actor->ServiceRoadProfile = RoadProfile;

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(10000.0, 0.0));
	if (!TestTrue(TEXT("a service road connects"),
		Actor->ConnectNodes(A, B, ERoadKind::ServiceRoad))) { return false; }

	if (!TestEqual(TEXT("one segment"), Actor->Network->GetSegments().Num(), 1)) { return false; }

	// THE POINT OF THE WHOLE TASK: the segment carries the ROAD profile, not the taxiway's.
	// Asserted on the POINTER rather than on the width, because a width that happened to
	// match would pass while the guideline class was still Aircraft.
	TestEqual(TEXT("the segment carries the service road profile"),
		Actor->Network->ProfileFor(Actor->Network->GetSegments()[0]),
		static_cast<const URoadProfile*>(RoadProfile));

	// And the two-argument overload still lays a TAXIWAY - every caller written before this
	// task meant that, and there are some fifty of them.
	const int32 C = Actor->PlaceNode(FVector2D(0.0, 10000.0));
	TestTrue(TEXT("the two-argument overload still connects"), Actor->ConnectNodes(A, C));
	TestNotEqual(TEXT("and lays a taxiway, not a road"),
		Actor->Network->ProfileFor(Actor->Network->GetSegments()[1]),
		static_cast<const URoadProfile*>(RoadProfile));

	// CLEARING THE ACTOR'S OVERRIDE FALLS BACK TO THE CONTENT SET, which since this slice
	// names DA_RoadProfile_ServiceRoad. That is the resolver's whole contract, and it is what
	// a player who never touches the Details panel actually gets.
	Actor->ServiceRoadProfile = nullptr;
	const int32 D = Actor->PlaceNode(FVector2D(10000.0, 10000.0));
	TestTrue(TEXT("with no override, the content default lays the road"),
		Actor->ConnectNodes(C, D, ERoadKind::ServiceRoad));
	TestEqual(TEXT("and it is NOT the taxiway profile"),
		Actor->Network->ProfileFor(Actor->Network->GetSegments()[2]) == Actor->ResolveProfileForTest(),
		false);

	// A REFUSAL, NOT A SILENT TAXIWAY, when there is no road profile ANYWHERE.
	//
	// The configured content set is cleared for exactly this assertion and restored straight
	// after. It has to be: once DA_AirsideContent names the profile there is no other way to
	// reach the branch, and the branch is the one that matters - a silent fallback to the
	// taxiway would admit AIRCRAFT onto a lane laid for vans, and nothing would report it.
	// This test passed for a while only because no service road asset existed yet.
	{
		UAirsideSettings* Settings = GetMutableDefault<UAirsideSettings>();
		const TSoftObjectPtr<UAirsideContent> Configured = Settings->Content;
		Settings->Content.Reset();
		ON_SCOPE_EXIT { Settings->Content = Configured; };

		const int32 E = Actor->PlaceNode(FVector2D(20000.0, 20000.0));
		const int32 Before = Actor->Network->GetSegments().Num();
		TestFalse(TEXT("no road profile anywhere refuses the road"),
			Actor->ConnectNodes(D, E, ERoadKind::ServiceRoad));
		TestEqual(TEXT("and lays nothing"), Actor->Network->GetSegments().Num(), Before);
	}

	// The registry is ONE list (CLAUDE.md): the tool exists, under key 9, named Road, and
	// its own display name agrees with the table - which is what Airside.Tool.BuildSession
	// asserts for every other entry.
	bool bFoundRoad = false;
	for (const FToolRegistration& Entry : ToolRegistry())
	{
		if (Entry.Key == EKeys::Nine)
		{
			bFoundRoad = true;
			TestEqual(TEXT("key 9 is the Road tool"), Entry.Name.ToString(), FString(TEXT("Road")));
			const TUniquePtr<IBuildTool> Tool = Entry.Make();
			TestEqual(TEXT("and its display name agrees with the registry"),
				Tool->GetDisplayName().ToString(), Entry.Name.ToString());
		}
	}
	TestTrue(TEXT("key 9 is registered"), bFoundRoad);
	return true;
}

#endif
