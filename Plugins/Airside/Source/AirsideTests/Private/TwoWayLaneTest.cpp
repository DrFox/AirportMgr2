#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Build/AnchorLink.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

// TWO-WAY ROADS (spec 2026-09-23 §2-§5). Every lateral assertion here is in WORLD
// coordinates, because "right" is the thing most likely to be got backwards: the model's
// "left" is PerpCCW of the tangent, which for travel along +X is +Y - and UE is left-handed,
// so seen from above +Y is to the RIGHT of +X. Right-hand traffic along +X therefore drives
// at Y > 0.

namespace TwoWayLane
{
	void Derive(URoadNetwork& Net)
	{
		// #311: this WAS its own SolveAll+Build pair, the exact shape TestGraph::Derive
		// now holds once; kept as a local wrapper because every call site in this file
		// says the bare, unqualified `Derive(Net)`.
		TestGraph::Derive(Net);
	}

	/** The one live segment edge of Seg running Dir. Null if there is not exactly one. */
	const FGuidelineEdge* Lane(const URoadNetwork& Net, FRoadSegmentId Seg, EGuidelineDir Dir)
	{
		const FGuidelineEdge* Found = nullptr;
		for (const FGuidelineEdge& Edge : Net.GetGuidelineEdges())
		{
			if (Edge.bAlive && Edge.DerivedFrom == Seg && Edge.Direction == Dir)
			{
				if (Found != nullptr)
				{
					return nullptr;
				}
				Found = &Edge;
			}
		}
		return Found;
	}

	FVector2D Pos(const URoadNetwork& Net, FGuidelineNodeId Node)
	{
		const FGuidelineNode* Found = Net.GetGuidelineNode(Node);
		return Found != nullptr ? Found->Position : FVector2D(NAN, NAN);
	}

	// #312: was a hand-built FRouteQuery that skipped AvoidRunways - see TestGraph::Probe's
	// own comment for why that silently answered every errand with the permissive policy.
	FRoutePlan Route(const URoadNetwork& Net, FGuidelineNodeId Start, FGuidelineNodeId Goal)
	{
		return TestGraph::Probe(Net, Start, Goal, ETraversalClass::GroundVehicle);
	}

	/** A one-guideline, bidirectional vehicle road - the shape every service road had before
	 *  2026-09-23, and what a hand-made profile may still be. */
	URoadProfile* OneLaneProfile(double Width)
	{
		URoadProfile* Profile = NewObject<URoadProfile>(GetTransientPackage());
		FProfileBand Band;
		Band.Width = Width;
		Band.Type = ERoadBandType::Lane;
		Profile->Bands.Add(Band);
		FProfileGuideline Line;
		Line.Class = ETraversalClass::GroundVehicle;
		Line.Direction = EGuidelineDir::Bidirectional;
		Line.Width = Width;
		Profile->Guidelines.Add(Line);
		Profile->ExitLength = 0.0;
		return Profile;
	}

	/**
	 * Right-of-travel check along a polyline, in world terms (see the file comment): for
	 * each span outside Box of the origin, the span's midpoint must lie on the Sign side of
	 * the ROAD it is on. Roads here run along the axes through the origin, so the road's own
	 * centreline is X = 0 or Y = 0 and the lateral offset is just the other coordinate.
	 * Returns how many spans were judged, and fills Bad with the count on the wrong side.
	 */
	int32 JudgeSide(const TArray<FVector2D>& Poly, double Box, double Sign, int32& Bad)
	{
		int32 Judged = 0;
		Bad = 0;
		for (int32 I = 1; I < Poly.Num(); ++I)
		{
			const FVector2D D = Poly[I] - Poly[I - 1];
			const FVector2D M = (Poly[I] + Poly[I - 1]) * 0.5;
			if (D.Size() < 1.0 || (FMath::Abs(M.X) < Box && FMath::Abs(M.Y) < Box))
			{
				continue;
			}
			// Screen-right of D in UE's left-handed frame is PerpCCW(D) = (-D.Y, D.X).
			const FVector2D ScreenRight(-D.Y, D.X);
			// Lateral position relative to the road's centreline: the road along X has
			// centreline Y = 0, the road along Y has centreline X = 0.
			const FVector2D Lateral = FMath::Abs(D.X) >= FMath::Abs(D.Y) ? FVector2D(0.0, M.Y) : FVector2D(M.X, 0.0);
			++Judged;
			if (FVector2D::DotProduct(Lateral, ScreenRight.GetSafeNormal()) * Sign <= 0.0)
			{
				++Bad;
			}
		}
		return Judged;
	}
}

using namespace TwoWayLane;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTwoWayLanesDerivedTest, "Airside.Build.TwoWay.LanesDerived",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTwoWayLanesDerivedTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadNodeId A = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId B = Net->AddNode(FVector2D(20000.0, 0.0));
	const FRoadSegmentId Seg = Net->AddStraightSegment(A, B, URoadProfile::MakeServiceRoadTransient());
	Derive(*Net);

	const FGuidelineEdge* Forward = Lane(*Net, Seg, EGuidelineDir::AToB);
	const FGuidelineEdge* Back = Lane(*Net, Seg, EGuidelineDir::BToA);
	if (!TestTrue(TEXT("one lane each way, both one-way"), Forward != nullptr && Back != nullptr)) { return false; }

	TestTrue(TEXT("right-hand traffic along +X keeps to +Y, screen-right in UE's left-handed frame"),
		Pos(*Net, Forward->A).Y > 0.0 && Pos(*Net, Forward->B).Y > 0.0);
	TestTrue(TEXT("and the lane back runs on the other side"),
		Pos(*Net, Back->A).Y < 0.0 && Pos(*Net, Back->B).Y < 0.0);
	TestEqual(TEXT("each lane centred in its own half of a 3 m + 3 m road"),
		FMath::Abs(Pos(*Net, Forward->A).Y), 150.0, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTwoWayRouteKeepsSideTest, "Airside.Build.TwoWay.RouteKeepsSide",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTwoWayRouteKeepsSideTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadNodeId A = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId B = Net->AddNode(FVector2D(20000.0, 0.0));
	const FRoadSegmentId Seg = Net->AddStraightSegment(A, B, URoadProfile::MakeServiceRoadTransient());

	for (const EDriveSide Side : { EDriveSide::Right, EDriveSide::Left })
	{
		Net->SetDriveSide(Side);
		Derive(*Net);
		const FGuidelineEdge* Forward = Lane(*Net, Seg, EGuidelineDir::AToB);
		if (!TestNotNull(TEXT("the A->B lane"), Forward)) { return false; }
		const FRoutePlan Plan = Route(*Net, Forward->A, Forward->B);
		if (!TestTrue(TEXT("a route down the road"), Plan.IsValid())) { return false; }

		int32 Bad = 0;
		const int32 Judged = JudgeSide(Plan.Polyline, 0.0, Side == EDriveSide::Right ? 1.0 : -1.0, Bad);
		TestTrue(TEXT("the route was judged at all"), Judged > 0);
		if (Side == EDriveSide::Right)
		{
			TestEqual(TEXT("right-hand traffic drives on the right of its travel"), Bad, 0);
		}
		else
		{
			TestEqual(TEXT("left-hand traffic drives on the left of it"), Bad, 0);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTwoWaySparedEdgeTest, "Airside.Build.TwoWay.SparedEdgeSurvivesFlip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTwoWaySparedEdgeTest::RunTest(const FString& Parameters)
{
	// Review focus 2: a hand-authored edge names its ends by (segment, end, guideline index),
	// which a flip does not change - so it must re-resolve, not be stranded.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadNodeId A = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId B = Net->AddNode(FVector2D(20000.0, 0.0));
	const FRoadSegmentId Seg = Net->AddStraightSegment(A, B, URoadProfile::MakeServiceRoadTransient());
	Derive(*Net);

	const FGuidelineEdge* Forward = Lane(*Net, Seg, EGuidelineDir::AToB);
	const FGuidelineEdge* Back = Lane(*Net, Seg, EGuidelineDir::BToA);
	if (!TestTrue(TEXT("two lanes"), Forward != nullptr && Back != nullptr)) { return false; }

	FGuidelineEdge Hand;
	Hand.A = Forward->B;
	Hand.B = Back->B;
	Hand.Control = (Pos(*Net, Hand.A) + Pos(*Net, Hand.B)) * 0.5;
	Hand.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
	Hand.Direction = EGuidelineDir::AToB;
	Hand.bDerived = false;
	Hand.EndRefA = Net->GetGuidelineNode(Hand.A)->Origin;
	Hand.EndRefB = Net->GetGuidelineNode(Hand.B)->Origin;
	const FGuidelineEdgeId HandId = Net->AddGuidelineEdge(MoveTemp(Hand));

	Net->SetDriveSide(EDriveSide::Left);
	Derive(*Net);

	const FGuidelineEdge* Survivor = Net->GetGuidelineEdge(HandId);
	if (!TestTrue(TEXT("the player's edge survives a flip"), Survivor != nullptr && Survivor->bAlive)) { return false; }
	const FGuidelineNode* EndA = Net->GetGuidelineNode(Survivor->A);
	const FGuidelineNode* EndB = Net->GetGuidelineNode(Survivor->B);
	TestTrue(TEXT("and still joins live nodes on the re-derived lanes"),
		EndA != nullptr && EndA->bAlive && EndA->Incident.Num() > 1
		&& EndB != nullptr && EndB->bAlive && EndB->Incident.Num() > 1);
	return true;
}

namespace TwoWayLane
{
	/** A hub at the origin with arms to the west, east and north, all added FROM the hub. */
	struct FTee
	{
		URoadNetwork* Net = nullptr;
		FRoadSegmentId West, East, North;
	};

	FTee Tee(URoadProfile* WestProfile)
	{
		FTee T;
		T.Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Two = URoadProfile::MakeServiceRoadTransient();
		const FRoadNodeId Hub = T.Net->AddNode(FVector2D(0.0, 0.0));
		T.West  = T.Net->AddStraightSegment(Hub, T.Net->AddNode(FVector2D(-20000.0, 0.0)), WestProfile ? WestProfile : Two);
		T.East  = T.Net->AddStraightSegment(Hub, T.Net->AddNode(FVector2D(20000.0, 0.0)), Two);
		T.North = T.Net->AddStraightSegment(Hub, T.Net->AddNode(FVector2D(0.0, 20000.0)), Two);
		Derive(*T.Net);
		return T;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTwoWayTJunctionTest, "Airside.Build.TwoWay.TJunctionTurns",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTwoWayTJunctionTest::RunTest(const FString& Parameters)
{
	const FTee T = Tee(nullptr);
	URoadNetwork& Net = *T.Net;

	// Arms were added FROM the hub, so on every arm B->A arrives at the hub and A->B leaves it.
	const FRoadSegmentId Arms[3] = { T.West, T.East, T.North };
	for (const FRoadSegmentId From : Arms)
	{
		for (const FRoadSegmentId To : Arms)
		{
			if (From == To) { continue; }
			const FGuidelineEdge* In = Lane(Net, From, EGuidelineDir::BToA);
			const FGuidelineEdge* Out = Lane(Net, To, EGuidelineDir::AToB);
			if (!TestTrue(TEXT("both arms have their lanes"), In != nullptr && Out != nullptr)) { return false; }
			int32 Turns = 0, Wrong = 0;
			for (const FGuidelineEdge& Edge : Net.GetGuidelineEdges())
			{
				if (!Edge.bAlive || Edge.DerivedFrom.IsSet()) { continue; }
				if (Edge.A == In->A && Edge.B == Out->A) { ++Turns; }
				// A turn out of the arriving lane may only land on a LEAVING lane.
				if (Edge.A == In->A)
				{
					const FGuidelineEdge* Landing = Lane(Net, To, EGuidelineDir::BToA);
					Wrong += (Landing != nullptr && Edge.B == Landing->A) ? 1 : 0;
				}
			}
			TestEqual(TEXT("exactly one turn from the arriving lane to the leaving lane"), Turns, 1);
			TestEqual(TEXT("and none onto a lane that runs back at the junction"), Wrong, 0);
		}
	}

	// A left turn and a right turn, each kept on the right outside the junction.
	for (const FRoadSegmentId To : { T.North, T.East })
	{
		const FGuidelineEdge* Start = Lane(Net, T.West, EGuidelineDir::BToA);
		const FGuidelineEdge* Goal = Lane(Net, To, EGuidelineDir::AToB);
		const FRoutePlan Plan = Route(Net, Start->B, Goal->B);
		if (!TestTrue(TEXT("the turn routes"), Plan.IsValid())) { return false; }
		int32 Bad = 0;
		TestTrue(TEXT("judged outside the junction"), JudgeSide(Plan.Polyline, 1500.0, 1.0, Bad) > 0);
		TestEqual(TEXT("on the right before and after the junction"), Bad, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTwoWayMixedLanesTest, "Airside.Build.TwoWay.MixedLaneCounts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTwoWayMixedLanesTest::RunTest(const FString& Parameters)
{
	// Review focus 1: a one-lane bidirectional road meeting two-lane roads. Pairing by index
	// dropped the second lane outright.
	const FTee T = Tee(OneLaneProfile(700.0));
	URoadNetwork& Net = *T.Net;

	const FGuidelineEdge* Old = Lane(Net, T.West, EGuidelineDir::Bidirectional);
	const FGuidelineEdge* EastOut = Lane(Net, T.East, EGuidelineDir::AToB);
	const FGuidelineEdge* EastIn = Lane(Net, T.East, EGuidelineDir::BToA);
	if (!TestTrue(TEXT("all three lanes exist"), Old && EastOut && EastIn)) { return false; }

	TestTrue(TEXT("from the one-lane road onto the two-lane one"), Route(Net, Old->B, EastOut->B).IsValid());
	TestTrue(TEXT("and back, which index pairing lost"), Route(Net, EastIn->B, Old->B).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTwoWayParallelLanesTest, "Airside.Build.TwoWay.ParallelLanes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTwoWayParallelLanesTest::RunTest(const FString& Parameters)
{
	// Review focus 5: straight through between lanes of different offset. The lane lines are
	// parallel, so there is no intersection to use as a control. WAS the chord midpoint - one
	// straight diagonal, kinked at both ends, and with both cuts on the node a 90 degree jog.
	// CHANGED BY RULING (2026-09-25, "inset nodes and a curve between the two widths"): the cuts
	// are inset and each lane crosses on an S, two pieces meeting mid-way. So each piece is
	// TANGENT TO ITS OWN LANE at its lane end - its control on that lane's line, ahead of it -
	// which is the property the chord never had. Airside.Build.WidthTaper.* measures the rest.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadNodeId W = Net->AddNode(FVector2D(-20000.0, 0.0));
	const FRoadNodeId Mid = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(20000.0, 0.0));
	Net->AddStraightSegment(W, Mid, URoadProfile::MakeServiceRoadTransient());
	Net->AddStraightSegment(Mid, E, URoadProfile::MakeServiceRoadTransient(500.0, 60.0));
	Derive(*Net);

	int32 Turns = 0;
	int32 Tangent = 0;
	for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
	{
		// Junction edges only: the dead-end balloons at the far ends carry no DerivedFrom either.
		if (!Edge.bAlive || Edge.DerivedFrom.IsSet() || FMath::Abs(Pos(*Net, Edge.A).X) > 5000.0) { continue; }
		++Turns;
		const FVector2D P = Pos(*Net, Edge.A);
		const FVector2D Q = Pos(*Net, Edge.B);
		const double Along = FVector2D::DotProduct(Edge.Control - P, (Q - P).GetSafeNormal());
		TestTrue(TEXT("the control is between the ends, never behind one - no loop back"),
			Along >= 0.0 && Along <= FVector2D::Distance(P, Q));
		// The lanes run along X: a piece leaving a lane end has its control at that end's Y, a
		// piece arriving at one at the far end's Y. Exactly one of the two holds per piece.
		const bool bLeavesTangent = FMath::Abs(Edge.Control.Y - P.Y) < 1e-6;
		const bool bArrivesTangent = FMath::Abs(Edge.Control.Y - Q.Y) < 1e-6;
		TestTrue(TEXT("each piece is tangent to the lane it leaves or joins - the S, not a chord"),
			bLeavesTangent != bArrivesTangent);
		Tangent += (bLeavesTangent != bArrivesTangent) ? 1 : 0;
	}
	TestEqual(TEXT("one S each way: two pieces per lane"), Turns, 4);
	TestEqual(TEXT("and every piece tangent to its lane"), Tangent, 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTwoWayTaxiwayControlTest, "Airside.Build.TwoWay.TaxiwayControlUnchanged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTwoWayTaxiwayControlTest::RunTest(const FString& Parameters)
{
	// Taxiway turns must not move: a centreline guideline's tangent lines meet exactly at the
	// node, and the control stays exactly - bitwise - the node.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0);
	const FRoadNodeId Hub = Net->AddNode(FVector2D(0.0, 0.0));
	Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(-30000.0, 0.0)), Taxiway);
	Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(30000.0, 0.0)), Taxiway);
	Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(0.0, 30000.0)), Taxiway);
	Derive(*Net);

	int32 Turns = 0;
	for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
	{
		if (!Edge.bAlive || Edge.DerivedFrom.IsSet()) { continue; }
		++Turns;
		TestTrue(TEXT("a taxiway turn's control is exactly the node"), Edge.Control == FVector2D(0.0, 0.0));
	}
	TestEqual(TEXT("six turns at a three-arm taxiway junction"), Turns, 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTwoWayDeadEndTest, "Airside.Build.TwoWay.DeadEnd",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTwoWayDeadEndTest::RunTest(const FString& Parameters)
{
	// Spec test 5 (the bowser half; the rig half is PR 3) and review focus 3: a 20 m stub,
	// shorter than the balloon, still turns a vehicle round.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadNodeId A = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId B = Net->AddNode(FVector2D(2000.0, 0.0));
	const FRoadSegmentId Seg = Net->AddStraightSegment(A, B, URoadProfile::MakeServiceRoadTransient());
	Derive(*Net);

	const FGuidelineEdge* Forward = Lane(*Net, Seg, EGuidelineDir::AToB);
	const FGuidelineEdge* Back = Lane(*Net, Seg, EGuidelineDir::BToA);
	if (!TestTrue(TEXT("two lanes"), Forward != nullptr && Back != nullptr)) { return false; }

	const FRoutePlan Plan = Route(*Net, Forward->A, Back->B);
	if (!TestTrue(TEXT("down the stub, round, and back: one-way lanes do not strand a vehicle"), Plan.IsValid()))
	{
		return false;
	}
	double Furthest = -1.0;
	for (const FVector2D& P : Plan.Polyline)
	{
		Furthest = FMath::Max(Furthest, P.X);
	}
	TestTrue(TEXT("the balloon lies past the road end, over grass by ruling"), Furthest > 2000.0 + 1000.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTwoWayAnchorBothLanesTest, "Airside.Build.TwoWay.AnchorBothLanes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTwoWayAnchorBothLanesTest::RunTest(const FString& Parameters)
{
	// Spec test 8 and review focus 4: a depot beside a two-lane road is reached from either
	// direction without driving to a dead end and back.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadNodeId A = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId B = Net->AddNode(FVector2D(20000.0, 0.0));
	const FRoadSegmentId Seg = Net->AddStraightSegment(A, B, URoadProfile::MakeServiceRoadTransient());

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	const FEntityInstanceId Placed = Net->PlaceEntity(Depot, Depot->Anchors,
		FVector2D(10000.0, 1000.0), UE_DOUBLE_PI * 0.5, /*DesignWingspan=*/0.0, Depot->PoseRole, Depot->Trucks);
	const FEntityInstance* Instance = Net->GetEntity(Placed);
	if (!TestNotNull(TEXT("the depot resolves"), Instance)) { return false; }
	const FGuidelineNodeId PoseNode = Instance->PoseNode;

	Derive(*Net);
	FAnchorLink::Build(*Net, UAirsideSettings::ResolveLargestServiceVehicle());

	// The lanes are split where the links join, so take their far ends by position.
	FGuidelineNodeId ForwardStart, BackStart;
	for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
	{
		if (!Edge.bAlive || Edge.DerivedFrom != Seg) { continue; }
		const FGuidelineNode* EndA = Net->GetGuidelineNode(Edge.A);
		const FGuidelineNode* EndB = Net->GetGuidelineNode(Edge.B);
		if (Edge.Direction == EGuidelineDir::AToB && EndA && EndA->Position.X < 1.0 + 1500.0 && EndA->Origin.IsSet())
		{
			ForwardStart = Edge.A;
		}
		if (Edge.Direction == EGuidelineDir::BToA && EndB && EndB->Position.X > 20000.0 - 1500.0 && EndB->Origin.IsSet())
		{
			BackStart = Edge.B;
		}
	}
	if (!TestTrue(TEXT("found both lanes' far ends"), ForwardStart.IsSet() && BackStart.IsSet())) { return false; }

	const FRoutePlan FromWest = Route(*Net, ForwardStart, PoseNode);
	const FRoutePlan FromEast = Route(*Net, BackStart, PoseNode);
	TestTrue(TEXT("reached travelling east"), FromWest.IsValid());
	TestTrue(TEXT("reached travelling west"), FromEast.IsValid());
	TestTrue(TEXT("travelling west is no detour round a dead end"),
		FromEast.IsValid() && FromEast.Length < 10000.0 + 2500.0);
	TestTrue(TEXT("travelling east is no detour either"),
		FromWest.IsValid() && FromWest.Length < 10000.0 + 2500.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTwoWaySiblingLaneDoesNotRescanEveryGuidelineTest,
	"Airside.Build.TwoWay.SiblingLaneDoesNotRescanEveryGuideline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTwoWaySiblingLaneDoesNotRescanEveryGuidelineTest::RunTest(const FString& Parameters)
{
	// #306: FindSiblingLane kept its OWN copy of FProximityLinkFinder::Find's loop, and the
	// copy never called CannotReachWithin - so the SECOND lane of every two-way road's anchor
	// link paid for URoadNetwork::SampleGuideline on every OTHER joinable edge in the airport,
	// the exact cost #177 spent a session removing from the FIRST lane's own search. Both
	// finders now share AnchorLinkFinder.cpp's NearestJoinable, so this is
	// Airside.Build.AnchorLinkDoesNotRescanEveryGuideline's own measurement, aimed at the
	// sibling-lane path that test's single-lane fixture never reaches: FTwoWayAnchorBothLanesTest
	// is the base fixture, with a crowd of decoy guidelines added AFTER the derive pass so they
	// stand alongside the real two-lane road rather than being swept by it.
	using namespace TwoWayLane;

	auto LayFixture = [](URoadNetwork& Net, int32 DecoyCount)
	{
		const FRoadNodeId A = Net.AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId B = Net.AddNode(FVector2D(20000.0, 0.0));
		Net.AddStraightSegment(A, B, URoadProfile::MakeServiceRoadTransient());

		UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
		Net.PlaceEntity(Depot, Depot->Anchors, FVector2D(10000.0, 1000.0), UE_DOUBLE_PI * 0.5,
			/*DesignWingspan=*/0.0, Depot->PoseRole, Depot->Trucks);

		Derive(Net);

		// FAR BEYOND EVERY REACH THIS FIXTURE'S LINK USES (DefaultServiceLinkRadius is 6500 uu),
		// and added AFTER Derive so a second Topology rebuild sweeping every bDerived guideline
		// is not what keeps them out of this run.
		for (int32 Index = 0; Index < DecoyCount; ++Index)
		{
			const double OffsetY = 5000.0 * Index;
			const FGuidelineNodeId DecoyA = Net.AddGuidelineNode(FVector2D(2000000.0, OffsetY));
			const FGuidelineNodeId DecoyB = Net.AddGuidelineNode(FVector2D(2000000.0, OffsetY + 1000.0));
			FGuidelineEdge Decoy;
			Decoy.A = DecoyA;
			Decoy.B = DecoyB;
			Decoy.Control = (FVector2D(2000000.0, OffsetY) + FVector2D(2000000.0, OffsetY + 1000.0)) * 0.5;
			Decoy.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
			Decoy.Direction = EGuidelineDir::Bidirectional;
			Decoy.bDerived = true;
			Net.AddGuidelineEdge(MoveTemp(Decoy));
		}
	};

	URoadNetwork* Bare = NewObject<URoadNetwork>(GetTransientPackage());
	LayFixture(*Bare, 0);
	const int32 BeforeBare = Bare->SampleGuidelineCallCountForTest();
	FAnchorLink::Build(*Bare, UAirsideSettings::ResolveLargestServiceVehicle());
	const int32 BareCost = Bare->SampleGuidelineCallCountForTest() - BeforeBare;

	URoadNetwork* Crowded = NewObject<URoadNetwork>(GetTransientPackage());
	constexpr int32 DecoyCount = 200;
	LayFixture(*Crowded, DecoyCount);
	const int32 BeforeCrowded = Crowded->SampleGuidelineCallCountForTest();
	FAnchorLink::Build(*Crowded, UAirsideSettings::ResolveLargestServiceVehicle());
	const int32 CrowdedCost = Crowded->SampleGuidelineCallCountForTest() - BeforeCrowded;

	AddInfo(FString::Printf(TEXT("SampleGuideline calls: %d with no decoys, %d with %d of them"),
		BareCost, CrowdedCost, DecoyCount));

	// SAME GENEROUS MULTIPLE AS Airside.Build.AnchorLinkDoesNotRescanEveryGuideline: not one
	// sample per decoy per link, which is what a NearestJoinable with no CannotReachWithin call
	// would cost - low thousands against this bound.
	TestTrue(
		*FString::Printf(TEXT("%d distant decoys cost about the same as none (%d vs %d)"),
			DecoyCount, CrowdedCost, BareCost),
		CrowdedCost <= BareCost * 4 + 4);

	return true;
}

#endif
