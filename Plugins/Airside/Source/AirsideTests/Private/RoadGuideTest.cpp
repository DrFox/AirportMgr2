#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/BuildSession.h"
#include "Tool/RoadDrawTool.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/SnapGuideChain.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A road-drawing session with the taxiway tool selected, found BY ID. */
	struct FRoadGesture
	{
		FAirsideTestWorld TestWorld;
		FBuildSession Session;
		FBuildSessionTunables Tunables;
		IBuildTool* Tool = nullptr;

		FToolContext At(const FVector2D& Where) const
		{
			return Session.MakeContext(TestWorld.Actor, Where, Tunables, false, false);
		}

		FRoadDrawTool* Road() const { return static_cast<FRoadDrawTool*>(Tool); }

		const URoadNetwork* Network() const { return TestWorld.Actor->Network; }
	};

	bool StartRoadGesture(FRoadGesture& Out)
	{
		if (Out.TestWorld.World == nullptr || Out.TestWorld.Actor == nullptr) { return false; }

		// BY ID, never a literal index: the next tool added to the registry moves every one.
		int32 Taxiway = INDEX_NONE;
		const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
		for (int32 Index = 0; Index < Registry.Num(); ++Index)
		{
			if (Registry[Index].Id == FName(TEXT("Taxiway"))) { Taxiway = Index; }
		}
		if (Taxiway == INDEX_NONE) { return false; }

		Out.Tunables = Out.TestWorld.Actor->MakeTunables(10000.0);
		Out.Session.SelectTool(Taxiway);
		Out.Tool = Out.Session.GetActiveTool();
		return Out.Tool != nullptr;
	}
}

/**
 * A CHAIN EXTENDS THE ROAD IT IS ALREADY DRAWING. Two clicks make one segment; the third
 * click's guide must be square to THAT segment, which is design section 3's Extending row
 * applied to the tool it was written for.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadAnchorExtendsTheSegmentBehindItTest,
	"Airside.Tool.RoadAnchorExtendsTheSegmentBehindIt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadAnchorExtendsTheSegmentBehindItTest::RunTest(const FString& Parameters)
{
	FRoadGesture Gesture;
	if (!TestTrue(TEXT("a taxiway gesture"), StartRoadGesture(Gesture))) { return false; }

	// NOTHING PENDING YET: the first click has no segment behind it to extend, and the network
	// does not exist until something is placed - which is the null case the hook must decline.
	FGuideAnchor Idle;
	TestFalse(TEXT("an idle road tool offers no anchor"),
		Gesture.Tool->DescribeGuideAnchor(Gesture.Network(), Idle));

	// Two clicks, west to east: one segment, and the chain now pends at its east end.
	Gesture.Tool->OnClick(Gesture.At(FVector2D(0.0, 0.0)));
	Gesture.Tool->OnClick(Gesture.At(FVector2D(6000.0, 0.0)));

	if (!TestTrue(TEXT("the clicks built a network"), Gesture.Network() != nullptr))
	{
		return false;
	}

	FGuideAnchor Anchor;
	if (!TestTrue(TEXT("with a segment behind it, the tool offers an anchor"),
		Gesture.Tool->DescribeGuideAnchor(Gesture.Network(), Anchor)))
	{
		return false;
	}

	TestTrue(TEXT("the anchor swings around the node the chain is drawing from"),
		Anchor.Origin.Equals(FVector2D(6000.0, 0.0), 1.0));

	// ALONG THE SEGMENT, either sign: a guide is a line, so what matters is the axis.
	TestTrue(TEXT("and extends the segment already arriving there"),
		FMath::IsNearlyEqual(FMath::Abs(Anchor.Reference.X), 1.0, 1.0e-6));
	TestTrue(TEXT("with the dashed line pointing back down that segment"),
		Anchor.ReferenceAt.Equals(FVector2D(0.0, 0.0), 1.0));
	TestEqual(TEXT("named as the road it is extending"),
		Anchor.ReferenceName, FString(TEXT("this road")));

	// THE OTHER END IS SOMETHING TO LINE UP WITH, and the node being extended FROM is not -
	// its own lines pass through the origin, so it would always be in tolerance and the guide
	// would be telling the player they are level with themselves.
	TestTrue(TEXT("the far node is offered as an alignment point"),
		Anchor.AlignTo.ContainsByPredicate([](const FGuidePoint& P)
			{ return P.At.Equals(FVector2D(0.0, 0.0), 1.0); }));
	TestFalse(TEXT("and the node being drawn from is not"),
		Anchor.AlignTo.ContainsByPredicate([](const FGuidePoint& P)
			{ return P.At.Equals(FVector2D(6000.0, 0.0), 1.0); }));

	return true;
}

#endif
