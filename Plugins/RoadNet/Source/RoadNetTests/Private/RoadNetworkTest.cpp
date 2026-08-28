#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadNetworkTest,
	"RoadNet.Model.Network",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadNetworkTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(2300.0, 1500.0);

	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId East   = Net->AddNode(FVector2D(10000.0, 0.0));
	const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 10000.0));
	const FRoadNodeId West   = Net->AddNode(FVector2D(-10000.0, 0.0));

	const FRoadSegmentId ToNorth = Net->AddStraightSegment(Centre, North, Profile);
	const FRoadSegmentId ToEast  = Net->AddStraightSegment(Centre, East,  Profile);
	const FRoadSegmentId ToWest  = Net->AddStraightSegment(Centre, West,  Profile);

	TestTrue(TEXT("segments created"), ToNorth.IsValid() && ToEast.IsValid() && ToWest.IsValid());

	// Outgoing tangents at the centre node.
	const FVector2D TanEast = Net->GetOutgoingTangent(ToEast, Centre);
	TestTrue(TEXT("east tangent"), TanEast.Equals(FVector2D(1.0, 0.0), 1e-6));

	const FVector2D TanNorth = Net->GetOutgoingTangent(ToNorth, Centre);
	TestTrue(TEXT("north tangent"), TanNorth.Equals(FVector2D(0.0, 1.0), 1e-6));

	// Tangent at the far end points back toward the centre.
	const FVector2D TanBack = Net->GetOutgoingTangent(ToEast, East);
	TestTrue(TEXT("reverse tangent"), TanBack.Equals(FVector2D(-1.0, 0.0), 1e-6));

	// Incident list sorted by bearing ascending: east(0), north(UE_DOUBLE_PI/2), west(UE_DOUBLE_PI).
	const FRoadNode* CentreNode = Net->GetNode(Centre);
	TestEqual(TEXT("incident count"), CentreNode->Incident.Num(), 3);
	TestTrue(TEXT("order[0] east"),  CentreNode->Incident[0] == ToEast);
	TestTrue(TEXT("order[1] north"), CentreNode->Incident[1] == ToNorth);
	TestTrue(TEXT("order[2] west"),  CentreNode->Incident[2] == ToWest);

	// Removing a segment updates both endpoints' incident lists.
	TestTrue(TEXT("remove"), Net->RemoveSegment(ToNorth));
	TestEqual(TEXT("incident after remove"), Net->GetNode(Centre)->Incident.Num(), 2);
	TestEqual(TEXT("far node emptied"), Net->GetNode(North)->Incident.Num(), 0);
	TestNull(TEXT("segment gone"), Net->GetSegment(ToNorth));

	// Removing a node cascades to its segments.
	TestTrue(TEXT("remove centre"), Net->RemoveNode(Centre));
	TestNull(TEXT("centre gone"), Net->GetNode(Centre));
	TestNull(TEXT("east segment cascaded"), Net->GetSegment(ToEast));
	TestEqual(TEXT("east node emptied"), Net->GetNode(East)->Incident.Num(), 0);

	// Self-loops and invalid handles are rejected.
	TestFalse(TEXT("self loop rejected"), Net->AddStraightSegment(East, East, Profile).IsValid());
	TestFalse(TEXT("stale handle rejected"), Net->AddStraightSegment(Centre, East, Profile).IsValid());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
