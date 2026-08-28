#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadNetworkActorTest,
	"RoadNet.Present.NetworkActor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadNetworkActorTest::RunTest(const FString& Parameters)
{
	ARoadNetworkActor* Actor = NewObject<ARoadNetworkActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("actor constructed"), Actor))
	{
		return false;
	}

	// The facade owns creating the network. Until Slice 3's build tool exists nothing
	// else ever would, which is exactly why RebuildMesh used to do nothing at all.
	TestTrue(TEXT("no network before the first edit"), Actor->Network == nullptr);

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(40000.0, 0.0));

	TestTrue(TEXT("placing a node creates the network"), Actor->Network != nullptr);
	TestTrue(TEXT("node handles are real"), A != INDEX_NONE && B != INDEX_NONE);
	TestEqual(TEXT("two nodes placed"), Actor->Network->GetNodes().Num(), 2);

	// Connecting is the only way a segment appears, so URoadNetwork stays the sole
	// owner of the graph invariants the solver depends on - above all the incident
	// lists being sorted by bearing.
	TestTrue(TEXT("connecting two placed nodes succeeds"), Actor->ConnectNodes(A, B));
	TestEqual(TEXT("one segment created"), Actor->Network->GetSegments().Num(), 1);

	// Rejections are reported through the return value rather than swallowed. A build
	// tool that silently drops an edit is indistinguishable from one that is broken.
	TestFalse(TEXT("a node cannot connect to itself"), Actor->ConnectNodes(A, A));
	TestFalse(TEXT("an out-of-range index is rejected"), Actor->ConnectNodes(A, 9999));
	TestFalse(TEXT("a negative index is rejected"), Actor->ConnectNodes(-1, B));
	TestEqual(TEXT("rejected connections created nothing"), Actor->Network->GetSegments().Num(), 1);

	// Picking an existing node is what makes a junction authorable at all: without it
	// every click would start a road disconnected from the last one.
	TestEqual(TEXT("finds the node under the cursor"),
		Actor->FindNodeNear(FVector2D(500.0, 0.0), 1000.0), A);
	TestEqual(TEXT("finds the nearer of two candidates"),
		Actor->FindNodeNear(FVector2D(39000.0, 0.0), 5000.0), B);
	TestEqual(TEXT("nothing inside the radius"),
		Actor->FindNodeNear(FVector2D(20000.0, 0.0), 100.0), INDEX_NONE);

	// Two segments meeting at one node is a junction - the shape Slice 2a exists to
	// render without a seam, and the thing this facade has to make reachable at runtime.
	const int32 C = Actor->PlaceNode(FVector2D(0.0, 40000.0));
	TestTrue(TEXT("a second segment can join the same node"), Actor->ConnectNodes(A, C));
	TestEqual(TEXT("the centre node carries two incident segments"),
		Actor->Network->GetNodes()[A].Incident.Num(), 2);

	// A profile is required to solve, so the facade supplies one when none is authored.
	TestTrue(TEXT("a profile was supplied for the segments"),
		Actor->Network->GetSegment(Actor->Network->GetNodes()[A].Incident[0])->Profile != nullptr);

	Actor->ClearNetwork();
	TestEqual(TEXT("clearing empties the nodes"), Actor->Network->GetNodes().Num(), 0);
	TestEqual(TEXT("clearing empties the segments"), Actor->Network->GetSegments().Num(), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
