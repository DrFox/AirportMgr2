#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/AnchorLink.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	/**
	 * The sharpest direction change anywhere along a polyline, in degrees.
	 *
	 * THE measurement this whole change exists to move. A hard join reads as one ~90 degree
	 * vertex; a swept one spreads the same total turn over many small ones. Asserting on
	 * this rather than on "an arc was added" is what stops the test passing on geometry that
	 * has an arc AND still corners hard where the arc meets the line.
	 */
	double SharpestTurnDegrees(const TArray<FVector2D>& Polyline)
	{
		double Worst = 0.0;
		for (int32 At = 1; At + 1 < Polyline.Num(); ++At)
		{
			const FVector2D In = (Polyline[At] - Polyline[At - 1]).GetSafeNormal();
			const FVector2D Out = (Polyline[At + 1] - Polyline[At]).GetSafeNormal();
			if (In.IsNearlyZero() || Out.IsNearlyZero())
			{
				continue;
			}

			const double Cosine = FMath::Clamp(FVector2D::DotProduct(In, Out), -1.0, 1.0);
			Worst = FMath::Max(Worst, FMath::RadiansToDegrees(FMath::Acos(Cosine)));
		}
		return Worst;
	}

	/** A Code C stand, so the sweep has a published radius to come from. */
	UEntityDefinition* SweepStandDefinition()
	{
		UAircraftType* Aircraft = NewObject<UAircraftType>(GetTransientPackage());
		UAircraftType::BuildA320(Aircraft);

		UEntityDefinition* Stand = NewObject<UEntityDefinition>(GetTransientPackage());
		UEntityDefinition::BuildCodeCStand(Stand);
		Stand->DesignAircraft = Aircraft;
		return Stand;
	}

	/**
	 * A long east-west taxiway with a stand set back to the south of it, facing north.
	 *
	 * Perpendicular deliberately: it is the worst case and the one reported - the lead-in
	 * ray meets the taxiway at a right angle, so a join at the hit point is a 90 degree
	 * corner with nowhere to put a curve.
	 */
	URoadNetwork* SweepFixture(UEntityDefinition* Stand)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Profile = URoadProfile::MakeTransient(2300.0, 1500.0);

		const FRoadNodeId West = Net->AddNode(FVector2D(-30000.0, 0.0));
		const FRoadNodeId East = Net->AddNode(FVector2D(30000.0, 0.0));
		Net->AddStraightSegment(West, East, Profile);

		// Facing north (+Y): the lead-in leaves along heading + 180, i.e. south to north is
		// how the stand faces, so the ray runs north to the taxiway.
		Net->PlaceEntity(Stand, FVector2D(0.0, -9000.0), UE_DOUBLE_PI * 0.5 + UE_DOUBLE_PI);

		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
		FRoadGuidelineBuilder::Build(*Net, Solved);
		FAnchorLink::Build(*Net);
		return Net;
	}

	/** The guideline node nearest a point, whatever it is joined to. */
	FGuidelineNodeId SweepNodeNearest(const URoadNetwork& Network, const FVector2D& Where)
	{
		FGuidelineNodeId Best;
		double BestDistance = TNumericLimits<double>::Max();

		const TArray<FGuidelineNode>& Nodes = Network.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (!Nodes[Index].bAlive)
			{
				continue;
			}
			const double Distance = FVector2D::Distance(Nodes[Index].Position, Where);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best.Index = Index;
				Best.Generation = Nodes[Index].Generation;
			}
		}
		return Best;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLeadInSweepTest,
	"Airside.Build.LeadInSweep",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLeadInSweepTest::RunTest(const FString& Parameters)
{
	UEntityDefinition* Stand = SweepStandDefinition();
	URoadNetwork* Net = SweepFixture(Stand);

	if (!TestTrue(TEXT("the fixture placed a stand"), Net->GetEntities().Num() > 0))
	{
		return false;
	}

	const FEntityInstance& Instance = Net->GetEntities()[0];
	const FGuidelineNode* Pose = Net->GetGuidelineNode(Instance.PoseNode);
	if (!TestNotNull(TEXT("the stand has a stop position in the graph"), Pose))
	{
		return false;
	}

	// The stand must be joined at all before anything about the SHAPE of the join means
	// something - an unreachable stand would trivially have no sharp corner.
	TestTrue(TEXT("the stop position is linked to the taxiway"), Pose->Incident.Num() > 0);

	// A Code C stand's minimum centreline radius is 25 m. Two 90 degree corners each spread
	// over an arc cannot be sharper than a few degrees a sample; the hard join this
	// replaces is a single vertex of very nearly 90.
	constexpr double AcceptableTurnDegrees = 25.0;

	// --- Arriving from the WEST -------------------------------------------------------
	{
		const FGuidelineNodeId From = SweepNodeNearest(*Net, FVector2D(-28000.0, 0.0));
		const FRouteQuery Query{ From, Instance.PoseNode, ETraversalClass::Aircraft, 0.0 };
		const FRoutePlan Plan = RouteSearch::Find(*Net, Query);

		if (TestTrue(TEXT("a route reaches the stand from the west"), Plan.IsValid()))
		{
			const double Worst = SharpestTurnDegrees(Plan.Polyline);
			TestTrue(FString::Printf(
				TEXT("the approach from the west sweeps rather than corners (worst %.1f deg)"),
				Worst),
				Worst < AcceptableTurnDegrees);
		}
	}

	// --- And from the EAST ------------------------------------------------------------
	//
	// Both, because a fillet curves one way: built only toward the nearer end, an aircraft
	// arriving from the other side is routed round a hairpin at the tangent point, which is
	// the same defect moved a few metres rather than fixed.
	{
		const FGuidelineNodeId From = SweepNodeNearest(*Net, FVector2D(28000.0, 0.0));
		const FRouteQuery Query{ From, Instance.PoseNode, ETraversalClass::Aircraft, 0.0 };
		const FRoutePlan Plan = RouteSearch::Find(*Net, Query);

		if (TestTrue(TEXT("a route reaches the stand from the east"), Plan.IsValid()))
		{
			const double Worst = SharpestTurnDegrees(Plan.Polyline);
			TestTrue(FString::Printf(
				TEXT("the approach from the east sweeps rather than corners (worst %.1f deg)"),
				Worst),
				Worst < AcceptableTurnDegrees);
		}
	}

	// --- The join is OFFSET along the taxiway ------------------------------------------
	//
	// The mechanism, measured directly. A swept entry cannot join at the point the lead-in
	// ray strikes: that is the corner, and a curve needs room either side of it. So there
	// must be guideline nodes on the taxiway to BOTH sides of x=0, and none at x=0 itself.
	{
		int32 OnTaxiwayLeft = 0;
		int32 OnTaxiwayRight = 0;
		int32 AtTheCorner = 0;

		for (const FGuidelineNode& Node : Net->GetGuidelineNodes())
		{
			if (!Node.bAlive || FMath::Abs(Node.Position.Y) > 1.0)
			{
				continue;   // not on the taxiway centreline
			}

			if (FMath::Abs(Node.Position.X) < 1.0)      { ++AtTheCorner; }
			else if (Node.Position.X < 0.0)             { ++OnTaxiwayLeft; }
			else                                        { ++OnTaxiwayRight; }
		}

		TestEqual(TEXT("nothing joins at the corner the lead-in ray strikes"), AtTheCorner, 0);
		TestTrue(TEXT("the sweep meets the taxiway west of the corner"), OnTaxiwayLeft > 0);
		TestTrue(TEXT("and east of it"), OnTaxiwayRight > 0);
	}

	return true;
}

#endif
