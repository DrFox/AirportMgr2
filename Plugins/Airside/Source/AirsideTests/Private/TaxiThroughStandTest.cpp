#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Build/AnchorLink.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	/** One straight east-west guideline on each side of the stand, so a ray cast EITHER way
	 *  has something to find. A one-sided fixture could not tell "the second ray was never
	 *  cast" from "it was cast and hit nothing". */
	void TaxiThroughLayLines(URoadNetwork& Net)
	{
		auto Lay = [&Net](double Y)
		{
			const FGuidelineNodeId West = Net.AddGuidelineNode(FVector2D(-10000.0, Y));
			const FGuidelineNodeId East = Net.AddGuidelineNode(FVector2D(10000.0, Y));

			FGuidelineEdge Edge;
			Edge.A = West;
			Edge.B = East;
			Edge.Control = FVector2D(0.0, Y);
			Edge.AllowedTraffic = FTrafficMask::All();
			Edge.Direction = EGuidelineDir::Bidirectional;
			Edge.Width = 2300.0;
			Edge.bDerived = true;
			Net.AddGuidelineEdge(MoveTemp(Edge));
		};

		Lay(0.0);        // the taxiway the stand faces away from
		Lay(8000.0);     // and one on the far side, for a taxi-through stand to reach
	}

	/** Places one stand at (0, 4000) between the two lines, heading +Y - so the ordinary
	 *  lead-in casts along -Y to the line at 0, and a taxi-through one also casts +Y to 8000. */
	FEntityInstanceId TaxiThroughPlaceStand(URoadNetwork& Net, UEntityDefinition* Stand)
	{
		return Net.PlaceEntity(Stand, Stand->Anchors, FVector2D(0.0, 4000.0), UE_DOUBLE_HALF_PI);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaxiThroughStandTest,
	"Airside.Build.TaxiThroughStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiThroughStandTest::RunTest(const FString& Parameters)
{
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	if (!TestNotNull(TEXT("a stand definition"), Stand))
	{
		return false;
	}

	// 1. AN ORDINARY STAND HAS EXACTLY ONE LEAD-IN - today's behaviour, and it must not move.
	//    A second line into one nose-stop is normally a defect, and this is what says the new
	//    flag has not quietly made it the default.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		TaxiThroughLayLines(*Net);
		Stand->bTaxiThrough = false;
		const FEntityInstanceId Placed = TaxiThroughPlaceStand(*Net, Stand);

		FAnchorLink::Build(*Net, UAirsideSettings::ResolveLargestServiceVehicle());

		// Re-read AFTER Build: joining reallocates the node array, which is the trap
		// AnchorLinkTest documents at the same point.
		const FGuidelineNode* Pose = Net->GetGuidelineNode(Net->GetEntity(Placed)->PoseNode);
		if (!TestNotNull(TEXT("the stop position resolves"), Pose))
		{
			return false;
		}
		TestEqual(TEXT("an ordinary stand has exactly one lead-in"), Pose->Incident.Num(), 1);
	}

	// 2. A TAXI-THROUGH STAND HAS TWO, and the second one leaves the OTHER WAY.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		TaxiThroughLayLines(*Net);
		Stand->bTaxiThrough = true;
		const FEntityInstanceId Placed = TaxiThroughPlaceStand(*Net, Stand);

		FAnchorLink::Build(*Net, UAirsideSettings::ResolveLargestServiceVehicle());

		const FGuidelineNode* Pose = Net->GetGuidelineNode(Net->GetEntity(Placed)->PoseNode);
		if (!TestNotNull(TEXT("the stop position resolves"), Pose))
		{
			return false;
		}
		if (!TestEqual(TEXT("a taxi-through stand has a way out as well as a way in"),
			Pose->Incident.Num(), 2))
		{
			return false;
		}

		// THE DIRECTIONS, NOT JUST THE COUNT. A bug that cast the SAME ray twice would produce
		// two incident edges and pass a count assertion while leaving the stand exactly as
		// blocked as before - which is the whole thing this feature exists to change.
		//
		// The stand sits at y = 4000 heading +Y. One line must run away from it toward y = 0
		// (the ordinary lead-in, cast along Heading + PI) and the other toward y = 8000.
		bool bWentBack = false;
		bool bWentForward = false;
		for (const FGuidelineEdgeId EdgeId : Pose->Incident)
		{
			const FGuidelineEdge* Edge = Net->GetGuidelineEdge(EdgeId);
			if (Edge == nullptr)
			{
				continue;
			}

			const FGuidelineNode* Far = Net->GetGuidelineNode(
				Edge->A == Net->GetEntity(Placed)->PoseNode ? Edge->B : Edge->A);
			if (Far == nullptr)
			{
				continue;
			}

			bWentBack = bWentBack || Far->Position.Y < 4000.0;
			bWentForward = bWentForward || Far->Position.Y > 4000.0;
		}

		TestTrue(TEXT("one line runs back out of the stand, as every stand's does"), bWentBack);
		TestTrue(TEXT("and the other runs FORWARD, which is the whole point"), bWentForward);
	}

	return true;
}

#endif
