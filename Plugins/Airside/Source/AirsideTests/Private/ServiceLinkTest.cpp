#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Build/ServiceLoopBuild.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RouteSearch.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ServiceLinkFixture
{
	/** A straight guideline admitting exactly one class, plus Emergency as derived ones do.
	 *  Returns the node at From; OutFar is the one at To. */
	FGuidelineNodeId Lay(URoadNetwork& Net, const FVector2D& From, const FVector2D& To,
		ETraversalClass Class, FGuidelineNodeId& OutFar)
	{
		const FGuidelineNodeId Near = Net.AddGuidelineNode(From);
		OutFar = Net.AddGuidelineNode(To);

		FGuidelineEdge Edge;
		Edge.A = Near;
		Edge.B = OutFar;
		Edge.Control = (From + To) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::Only(Class);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 600.0;
		Edge.bDerived = true;
		Net.AddGuidelineEdge(MoveTemp(Edge));
		return Near;
	}

	FEntityInstanceId PlaceStand(URoadNetwork& Net, UEntityDefinition& Stand,
		const FVector2D& At, double Heading)
	{
		return Net.PlaceEntity(&Stand, Stand.Anchors, At, Heading,
			Stand.DesignAircraft != nullptr ? Stand.DesignAircraft->Footprint.Wingspan : 0.0,
			Stand.PoseRole, Stand.Trucks);
	}

	/** The node a named anchor resolved to, or an unset handle. */
	FGuidelineNodeId AnchorNode(const URoadNetwork& Net, FEntityInstanceId Entity, const TCHAR* Id)
	{
		const FResolvedAnchor* Found = Net.FindResolvedAnchor(Entity, FName(Id));
		return Found != nullptr ? Found->Node : FGuidelineNodeId();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceLoopReachesTheGraphTest,
	"Airside.Build.ServiceLoopReachesTheGraph",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceLoopReachesTheGraphTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);

	const FServiceLoopBuild::FResult First = FServiceLoopBuild::Build(*Net);
	TestEqual(TEXT("one stand, one lane"), First.LoopsBuilt, 1);
	TestEqual(TEXT("a spur for every service anchor"), First.SpursBuilt, 5);

	// EVERY SERVICE ANCHOR NOW HAS LINE ON IT, and the AIRCRAFT stop mark still does not -
	// the lane is for vehicles, and a painted lead-in is not this builder's business.
	for (const FResolvedAnchor& Anchor : Net->GetEntity(Placed)->ResolvedAnchors)
	{
		const FGuidelineNode* Node = Net->GetGuidelineNode(Anchor.Node);
		if (TestNotNull(TEXT("the anchor resolves"), Node))
		{
			TestTrue(TEXT("and is spurred to the lane"), Node->Incident.Num() > 0);
		}
	}
	TestEqual(TEXT("the aircraft stop mark is untouched"),
		Net->GetGuidelineNode(Net->GetEntity(Placed)->PoseNode)->Incident.Num(), 0);

	// BUT IT IS NOT ON A ROAD. The lane is itself a vehicle guideline, so a check that only
	// counted edges would call this stand connected and no truck would ever route to it.
	TestFalse(TEXT("a lane with no road near it is not a connection"),
		Net->IsServiceNodeConnected(AnchorNode(*Net, Placed, TEXT("HydrantPit"))));

	// AND IT IS A LOOP, not four unjoined sides: every corner has two lane edges on it, so a
	// truck can go round either way. Counted on the anchor-free corners, since a corner that
	// took a spur has three.
	{
		int32 CornersWithTwoWaysRound = 0;
		for (const FGuidelineNodeId& Node : First.Nodes)
		{
			const FGuidelineNode* Found = Net->GetGuidelineNode(Node);
			CornersWithTwoWaysRound += (Found != nullptr && Found->Incident.Num() >= 2) ? 1 : 0;
		}
		TestTrue(TEXT("the lane closes - every lane node has a way round both sides"),
			CornersWithTwoWaysRound >= 4);
	}

	// A SECOND PASS ADDS NOTHING. The graph is rebuilt on every road edit and this runs each
	// time; a builder that could not see its own previous output would stack a lane per pass.
	const FServiceLoopBuild::FResult Second = FServiceLoopBuild::Build(*Net);
	TestEqual(TEXT("a second pass builds no second lane"), Second.LoopsBuilt, 0);
	TestEqual(TEXT("and no second spur"), Second.SpursBuilt, 0);
	TestEqual(TEXT("but it still reports the lane that is there"), Second.Lanes.Num(), 1);

	// A DEPOT HAS NO LANE, so nothing is built for it at all - one pose, nothing parked.
	{
		URoadNetwork* Bare = NewObject<URoadNetwork>(GetTransientPackage());
		UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
		Bare->PlaceEntity(Depot, Depot->Anchors, FVector2D::ZeroVector, 0.0, 0.0,
			Depot->PoseRole, Depot->Trucks);
		TestEqual(TEXT("a depot gets no lane"), FServiceLoopBuild::Build(*Bare).LoopsBuilt, 0);
	}
	return true;
}

#endif
