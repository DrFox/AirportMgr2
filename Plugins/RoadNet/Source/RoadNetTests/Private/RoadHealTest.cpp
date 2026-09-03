#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadHeal.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadHealTest,
	"RoadNet.Tool.Heal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadHealTest::RunTest(const FString& Parameters)
{
	URoadProfile* Profile = URoadProfile::MakeTransient(200.0, 100.0, 20.0);

	FRoadPlacementLimits Limits;
	Limits.MinSegmentLength = 250.0;
	Limits.MinTurnDegrees = 25.0;

	// The shape from the bug report: a junction at Three with two leaves running off to the
	// west, and a road east to Four, which is itself a junction with two more roads.
	// Deleting Three must leave One and Two rejoined to Four rather than stranded.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId One = Net->AddNode(FVector2D(-4000.0, 1000.0));
		// Two approaches Four from the SOUTH-west rather than beside One.
		//
		// It used to sit at (-1500, 3000), which put both rejoins arriving at Four only
		// 16.2 degrees apart - a hairpin, built by heal itself and never measured, because
		// the corner a rejoin makes at the ANCHOR went unchecked until TooSharpAtEnd. The
		// fixture was quietly asserting that heal may do the very thing MinTurnDegrees
		// exists to forbid. The refusal is asserted on its own terms further down.
		const FRoadNodeId Two = Net->AddNode(FVector2D(0.0, -5000.0));
		const FRoadNodeId Three = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId Four = Net->AddNode(FVector2D(5000.0, 1500.0));
		const FRoadNodeId North = Net->AddNode(FVector2D(4500.0, 4000.0));
		const FRoadNodeId East = Net->AddNode(FVector2D(9000.0, 2500.0));

		Net->AddStraightSegment(One, Three, Profile);
		Net->AddStraightSegment(Two, Three, Profile);
		Net->AddStraightSegment(Three, Four, Profile);
		Net->AddStraightSegment(Four, North, Profile);
		Net->AddStraightSegment(Four, East, Profile);

		const FRoadDeletionPlan Plan = RoadHeal::PlanNodeDeletion(*Net, Three, Limits);

		TestTrue(TEXT("the junction can be deleted"), Plan.bValid);
		TestTrue(TEXT("the anchor is the neighbour that keeps its roads"), Plan.Anchor == Four);
		TestEqual(TEXT("both stranded leaves rejoin"), Plan.Rejoin.Num(), 2);
		TestTrue(TEXT("the first leaf rejoins"), Plan.Rejoin.Contains(One));
		TestTrue(TEXT("the second leaf rejoins"), Plan.Rejoin.Contains(Two));
		TestEqual(TEXT("all three of its roads go"), Plan.Doomed.Num(), 3);
		TestEqual(TEXT("and nothing is left to sweep"), Plan.Swept.Num(), 0);

		// The anchor is not itself rejoined to anything - it keeps the roads it had.
		TestFalse(TEXT("the anchor is not rejoined to itself"), Plan.Rejoin.Contains(Four));
	}

	// Heal REFUSES when the rejoin it would invent is itself a hairpin at the anchor.
	//
	// Two leaves approaching the anchor from nearly the same bearing is the case the
	// fixture above used to contain by accident. Healing is not a licence to build a
	// junction the build tool would have rejected, and Plan.Refusal already exists to say
	// so - it simply had no way to learn about a corner at the far end.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId One = Net->AddNode(FVector2D(-4000.0, 1000.0));
		const FRoadNodeId Two = Net->AddNode(FVector2D(-1500.0, 3000.0));
		const FRoadNodeId Three = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId Four = Net->AddNode(FVector2D(5000.0, 1500.0));
		const FRoadNodeId North = Net->AddNode(FVector2D(4500.0, 4000.0));
		const FRoadNodeId East = Net->AddNode(FVector2D(9000.0, 2500.0));

		Net->AddStraightSegment(One, Three, Profile);
		Net->AddStraightSegment(Two, Three, Profile);
		Net->AddStraightSegment(Three, Four, Profile);
		Net->AddStraightSegment(Four, North, Profile);
		Net->AddStraightSegment(Four, East, Profile);

		const FRoadDeletionPlan Plan = RoadHeal::PlanNodeDeletion(*Net, Three, Limits);

		TestFalse(TEXT("a heal that would build a 16 degree corner is refused"), Plan.bValid);
		TestTrue(TEXT("and says the far end is what refused it"),
			Plan.Refusal == ERoadPlacement::TooSharpAtEnd);
		TestEqual(TEXT("a refused plan rejoins nothing"), Plan.Rejoin.Num(), 0);
	}

	// Degree 2 heals even when NEITHER end would be stranded. Removing a point from the
	// middle of a road has to leave the road connected; the two junctions either side
	// would both survive on their own, and a rule that only rejoined orphans would leave
	// two dead ends facing each other.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId WestEnd = Net->AddNode(FVector2D(-9000.0, 0.0));
		const FRoadNodeId West = Net->AddNode(FVector2D(-5000.0, 0.0));
		const FRoadNodeId Middle = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId East = Net->AddNode(FVector2D(5000.0, 0.0));
		const FRoadNodeId EastEnd = Net->AddNode(FVector2D(9000.0, 0.0));

		Net->AddStraightSegment(WestEnd, West, Profile);
		Net->AddStraightSegment(West, Middle, Profile);
		Net->AddStraightSegment(Middle, East, Profile);
		Net->AddStraightSegment(East, EastEnd, Profile);

		const FRoadDeletionPlan Plan = RoadHeal::PlanNodeDeletion(*Net, Middle, Limits);

		TestTrue(TEXT("a mid-road node can be deleted"), Plan.bValid);
		TestEqual(TEXT("its two neighbours are rejoined into one road"), Plan.Rejoin.Num(), 1);
		// Guarded on the count, not just on the anchor: an empty Rejoin indexed at [0] takes
		// the whole suite down with an access violation instead of failing one assertion.
		TestTrue(TEXT("the road is healed rather than left as two dead ends"),
			Plan.Anchor.IsSet() && Plan.Rejoin.Num() == 1
			&& (Plan.Rejoin[0] == West || Plan.Rejoin[0] == East));
		TestEqual(TEXT("nothing is swept"), Plan.Swept.Num(), 0);
	}

	// At degree 3 or more, a neighbour that keeps roads of its own is NOT rejoined -
	// that would invent a road nobody drew.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId Hub = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId Leaf = Net->AddNode(FVector2D(-5000.0, 500.0));
		const FRoadNodeId LeftJunction = Net->AddNode(FVector2D(0.0, 6000.0));
		const FRoadNodeId LeftTail = Net->AddNode(FVector2D(0.0, 11000.0));
		const FRoadNodeId RightJunction = Net->AddNode(FVector2D(6000.0, -1000.0));
		const FRoadNodeId RightTail = Net->AddNode(FVector2D(11000.0, -2000.0));

		Net->AddStraightSegment(Hub, Leaf, Profile);
		Net->AddStraightSegment(Hub, LeftJunction, Profile);
		Net->AddStraightSegment(Hub, RightJunction, Profile);
		Net->AddStraightSegment(LeftJunction, LeftTail, Profile);
		Net->AddStraightSegment(RightJunction, RightTail, Profile);

		const FRoadDeletionPlan Plan = RoadHeal::PlanNodeDeletion(*Net, Hub, Limits);

		TestTrue(TEXT("the hub can be deleted"), Plan.bValid);
		TestEqual(TEXT("only the stranded leaf is rejoined"), Plan.Rejoin.Num(), 1);
		TestTrue(TEXT("and it is the leaf, not a junction"),
			Plan.Rejoin.Num() == 1 && Plan.Rejoin[0] == Leaf);
		TestFalse(TEXT("the other junction gets no invented road"),
			Plan.Rejoin.Contains(LeftJunction) || Plan.Rejoin.Contains(RightJunction));
	}

	// A leaf node has nothing to rejoin to. Its neighbour goes with it if that leaves the
	// neighbour holding no road, and stays if it does not.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId Tip = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId Lone = Net->AddNode(FVector2D(4000.0, 0.0));
		Net->AddStraightSegment(Tip, Lone, Profile);

		const FRoadDeletionPlan Plan = RoadHeal::PlanNodeDeletion(*Net, Tip, Limits);
		TestTrue(TEXT("a leaf can be deleted"), Plan.bValid);
		TestEqual(TEXT("nothing to rejoin"), Plan.Rejoin.Num(), 0);
		TestEqual(TEXT("its now-bare neighbour is swept"), Plan.Swept.Num(), 1);
		TestTrue(TEXT("and it is that neighbour"),
			Plan.Swept.Num() == 1 && Plan.Swept[0] == Lone);

		const FRoadNodeId Onward = Net->AddNode(FVector2D(8000.0, 0.0));
		Net->AddStraightSegment(Lone, Onward, Profile);

		const FRoadDeletionPlan Kept = RoadHeal::PlanNodeDeletion(*Net, Tip, Limits);
		TestEqual(TEXT("a neighbour that keeps a road is not swept"), Kept.Swept.Num(), 0);
	}

	// The refusal, and it is judged at the REJOINING end - see RoadHeal.h for why the anchor
	// is deliberately not also checked. Two neighbours closer together than the minimum
	// segment length cannot be rejoined into a road the solver can trim, so the WHOLE
	// deletion is refused rather than half-applied or quietly dropping one of them.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId Apex = Net->AddNode(FVector2D(0.0, 3000.0));
		const FRoadNodeId Left = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId Right = Net->AddNode(FVector2D(200.0, 0.0));

		Net->AddStraightSegment(Left, Apex, Profile);
		Net->AddStraightSegment(Apex, Right, Profile);

		// 200 uu apart against a 250 uu minimum.
		const FRoadDeletionPlan Plan = RoadHeal::PlanNodeDeletion(*Net, Apex, Limits);

		TestFalse(TEXT("a deletion that cannot rejoin legally is refused"), Plan.bValid);
		TestTrue(TEXT("with the rule that stopped it"), Plan.Refusal == ERoadPlacement::TooShort);
		TestTrue(TEXT("and it names the neighbour it could not rejoin"),
			Plan.RefusedNeighbour == Left || Plan.RefusedNeighbour == Right);
		TestEqual(TEXT("a refused plan proposes no rejoins at all"), Plan.Rejoin.Num(), 0);
	}

	// A rejoin that would duplicate a road already there is not a refusal - it is simply
	// nothing to do, because the neighbour is not stranded after all.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId Corner = Net->AddNode(FVector2D(0.0, 4000.0));
		const FRoadNodeId Base = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId Side = Net->AddNode(FVector2D(6000.0, 0.0));

		Net->AddStraightSegment(Corner, Base, Profile);
		Net->AddStraightSegment(Corner, Side, Profile);
		Net->AddStraightSegment(Base, Side, Profile);

		const FRoadDeletionPlan Plan = RoadHeal::PlanNodeDeletion(*Net, Corner, Limits);
		TestTrue(TEXT("a triangle corner deletes"), Plan.bValid);
		TestEqual(TEXT("no rejoin is proposed for a pair already joined"), Plan.Rejoin.Num(), 0);
		TestEqual(TEXT("and neither survivor is swept - both keep the base road"),
			Plan.Swept.Num(), 0);
	}

	// Judged against the graph as it WILL be, not as it is. This is the trap that would
	// make every heal refuse: the arm the rejoin is judged at is the one about to be
	// deleted, and it points almost along the replacement. A straight three-node road is
	// the purest case - deleting the middle must heal, and would read as a zero-degree
	// hairpin if judged against the live graph.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId Start = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId Middle = Net->AddNode(FVector2D(4000.0, 0.0));
		const FRoadNodeId Finish = Net->AddNode(FVector2D(8000.0, 0.0));
		Net->AddStraightSegment(Start, Middle, Profile);
		Net->AddStraightSegment(Middle, Finish, Profile);

		const FRoadDeletionPlan Plan = RoadHeal::PlanNodeDeletion(*Net, Middle, Limits);
		TestTrue(TEXT("a collinear mid-node heals rather than refusing itself"), Plan.bValid);
		TestEqual(TEXT("into exactly one road"), Plan.Rejoin.Num(), 1);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
