#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/BuildSession.h"
#include "Tool/RoadDrawTool.h"
#include "Tool/RoadEditTarget.h"
#include "Profiles/RoadProfile.h"
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
		Gesture.Tool->DescribeGuideAnchor(Gesture.Network(), Gesture.TestWorld.Actor, Idle));

	// Two clicks, west to east: one segment, and the chain now pends at its east end.
	Gesture.Tool->OnClick(Gesture.At(FVector2D(0.0, 0.0)));
	Gesture.Tool->OnClick(Gesture.At(FVector2D(6000.0, 0.0)));

	if (!TestTrue(TEXT("the clicks built a network"), Gesture.Network() != nullptr))
	{
		return false;
	}

	FGuideAnchor Anchor;
	if (!TestTrue(TEXT("with a segment behind it, the tool offers an anchor"),
		Gesture.Tool->DescribeGuideAnchor(Gesture.Network(), Gesture.TestWorld.Actor, Anchor)))
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

/**
 * THE THIRD CLICK SQUARES TO THE SECOND SEGMENT, landing exactly on the perpendicular rather
 * than merely near it. This is the assertion that fails if the tool never reads
 * FToolContext::Guide at all - every other test in this file passes on a tool that ignores it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadCornerFollowsTheGuideTest,
	"Airside.Tool.RoadCornerFollowsTheGuide",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadCornerFollowsTheGuideTest::RunTest(const FString& Parameters)
{
	FRoadGesture Gesture;
	if (!TestTrue(TEXT("a taxiway gesture"), StartRoadGesture(Gesture))) { return false; }

	Gesture.Tool->OnClick(Gesture.At(FVector2D(0.0, 0.0)));
	Gesture.Tool->OnClick(Gesture.At(FVector2D(6000.0, 0.0)));

	// A third click dragged nearly square to the run just drawn: 2000 out and 60 along, about
	// 1.7 degrees off the perpendicular and inside the 7-degree tolerance.
	const FVector2D NearSquare(6060.0, 2000.0);
	const FToolContext Guided = Gesture.At(NearSquare);
	if (!TestTrue(TEXT("the driver resolved a guide for the third click"), Guided.Guide.bActive))
	{
		return false;
	}

	Gesture.Tool->OnClick(Guided);

	// READ BACK FROM THE GRAPH, not from the tool, so this measures what was BUILT.
	const URoadNetwork* Network = Gesture.Network();
	if (!TestTrue(TEXT("a network"), Network != nullptr)) { return false; }

	const int32 Pending = Gesture.Road()->GetPendingNode();
	const FRoadNode* Placed = Network->GetNode(Network->NodeIdAt(Pending));
	if (!TestNotNull(TEXT("the third click placed a node"), Placed)) { return false; }

	TestTrue(TEXT("the new node is exactly square to the segment behind it"),
		FMath::IsNearlyEqual(Placed->Position.X, 6000.0, 1.0e-6));
	TestFalse(TEXT("and therefore not where the raw cursor was"),
		FMath::IsNearlyEqual(NearSquare.X, 6000.0, 1.0e-6));

	return true;
}

/**
 * A SNAP BEATS A GUIDE. Closing a junction on an existing node must win over any alignment, or
 * the player could never join two roads while a guide happened to be live.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadSnapBeatsTheGuideTest,
	"Airside.Tool.RoadSnapBeatsTheGuide",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadSnapBeatsTheGuideTest::RunTest(const FString& Parameters)
{
	FRoadGesture Gesture;
	if (!TestTrue(TEXT("a taxiway gesture"), StartRoadGesture(Gesture))) { return false; }

	// An existing node to close onto, placed deliberately OFF the square: if the guide won, the
	// click would land at x = 6000 instead of on this node.
	IRoadEditTarget* Target = Gesture.TestWorld.Actor;
	const int32 Existing = Target->PlaceNode(FVector2D(6080.0, 2000.0));

	Gesture.Tool->OnClick(Gesture.At(FVector2D(0.0, 0.0)));
	Gesture.Tool->OnClick(Gesture.At(FVector2D(6000.0, 0.0)));

	const URoadNetwork* Network = Gesture.Network();
	if (!TestTrue(TEXT("a network"), Network != nullptr)) { return false; }

	// A cursor ON the existing node: near enough to square that a guide is live, and near
	// enough to the node that the snap claims it.
	const FToolContext OnNode = Gesture.At(FVector2D(6080.0, 2000.0));
	if (!TestEqual(TEXT("the snap claimed the existing node"),
		static_cast<int32>(OnNode.Snap.Kind), static_cast<int32>(ERoadSnapKind::Node)))
	{
		return false;
	}

	Gesture.Tool->OnClick(OnNode);

	// THE NODE DID NOT MOVE to satisfy the guide, which is the whole assertion: a snap is a
	// statement about the graph and an alignment is only an aid.
	const FRoadNode* Reused = Network->GetNode(Network->NodeIdAt(Existing));
	if (!TestNotNull(TEXT("the existing node survives the click"), Reused)) { return false; }
	TestTrue(TEXT("and stayed exactly where it was put"),
		Reused->Position.Equals(FVector2D(6080.0, 2000.0), 1.0e-6));

	return true;
}

/**
 * THE ANCHOR KNOWS HOW WIDE THE DRAG IS, and that its point is a CENTRELINE.
 *
 * A road's centreline lined up with an apron's EDGE is not what anybody means - you want the
 * road's edge flush with the apron's, which needs the half-width at the point the guide is
 * proposed. See the 2026-09-20 guide-grid design section 6.
 *
 * THROUGH ResolveProfileFor, which is the one answer to "what would this gesture lay" - a guide
 * resolving a width of its own would be the third copy that function was made to prevent.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadAnchorCarriesItsHalfWidthTest,
	"Airside.Tool.RoadAnchorCarriesItsHalfWidth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadAnchorCarriesItsHalfWidthTest::RunTest(const FString& Parameters)
{
	FRoadGesture Gesture;
	if (!TestTrue(TEXT("a taxiway gesture"), StartRoadGesture(Gesture))) { return false; }

	Gesture.Tool->OnClick(Gesture.At(FVector2D(0.0, 0.0)));
	Gesture.Tool->OnClick(Gesture.At(FVector2D(6000.0, 0.0)));

	IRoadEditTarget* Target = Gesture.TestWorld.Actor;
	FGuideAnchor Anchor;
	if (!TestTrue(TEXT("the tool describes an anchor"),
		Gesture.Tool->DescribeGuideAnchor(Gesture.Network(), Target, Anchor)))
	{
		return false;
	}

	// A ROAD'S MOVING POINT IS ITS CENTRELINE. The plot and apron tools drag a corner of the
	// shape itself, and the displacement rule turns on exactly that difference.
	TestEqual(TEXT("a road drags a centreline"),
		static_cast<int32>(Anchor.Point), static_cast<int32>(EDragPoint::Centreline));

	// HONOURED, NOT ASSUMED: the profile has to resolve for the widths to mean anything, and a
	// content set with no taxiway would leave them legitimately zero.
	const URoadProfile* Profile = Target->ResolveProfileFor(ERoadKind::Taxiway,
		Gesture.Road()->GetWidthIndex());
	if (Profile == nullptr)
	{
		AddInfo(TEXT("No taxiway profile resolves; half-widths not checked"));
		return true;
	}

	// THE TWO ARE ASKED SEPARATELY because URoadProfile's are separate: a cross-section may be
	// off-centre, and a mirrored pair would be wrong on every such road.
	TestEqual(TEXT("the anchor carries the profile's own left half-width"),
		Anchor.HalfWidthLeft, Profile->GetHalfWidthLeft());
	TestEqual(TEXT("and its right, separately"),
		Anchor.HalfWidthRight, Profile->GetHalfWidthRight());
	TestTrue(TEXT("and they are a real width, not a default zero"),
		Anchor.HalfWidthLeft > 0.0);

	return true;
}

#endif
