#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Build/BendWidening.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/Vehicle.h"
#include "Profiles/RoadDesignVehicles.h"
#include "Profiles/RoadProfile.h"
#include "Solve/JunctionSolver.h"

#if WITH_DEV_AUTOMATION_TESTS

// #305: FJunctionResult::InnerCornerOfBend() replaces the hand-rolled "which corner is the
// inside" loop that BendWidening::InnerEdgeOf, FRoadNetworkSolver::SmoothBend and
// FRoadGuidelineBuilder::Build each had their own copy of. InnerEdgeOf is private to
// BendWidening.cpp and now calls the shared helper itself, so the two cannot drift apart
// without this test going red: it rebuilds the same two-arm bend BendWidening::Measure (the
// only caller that can reach InnerEdgeOf) is fed, and checks its published Out.Corner against
// InnerCornerOfBend() run on the same junction.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInnerCornerOfBendAgreesWithMeasureTest,
	"Airside.Build.BendLanes.InnerCornerAgreesWithMeasure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInnerCornerOfBendAgreesWithMeasureTest::RunTest(const FString& Parameters)
{
	// THE RIG COURSE'S OWN SHAPE (BendLaneTest's FBend): a plain right-angle bend on the Wide
	// tier, the one combination Airside.Build.BendLanes.WideBendCarriesTheRig already proves
	// needs its inside widened - so Measure is guaranteed to reach the Out.Corner assignment
	// this test reads, not bail out early with nothing to compare.
	const TArray<URoadProfile*> Profiles = TestProfiles::ServiceTiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }
	URoadProfile* Wide = Profiles[UAirsideSettings::WideServiceTier];

	const TestGraph::FCornerFixture Bend = TestGraph::Corner(Wide);
	const FJunctionResult* SolvedResult = Bend.Solved.NodeResults.Find(Bend.Corner.Index);
	const TArray<FRoadSegmentId>* ArmSegments = Bend.Solved.NodeArmSegments.Find(Bend.Corner.Index);
	if (!TestTrue(TEXT("the bend solves with two arms"),
		SolvedResult != nullptr && SolvedResult->bValid
		&& ArmSegments != nullptr && ArmSegments->Num() == 2)) { return false; }

	const int32 ExpectedInner = SolvedResult->InnerCornerOfBend();
	if (!TestTrue(TEXT("one corner is the rounded inside"), ExpectedInner != INDEX_NONE)) { return false; }

	// BuildNodeInput's own recipe (RoadNetworkSolver.cpp), read back through the same public
	// accessors it uses - not a second copy of the solve, a caller of the same profile queries.
	const FRoadDesignVehicles Designs = UAirsideSettings::ResolveRoadDesignVehicles();
	const FVehicle Rig = Designs.VehicleFor(Wide);

	FJunctionInput Input;
	Input.Position = Bend.Net->GetNode(Bend.Corner)->Position;
	TArray<BendWidening::FLane> Lanes[2];
	for (int32 Arm = 0; Arm < 2; ++Arm)
	{
		const FRoadSegmentId ArmSeg = (*ArmSegments)[Arm];
		const FRoadSegment* Segment = Bend.Net->GetSegment(ArmSeg);
		if (!TestTrue(TEXT("the arm's segment is alive"), Segment != nullptr)) { return false; }
		const bool bAtA = Segment->A == Bend.Corner;

		FJunctionArm JunctionArm;
		JunctionArm.Tangent = Bend.Net->GetOutgoingTangent(ArmSeg, Bend.Corner);
		JunctionArm.HalfWidthLeft = Wide->GetHalfWidthLeft();
		JunctionArm.HalfWidthRight = Wide->GetHalfWidthRight();
		JunctionArm.FilletRadius = Wide->ResolvedFilletRadius(Designs.For(Wide));
		Input.Arms.Add(JunctionArm);

		for (const FProfileGuideline& Guideline : Wide->Guidelines)
		{
			if (Guideline.Class != ETraversalClass::GroundVehicle) { continue; }
			BendWidening::FLane& Lane = Lanes[Arm].AddDefaulted_GetRef();
			Lane.Lateral = (bAtA ? 1.0 : -1.0) * Guideline.OffsetFor(Bend.Net->GetDriveSide());
			Lane.bArrives = Guideline.ArrivesAt(bAtA);
			Lane.bLeaves = Guideline.LeavesFrom(bAtA);
		}
	}
	if (!TestTrue(TEXT("both arms have at least one service-road lane"),
		Lanes[0].Num() > 0 && Lanes[1].Num() > 0)) { return false; }

	const FJunctionResult Reconstructed = FJunctionSolver::SolveCuts(Input);
	if (!TestTrue(TEXT("the reconstructed junction is the same shape as the fixture's"),
		Reconstructed.bValid && Reconstructed.Corners.Num() == 2
		&& Reconstructed.InnerCornerOfBend() == ExpectedInner)) { return false; }

	BendWidening::FWidening Widening;
	if (!TestTrue(TEXT("the rig needs the inside widened on this bend"),
		BendWidening::Measure(Input, Reconstructed, Lanes, Rig, Widening))) { return false; }

	TestEqual(TEXT("BendWidening::Measure's Out.Corner (InnerEdgeOf) agrees with FJunctionResult::InnerCornerOfBend"),
		Widening.Corner, ExpectedInner);
	return true;
}

#endif
