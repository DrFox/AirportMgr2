#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.
	struct FMergeFixture
	{
		URoadNetwork* Network = nullptr;

		explicit FMergeFixture()
			: Network(NewObject<URoadNetwork>())
		{
		}

		/** A profile of a known total width - one lane band, which is all the widest-wins
		 *  rule reads. */
		URoadProfile* Profile(double TotalWidth) const
		{
			URoadProfile* Made = NewObject<URoadProfile>();
			FProfileBand Band;
			Band.Width = TotalWidth;
			Band.Type = ERoadBandType::Lane;
			Made->Bands.Add(Band);
			return Made;
		}

		int32 LiveSegments() const
		{
			int32 Count = 0;
			for (const FRoadSegment& Segment : Network->GetSegments())
			{
				Count += Segment.bAlive ? 1 : 0;
			}
			return Count;
		}

		int32 LiveNodes() const
		{
			int32 Count = 0;
			for (const FRoadNode& Node : Network->GetNodes())
			{
				Count += Node.bAlive ? 1 : 0;
			}
			return Count;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMergeCollapsesASharedSegmentTest,
	"Airside.Model.MergeCollapsesASharedSegment",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMergeCollapsesASharedSegmentTest::RunTest(const FString& Parameters)
{
	// A-B-C, and A is dragged onto B. The A-B arm runs BETWEEN the two nodes being merged,
	// so once they are one node it has no length and no direction - a self-loop the junction
	// solver has no answer for. Collapsing a stubby pair like this IS the reported problem:
	// two nodes a few metres apart make a corner FSpeedProfile refuses to take at speed.
	FMergeFixture Fix;
	const FRoadNodeId A = Fix.Network->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId B = Fix.Network->AddNode(FVector2D(300.0, 0.0));
	const FRoadNodeId C = Fix.Network->AddNode(FVector2D(9000.0, 0.0));
	Fix.Network->AddStraightSegment(A, B, nullptr);
	Fix.Network->AddStraightSegment(B, C, nullptr);

	TestTrue(TEXT("the merge is accepted"), Fix.Network->MergeNodes(B, A));

	TestEqual(TEXT("the stub between the merged pair is gone, leaving one road"),
		Fix.LiveSegments(), 1);
	TestEqual(TEXT("and one of the two nodes with it"), Fix.LiveNodes(), 2);
	TestNull(TEXT("the absorbed node is dead"), Fix.Network->GetNode(A));

	const FRoadNode* Survivor = Fix.Network->GetNode(B);
	if (!TestNotNull(TEXT("the kept node lives"), Survivor)) { return false; }
	TestEqual(TEXT("and keeps the one arm that was not a self-loop"), Survivor->Incident.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMergeRepointsAnUnsharedArmTest,
	"Airside.Model.MergeRepointsAnUnsharedArm",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMergeRepointsAnUnsharedArmTest::RunTest(const FString& Parameters)
{
	// C-A and B-D, no shared end. Merging A into B has to give B both arms: this is the
	// case where a merge JOINS two roads rather than tidying one.
	FMergeFixture Fix;
	const FRoadNodeId A = Fix.Network->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId B = Fix.Network->AddNode(FVector2D(200.0, 0.0));
	const FRoadNodeId C = Fix.Network->AddNode(FVector2D(-9000.0, 0.0));
	const FRoadNodeId D = Fix.Network->AddNode(FVector2D(9000.0, 0.0));
	Fix.Network->AddStraightSegment(C, A, nullptr);
	Fix.Network->AddStraightSegment(B, D, nullptr);

	TestTrue(TEXT("the merge is accepted"), Fix.Network->MergeNodes(B, A));

	TestEqual(TEXT("both roads survive - nothing was between the merged pair to collapse"),
		Fix.LiveSegments(), 2);

	const FRoadNode* Survivor = Fix.Network->GetNode(B);
	if (!TestNotNull(TEXT("the kept node lives"), Survivor)) { return false; }
	TestEqual(TEXT("and now carries both arms, so the two roads are one run"),
		Survivor->Incident.Num(), 2);

	bool bReachesC = false;
	bool bReachesD = false;
	for (const FRoadSegmentId Arm : Survivor->Incident)
	{
		const FRoadNodeId Far = Fix.Network->GetOtherEnd(Arm, B);
		bReachesC |= (Far == C);
		bReachesD |= (Far == D);
	}
	TestTrue(TEXT("the absorbed node's far end is reachable from the survivor"), bReachesC);
	TestTrue(TEXT("and so is the survivor's own"), bReachesD);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMergeKeepsTheWiderDuplicateArmTest,
	"Airside.Model.MergeKeepsTheWiderDuplicateArm",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMergeKeepsTheWiderDuplicateArmTest::RunTest(const FString& Parameters)
{
	// A-C wide, B-C narrow. Merging A into B would leave TWO segments between B and C -
	// coincident pavement at one Z, which is a z-fight and not a surface. The wider survives:
	// ground geometry is sized for the largest aircraft admitted, so collapsing a stub must
	// never silently narrow a route something was cleared for.
	FMergeFixture Fix;
	const FRoadNodeId A = Fix.Network->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId B = Fix.Network->AddNode(FVector2D(300.0, 0.0));
	const FRoadNodeId C = Fix.Network->AddNode(FVector2D(150.0, 9000.0));

	URoadProfile* Wide = Fix.Profile(2300.0);       // 23 m
	URoadProfile* Narrow = Fix.Profile(1500.0);     // 15 m
	Fix.Network->AddStraightSegment(A, C, Wide);
	Fix.Network->AddStraightSegment(B, C, Narrow);

	TestTrue(TEXT("the merge is accepted"), Fix.Network->MergeNodes(B, A));

	TestEqual(TEXT("exactly one arm survives between the kept node and the shared end - two "
				   "would be coincident pavement at one Z"), Fix.LiveSegments(), 1);

	const FRoadNode* Survivor = Fix.Network->GetNode(B);
	if (!TestNotNull(TEXT("the kept node lives"), Survivor)) { return false; }
	if (!TestEqual(TEXT("with one arm"), Survivor->Incident.Num(), 1)) { return false; }

	const FRoadSegment* Kept = Fix.Network->GetSegment(Survivor->Incident[0]);
	if (!TestNotNull(TEXT("the surviving arm"), Kept)) { return false; }
	TestEqual(TEXT("and it is the WIDER of the two, never the narrower"),
		Kept->Profile.Get(), Wide);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMergeKeepsAStraightSegmentStraightTest,
	"Airside.Model.MergeKeepsAStraightSegmentStraight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMergeKeepsAStraightSegmentStraightTest::RunTest(const FString& Parameters)
{
	// A repointed arm's CONTROL point has to travel with its endpoint, by half the
	// displacement - the rule SetNodePosition already applies. Without it the arm keeps
	// aiming at where its end used to be, because GetOutgoingTangent derives direction from
	// Control and not from the endpoints, and the incidence sort then runs on stale geometry.
	FMergeFixture Fix;
	const FRoadNodeId A = Fix.Network->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId B = Fix.Network->AddNode(FVector2D(1000.0, 2000.0));
	const FRoadNodeId C = Fix.Network->AddNode(FVector2D(-9000.0, 0.0));
	Fix.Network->AddStraightSegment(C, A, nullptr);

	TestTrue(TEXT("the merge is accepted"), Fix.Network->MergeNodes(B, A));

	const FRoadNode* Survivor = Fix.Network->GetNode(B);
	if (!TestNotNull(TEXT("the kept node lives"), Survivor)) { return false; }
	if (!TestEqual(TEXT("carrying the repointed arm"), Survivor->Incident.Num(), 1)) { return false; }

	const FRoadSegment* Arm = Fix.Network->GetSegment(Survivor->Incident[0]);
	if (!TestNotNull(TEXT("the repointed arm"), Arm)) { return false; }

	// STRAIGHT MEANS Control == (A+B)/2 exactly. Recomputing Control as the new midpoint
	// would pass this and silently flatten every CURVED arm instead, which is why the rule
	// is a half-displacement shift rather than a recompute.
	const FVector2D Expected = (FVector2D(-9000.0, 0.0) + FVector2D(1000.0, 2000.0)) * 0.5;
	TestTrue(TEXT("the repointed arm is still exactly straight - its control point travelled "
				  "by half the endpoint's displacement"),
		Arm->Control.Equals(Expected, 1e-6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMergeSortsIncidentByBearingTest,
	"Airside.Model.MergeSortsIncidentByBearing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMergeSortsIncidentByBearingTest::RunTest(const FString& Parameters)
{
	// URoadNetwork's contract is that Incident stays sorted by outgoing bearing, and the
	// junction solver walks it assuming so - an arm in the wrong slot puts one road's
	// geometry on another road's cut line. A merge changes bearings at BOTH ends of every
	// repointed arm, which is the half SetNodePosition records being invisible until a
	// junction is solved.
	FMergeFixture Fix;
	const FRoadNodeId A = Fix.Network->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId B = Fix.Network->AddNode(FVector2D(200.0, 0.0));

	// Arms fanning out from A in an order that is NOT bearing order once they hang off B.
	const FRoadNodeId N = Fix.Network->AddNode(FVector2D(0.0, 9000.0));
	const FRoadNodeId E = Fix.Network->AddNode(FVector2D(9000.0, 0.0));
	const FRoadNodeId S = Fix.Network->AddNode(FVector2D(0.0, -9000.0));
	const FRoadNodeId W = Fix.Network->AddNode(FVector2D(-9000.0, 0.0));
	Fix.Network->AddStraightSegment(A, N, nullptr);
	Fix.Network->AddStraightSegment(A, S, nullptr);
	Fix.Network->AddStraightSegment(B, E, nullptr);
	Fix.Network->AddStraightSegment(B, W, nullptr);

	TestTrue(TEXT("the merge is accepted"), Fix.Network->MergeNodes(B, A));

	const FRoadNode* Survivor = Fix.Network->GetNode(B);
	if (!TestNotNull(TEXT("the kept node lives"), Survivor)) { return false; }
	if (!TestEqual(TEXT("with all four arms"), Survivor->Incident.Num(), 4)) { return false; }

	// MEASURED, not asserted by name. A test that merely said "SortIncident was called"
	// would pass on a list sorted by nothing at all.
	double Previous = -TNumericLimits<double>::Max();
	for (const FRoadSegmentId Arm : Survivor->Incident)
	{
		const double Bearing = RoadGeom::Bearing(Fix.Network->GetOutgoingTangent(Arm, B));
		TestTrue(TEXT("the kept node's arms are in ascending bearing order after the merge"),
			Bearing >= Previous);
		Previous = Bearing;
	}
	return true;
}

#endif
