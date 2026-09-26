#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/BuildSession.h"
#include "Tool/EditTool.h"
#include "Tool/RoadDrawTool.h"
#include "Tool/RoadGuideAnchor.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * ISSUE #303: FRoadDrawTool's chaining anchor and FEditTool's drag anchor used to answer "the
 * one arm arriving at this node" two ways - FEditTool through FRoadNode::Incident directly,
 * FRoadDrawTool by walking every live segment in the network on every MakeContext. This tests
 * RoadGuideAnchor::DescribeIncomingArm - the one function both now call - directly against the
 * graph, at each of the arm counts the two hand-written answers had to agree about by
 * construction rather than by test.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDescribeIncomingArmTest,
	"Airside.Tool.DescribeIncomingArm",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDescribeIncomingArmTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	// ONE ARM: A-B. B's one incident segment is unambiguous - the answer is the direction
	// from B back to A.
	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(6000.0, 0.0));
	Actor->ConnectNodes(A, B);

	const URoadNetwork* Network = Actor->GetNetwork();
	if (!TestNotNull(TEXT("a network"), Network)) { return false; }

	const FRoadNodeId BId = Network->NodeIdAt(B);
	const FRoadNode* BNode = Network->GetNode(BId);
	if (!TestNotNull(TEXT("node B"), BNode)) { return false; }
	if (!TestEqual(TEXT("B starts with exactly one arm"), BNode->Incident.Num(), 1)) { return false; }

	FGuideAnchor OneArm;
	RoadGuideAnchor::DescribeIncomingArm(*Network, *BNode, BId, OneArm);
	TestTrue(TEXT("one arm gives a direction to hold, pointing back down it"),
		FMath::IsNearlyEqual(FMath::Abs(OneArm.Reference.X), 1.0, 1.0e-6));
	TestTrue(TEXT("the dashed line runs to the far end of that one arm"),
		OneArm.ReferenceAt.Equals(FVector2D(0.0, 0.0), 1.0));
	TestEqual(TEXT("named as the road it is extending"),
		OneArm.ReferenceName, FString(TEXT("this road")));

	// TWO ARMS: connect a second road onto B. No arm is "the" one now, so DescribeIncomingArm
	// must leave Out exactly as it found it - the rule both tools' own comments name.
	const int32 C = Actor->PlaceNode(FVector2D(6000.0, -6000.0));
	Actor->ConnectNodes(B, C);
	Network = Actor->GetNetwork();
	BNode = Network->GetNode(BId);
	if (!TestNotNull(TEXT("node B after a second arm"), BNode)) { return false; }
	if (!TestEqual(TEXT("B now has two arms"), BNode->Incident.Num(), 2)) { return false; }

	FGuideAnchor TwoArms;
	RoadGuideAnchor::DescribeIncomingArm(*Network, *BNode, BId, TwoArms);
	TestTrue(TEXT("two arms offer no reference - picking one would be an arbitrary guess"),
		TwoArms.Reference.IsNearlyZero());
	TestTrue(TEXT("nor a name for one"), TwoArms.ReferenceName.IsEmpty());

	// THREE ARMS: a third road onto B. Still no reference - the "more than one" branch, not
	// just the "exactly two" one.
	const int32 D = Actor->PlaceNode(FVector2D(-6000.0, -6000.0));
	Actor->ConnectNodes(B, D);
	Network = Actor->GetNetwork();
	BNode = Network->GetNode(BId);
	if (!TestNotNull(TEXT("node B after a third arm"), BNode)) { return false; }
	if (!TestEqual(TEXT("B now has three arms"), BNode->Incident.Num(), 3)) { return false; }

	FGuideAnchor ThreeArms;
	RoadGuideAnchor::DescribeIncomingArm(*Network, *BNode, BId, ThreeArms);
	TestTrue(TEXT("three arms offer no reference either"), ThreeArms.Reference.IsNearlyZero());

	return true;
}

/**
 * THE SAME ANSWER FROM BOTH TOOLS, through the one function - the parity issue #303 asks for
 * directly, rather than trusting that two independent implementations of the same rule cannot
 * drift. FRoadDrawTool resumes a chain from an existing node; FEditTool drags it - different
 * gestures, but DescribeIncomingArm can only see the node, and must not care which tool asked.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDescribeIncomingArmAgreesAcrossToolsTest,
	"Airside.Tool.DescribeIncomingArmAgreesAcrossTools",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDescribeIncomingArmAgreesAcrossToolsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	// A single road A-B: B has exactly one arm, the case both tools offer a reference for.
	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(6000.0, 0.0));
	Actor->ConnectNodes(A, B);

	FBuildSessionTunables Tunables = Actor->MakeTunables(10000.0);

	// FRoadDrawTool: click B to resume a chain from it. A snap onto the existing node - not a
	// new one - is what sets GetPendingNode() to B without touching the graph.
	FBuildSession DrawSession;
	DrawSession.SelectTool(1); // Taxiway, by registry index
	IBuildTool* DrawTool = DrawSession.GetActiveTool();
	if (!TestNotNull(TEXT("a taxiway tool"), DrawTool)) { return false; }
	DrawTool->OnClick(DrawSession.MakeContext(Actor, FVector2D(6000.0, 0.0), Tunables, false, false));

	FGuideAnchor DrawAnchor;
	if (!TestTrue(TEXT("the draw tool resumes from B and offers an anchor"),
		DrawTool->DescribeGuideAnchor(Actor->GetNetwork(), Actor, DrawAnchor)))
	{
		return false;
	}

	// FEditTool: grab B directly. The snap chain finds it because the cursor sits exactly on it.
	FBuildSession EditSession;
	EditSession.SelectTool(1); // Taxiway lights AirsideNode handles
	EditSession.SetGestureMode(EGestureMode::Edit);
	IBuildTool* EditTool = EditSession.GetActiveTool();
	if (!TestNotNull(TEXT("an edit tool"), EditTool)) { return false; }
	EditTool->OnDragBegin(EditSession.MakeContext(Actor, FVector2D(6000.0, 0.0), Tunables, false, false));
	if (!TestFalse(TEXT("B was really grabbed"), EditTool->IsIdle())) { return false; }

	FGuideAnchor EditAnchor;
	if (!TestTrue(TEXT("the edit tool holds B and offers an anchor"),
		EditTool->DescribeGuideAnchor(Actor->GetNetwork(), Actor, EditAnchor)))
	{
		return false;
	}

	TestTrue(TEXT("both tools extend the SAME one arm at B, through the one function"),
		DrawAnchor.Reference.Equals(EditAnchor.Reference, 1.0e-6));
	TestTrue(TEXT("to the same far point"),
		DrawAnchor.ReferenceAt.Equals(EditAnchor.ReferenceAt, 1.0e-6));
	TestEqual(TEXT("named the same way"), DrawAnchor.ReferenceName, EditAnchor.ReferenceName);

	EditTool->OnDragEnd(EditSession.MakeContext(Actor, FVector2D(6000.0, 0.0), Tunables, false, false));

	// NOW GIVE B A SECOND ARM. Neither tool has "the" one to extend any more, and neither
	// must invent one - the case a per-tool implementation is most likely to answer
	// differently, because there is no obviously-right single answer to converge on by luck.
	const int32 C = Actor->PlaceNode(FVector2D(6000.0, -6000.0));
	Actor->ConnectNodes(B, C);

	FBuildSession DrawSession2;
	DrawSession2.SelectTool(1);
	IBuildTool* DrawTool2 = DrawSession2.GetActiveTool();
	DrawTool2->OnClick(DrawSession2.MakeContext(Actor, FVector2D(6000.0, 0.0), Tunables, false, false));
	FGuideAnchor DrawAnchorTwoArms;
	DrawTool2->DescribeGuideAnchor(Actor->GetNetwork(), Actor, DrawAnchorTwoArms);

	FBuildSession EditSession2;
	EditSession2.SelectTool(1);
	EditSession2.SetGestureMode(EGestureMode::Edit);
	IBuildTool* EditTool2 = EditSession2.GetActiveTool();
	EditTool2->OnDragBegin(EditSession2.MakeContext(Actor, FVector2D(6000.0, 0.0), Tunables, false, false));
	FGuideAnchor EditAnchorTwoArms;
	EditTool2->DescribeGuideAnchor(Actor->GetNetwork(), Actor, EditAnchorTwoArms);

	TestTrue(TEXT("with two arms, the draw tool offers no reference direction"),
		DrawAnchorTwoArms.Reference.IsNearlyZero());
	TestTrue(TEXT("and neither does the edit tool - the same answer, agreeing on 'none'"),
		EditAnchorTwoArms.Reference.IsNearlyZero());

	EditTool2->OnDragEnd(EditSession2.MakeContext(Actor, FVector2D(6000.0, 0.0), Tunables, false, false));
	return true;
}

#endif
