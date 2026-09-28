#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Build/ExitGeometry.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/RoadTraffic.h"
#include "Model/TaxiwayStrip.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// LayCrossing moved to AirsideTestFixtures.h as FRoadCrossingFixture::Lay (2026-09-29):
	// TruckCrossingTest.cpp drives traffic over the same junction.

	/** A TURN PATH carries no DerivedFrom - that is how it is told apart from a segment's own
	 *  guideline (see FRoadGuidelineBuilder, where DerivedFrom is deliberately left unset).
	 *  So does a dead end's U-turn balloon (2026-09-23), which lies at a road's FAR end - so a
	 *  turn path here is also one whose control lies within the crossing junction at the
	 *  origin. The taxiway ends are dead ends too, but a bidirectional arm gets no balloon. */
	bool IsTurnPath(const FGuidelineEdge& Edge)
	{
		return Edge.bAlive && Edge.bDerived && !Edge.DerivedFrom.IsSet() && Edge.Control.Size() < 5000.0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadCrossesTaxiwayTest,
	"Airside.Build.RoadCrossesTaxiway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadCrossesTaxiwayTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	FRoadCrossingFixture::Lay(*Net, /*bFarSide=*/true);
	TestGraph::Derive(*Net);

	// THE WHOLE CROSSING RULE, and it is already in the builder: Turn.AllowedTraffic is
	// FromMask & ToMask, so a turn between arms of different classes keeps only Emergency.
	// This test exists because that behaviour is now LOAD BEARING for the fuel slice - it is
	// the only thing keeping a truck off a taxiway - and nothing pinned it.
	int32 VehicleTurns = 0, AircraftTurns = 0, MixedTurns = 0;
	for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
	{
		if (!IsTurnPath(Edge)) { continue; }
		const bool bVehicle = Edge.AllowedTraffic.Allows(ETraversalClass::GroundVehicle);
		const bool bAircraft = Edge.AllowedTraffic.Allows(ETraversalClass::Aircraft);
		VehicleTurns += (bVehicle && !bAircraft) ? 1 : 0;
		AircraftTurns += (bAircraft && !bVehicle) ? 1 : 0;
		MixedTurns += (bVehicle && bAircraft) ? 1 : 0;
	}

	TestTrue(TEXT("vehicles get road-to-road turns through the crossing"), VehicleTurns > 0);
	TestTrue(TEXT("aircraft get taxiway-to-taxiway turns through it"), AircraftTurns > 0);

	// THE SAFETY PROPERTY. A single mixed turn is an aircraft admitted onto a service road,
	// or a truck onto a taxiway, and neither is recoverable by any later rule.
	TestEqual(TEXT("and NO turn admits both - no road-to-taxiway path for anybody"),
		MixedTurns, 0);

	// Emergency crosses between the two, alone, and that is deliberate rather than a leak:
	// the builder's own comment says a fire truck may cross between a service road and a
	// taxiway and nothing else may. Asserted so a later "tighten the mask" change has to
	// argue with it rather than silently delete it.
	int32 EmergencyOnlyTurns = 0;
	for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
	{
		if (!IsTurnPath(Edge)) { continue; }
		EmergencyOnlyTurns += (Edge.AllowedTraffic.Allows(ETraversalClass::Emergency)
			&& !Edge.AllowedTraffic.Allows(ETraversalClass::Aircraft)
			&& !Edge.AllowedTraffic.Allows(ETraversalClass::GroundVehicle)) ? 1 : 0;
	}
	TestTrue(TEXT("emergency alone may cross between the two"), EmergencyOnlyTurns > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadEndsAgainstTaxiwayTest,
	"Airside.Build.RoadEndsAgainstTaxiway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadEndsAgainstTaxiwayTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	FRoadCrossingFixture::Lay(*Net, /*bFarSide=*/false);
	TestGraph::Derive(*Net);

	// A road dead-ending against a taxiway's side: the only arm of its own class at that
	// node is itself, so there is nothing to turn INTO and the junction derives no vehicle
	// path through it.
	int32 VehicleTurns = 0, AircraftTurns = 0;
	for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
	{
		if (!IsTurnPath(Edge)) { continue; }
		VehicleTurns += Edge.AllowedTraffic.Allows(ETraversalClass::GroundVehicle) ? 1 : 0;
		AircraftTurns += Edge.AllowedTraffic.Allows(ETraversalClass::Aircraft) ? 1 : 0;
	}
	TestEqual(TEXT("no vehicle path through the junction"), VehicleTurns, 0);

	// NOT REFUSED, AND THE AIRCRAFT SIDE IS UNAFFECTED. The player may be about to draw the
	// far side, so the tool lets it stand and the census counts it; meanwhile the taxiway
	// must be exactly the taxiway it was.
	TestTrue(TEXT("aircraft still turn through it"), AircraftTurns > 0);
	return true;
}

namespace
{
	/** Does this edge admit ground vehicles and not aircraft (a road lane or road turn)? */
	bool CrossingIsVehicleOnly(const FGuidelineEdge& Edge)
	{
		return Edge.AllowedTraffic.Allows(ETraversalClass::GroundVehicle) && !Edge.AllowedTraffic.Allows(ETraversalClass::Aircraft);
	}

	/** Does this edge admit aircraft and not ground vehicles (a taxiway line or taxiway turn)? */
	bool CrossingIsAircraftOnly(const FGuidelineEdge& Edge)
	{
		return Edge.AllowedTraffic.Allows(ETraversalClass::Aircraft) && !Edge.AllowedTraffic.Allows(ETraversalClass::GroundVehicle);
	}

	/** Every alive node an edge of each class meets: a conflict node, by what it IS, not how
	 *  the builder happens to record it. */
	TArray<FGuidelineNodeId> CrossingConflictNodes(const URoadNetwork& Net)
	{
		TArray<FGuidelineNodeId> Out;
		const TArray<FGuidelineNode>& Nodes = Net.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (!Nodes[Index].bAlive) { continue; }
			bool bVehicle = false, bAircraft = false;
			for (const FGuidelineEdgeId Id : Nodes[Index].Incident)
			{
				if (const FGuidelineEdge* Edge = Net.GetGuidelineEdge(Id))
				{
					bVehicle |= CrossingIsVehicleOnly(*Edge);
					bAircraft |= CrossingIsAircraftOnly(*Edge);
				}
			}
			if (bVehicle && bAircraft) { Out.Add(Net.GuidelineNodeIdAt(Index)); }
		}
		return Out;
	}

	/** Incident edges of a node admitting one class only, by class. */
	void CrossingIncidentByClass(const URoadNetwork& Net, FGuidelineNodeId Node, int32& OutVehicle, int32& OutAircraft)
	{
		OutVehicle = OutAircraft = 0;
		for (const FGuidelineEdgeId Id : Net.GetGuidelineNode(Node)->Incident)
		{
			const FGuidelineEdge* Edge = Net.GetGuidelineEdge(Id);
			OutVehicle += (Edge && CrossingIsVehicleOnly(*Edge)) ? 1 : 0;
			OutAircraft += (Edge && CrossingIsAircraftOnly(*Edge)) ? 1 : 0;
		}
	}

	/**
	 * THE INVARIANT STAGE 4 ADDS, measured on the geometry: how many places a vehicle-only line
	 * and an aircraft-only line cross WITHOUT sharing a node there. Each one is a crossing the
	 * claim table cannot see, because it conflicts by resource identity only. Both lines are
	 * read through GuidelineGeom::Sample, the one sampler the follower walks.
	 */
	int32 CrossingUnsharedCrossings(const URoadNetwork& Net)
	{
		const TArray<FGuidelineEdge>& Edges = Net.GetGuidelineEdges();
		auto Sampled = [&Net](const FGuidelineEdge& Edge)
		{
			TArray<FVector2D> Points;
			GuidelineGeom::Sample(Net.GetGuidelineNode(Edge.A)->Position, Edge.Control,
				Net.GetGuidelineNode(Edge.B)->Position, Points);
			return Points;
		};
		auto Near = [](const FVector2D& P, const FVector2D& Q) { return FVector2D::Distance(P, Q) < 1.0; };
		int32 Unshared = 0;
		for (const FGuidelineEdge& V : Edges)
		{
			if (!V.bAlive || !CrossingIsVehicleOnly(V)) { continue; }
			const TArray<FVector2D> PV = Sampled(V);
			for (const FGuidelineEdge& A : Edges)
			{
				if (!A.bAlive || !CrossingIsAircraftOnly(A)) { continue; }
				const TArray<FVector2D> PA = Sampled(A);
				for (int32 I = 1; I < PV.Num(); ++I)
				{
					for (int32 J = 1; J < PA.Num(); ++J)
					{
						const FVector2D R = PV[I] - PV[I - 1];
						const FVector2D S = PA[J] - PA[J - 1];
						const double Den = FVector2D::CrossProduct(R, S);
						if (FMath::IsNearlyZero(Den)) { continue; }
						const FVector2D QP = PA[J - 1] - PV[I - 1];
						const double T = FVector2D::CrossProduct(QP, S) / Den;
						const double U = FVector2D::CrossProduct(QP, R) / Den;
						if (T < 0.0 || T > 1.0 || U < 0.0 || U > 1.0) { continue; }
						const FVector2D X = PV[I - 1] + R * T;
						// AT A SHARED NODE is the fix, not a defect.
						const bool bShared = ((V.A == A.A || V.A == A.B) && Near(X, Net.GetGuidelineNode(V.A)->Position))
							|| ((V.B == A.A || V.B == A.B) && Near(X, Net.GetGuidelineNode(V.B)->Position));
						Unshared += bShared ? 0 : 1;
					}
				}
			}
		}
		return Unshared;
	}

	/** The derived node a segment end's Nth guideline ends on, found by identity. */
	FGuidelineNodeId CrossingLaneEnd(const URoadNetwork& Net, FRoadSegmentId Segment, bool bEndA, int32 Which)
	{
		const TArray<FGuidelineNode>& Nodes = Net.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			const FGuidelineEndRef& Origin = Nodes[Index].Origin;
			if (Nodes[Index].bAlive && Origin.Segment == Segment && Origin.bEndA == bEndA && Origin.GuidelineIndex == Which)
			{
				return Net.GuidelineNodeIdAt(Index);
			}
		}
		return FGuidelineNodeId();
	}

	/** Road-lane ends of Arm at the crossing node: how far back along the arm each sits, and
	 *  whether each arriving lane carries the stop line. Returns the arriving ends. */
	TArray<FGuidelineNodeId> CrossingCheckArm(FAutomationTestBase& Test, const URoadNetwork& Net, FRoadSegmentId Arm,
		FRoadNodeId Junction, double ExpectedSetBack, const TCHAR* Name)
	{
		TArray<FGuidelineNodeId> Arriving;
		const FRoadSegment* Segment = Net.GetSegment(Arm);
		const URoadProfile* Profile = Net.ProfileFor(*Segment);
		const bool bEndA = Segment->A == Junction;
		const FVector2D At = Net.GetNode(Junction)->Position;
		const FVector2D Axis = Net.GetOutgoingTangent(Arm, Junction).GetSafeNormal();
		for (int32 Which = 0; Which < Profile->Guidelines.Num(); ++Which)
		{
			const FGuidelineNodeId End = CrossingLaneEnd(Net, Arm, bEndA, Which);
			const FGuidelineNode* Node = Net.GetGuidelineNode(End);
			if (!Test.TestNotNull(*FString::Printf(TEXT("%s lane %d has an end at the crossing"), Name, Which), Node))
			{
				continue;
			}
			// ALONG THE ARM, not straight-line: a lane sits its offset off the centreline, and
			// the stop line is a distance down the road, not a radius.
			const double Along = FVector2D::DotProduct(Node->Position - At, Axis);
			Test.TestNearlyEqual(*FString::Printf(TEXT("%s lane %d ends at the strip edge: %.1f uu back"), Name, Which, Along),
				Along, ExpectedSetBack, 1.0);
			if (Profile->Guidelines[Which].ArrivesAt(bEndA))
			{
				Arriving.Add(End);
				Test.TestTrue(*FString::Printf(TEXT("%s lane %d, arriving, is a taxiway-crossing stop line"), Name, Which),
					Node->HoldingPosition == EHoldingPositionKind::TaxiwayCrossing);
				Test.TestFalse(TEXT("and names no runway - it protects conflict nodes, not a strip"),
					Node->HoldingPositionFor.IsSet());
			}
			else
			{
				Test.TestTrue(*FString::Printf(TEXT("%s lane %d, leaving, carries no stop line"), Name, Which),
					Node->HoldingPosition == EHoldingPositionKind::None);
			}
		}
		return Arriving;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadCrossingConflictAndStopLineTest,
	"Airside.Build.RoadCrossingConflictAndStopLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadCrossingConflictAndStopLineTest::RunTest(const FString& Parameters)
{
	// NAMED RoadCrossingConflictAndStopLine, not RoadCrossesTaxiway.ConflictAndStopLine as the
	// plan had it: a dotted child drops the bare-named RoadCrossesTaxiway from the automation
	// tree (memory: unreal-automation-test-tree-drops-bare-parent).
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadCrossingFixture Crossing = FRoadCrossingFixture::Lay(*Net);
	TestGraph::Derive(*Net);

	const URoadProfile* Taxiway = Net->ProfileFor(*Net->GetSegment(Crossing.West));
	const double StripEdge = Taxiway->GetTotalWidth() * 0.5 + TaxiwayStrip::StripWidthOf(*Net, Crossing.West);
	TestTrue(TEXT("the fixture's taxiway has a strip"), StripEdge > Taxiway->GetTotalWidth() * 0.5);

	// ONE CONFLICT PER ROAD LANE: two one-way lanes each cross the taxiway's one bidirectional
	// line once. The line's two directions are coincident turn edges, and both run through
	// the SAME node - a conflict welded per edge pair would leave W->E and E->W on different
	// nodes, and an aircraft going one way would not contend with a truck the other saw.
	const TArray<FGuidelineNodeId> Conflicts = CrossingConflictNodes(*Net);
	TestEqual(TEXT("one conflict node per road lane"), Conflicts.Num(), 2);
	for (const FGuidelineNodeId Conflict : Conflicts)
	{
		const FGuidelineNode* Node = Net->GetGuidelineNode(Conflict);
		TestNearlyEqual(TEXT("the conflict lies on the taxiway centreline"), Node->Position.Y, 0.0, 1.0);
		int32 Vehicle = 0, Aircraft = 0;
		CrossingIncidentByClass(*Net, Conflict, Vehicle, Aircraft);
		TestEqual(TEXT("the road lane runs into and out of it"), Vehicle, 2);
		TestEqual(TEXT("both directions of the taxiway line run into and out of it"), Aircraft, 4);
	}

	TestEqual(TEXT("no vehicle line crosses an aircraft line without sharing a node there"),
		CrossingUnsharedCrossings(*Net), 0);

	// THE STOP LINE, at the strip edge on both arms: (half width + strip) / sin(90 deg).
	for (const TPair<FRoadSegmentId, const TCHAR*> Arm : { TPair<FRoadSegmentId, const TCHAR*>(Crossing.South, TEXT("south")),
		TPair<FRoadSegmentId, const TCHAR*>(Crossing.North, TEXT("north")) })
	{
		for (const FGuidelineNodeId Hold : CrossingCheckArm(*this, *Net, Arm.Key, Crossing.Centre, StripEdge, Arm.Value))
		{
			const FGuidelineNode* Node = Net->GetGuidelineNode(Hold);
			// THE LANE'S OWN CONFLICT and nothing else: the other lane's traffic runs the other way
			// and never reaches that node from this line.
			if (TestEqual(TEXT("the stop line protects exactly the conflict its lane runs through"),
				Node->ProtectsConflicts.Num(), 1))
			{
				const FGuidelineNode* Conflict = Net->GetGuidelineNode(Node->ProtectsConflicts[0]);
				TestTrue(TEXT("which is a conflict node"), Conflicts.Contains(Node->ProtectsConflicts[0]));
				TestNearlyEqual(TEXT("on this lane"), Conflict ? Conflict->Position.X : 1e9, Node->Position.X, 1.0);
			}
		}
	}

	// A REBUILD RE-DERIVES THEM, not accumulates them.
	TestGraph::Derive(*Net);
	TestEqual(TEXT("a second derive still has one conflict per lane"), CrossingConflictNodes(*Net).Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadEndsAgainstTaxiwayStopLineTest,
	"Airside.Build.RoadEndsAgainstTaxiwayStopLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadEndsAgainstTaxiwayStopLineTest::RunTest(const FString& Parameters)
{
	// Review Focus 1: a road ENDING on the taxiway has no through-path, so nothing crosses and
	// there is no conflict - but the road still meets a live taxiway (an Emergency turn onto it
	// exists), so its arriving lane still stops at the strip edge.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadCrossingFixture Crossing = FRoadCrossingFixture::Lay(*Net, /*bFarSide=*/false);
	TestGraph::Derive(*Net);

	const URoadProfile* Taxiway = Net->ProfileFor(*Net->GetSegment(Crossing.West));
	const double StripEdge = Taxiway->GetTotalWidth() * 0.5 + TaxiwayStrip::StripWidthOf(*Net, Crossing.West);

	TestEqual(TEXT("no conflict node: no road line crosses the taxiway"), CrossingConflictNodes(*Net).Num(), 0);
	const TArray<FGuidelineNodeId> Holds = CrossingCheckArm(*this, *Net, Crossing.South, Crossing.Centre, StripEdge, TEXT("south"));
	TestEqual(TEXT("one arriving lane"), Holds.Num(), 1);
	for (const FGuidelineNodeId Hold : Holds)
	{
		TestEqual(TEXT("which protects nothing - there is nothing for it to reserve"),
			Net->GetGuidelineNode(Hold)->ProtectsConflicts.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadCrossingTwoTaxiwaysTest,
	"Airside.Build.RoadCrossingTwoTaxiways",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadCrossingTwoTaxiwaysTest::RunTest(const FString& Parameters)
{
	// Review Focus 2: a road through a node where a second taxiway also meets. Every aircraft
	// turn at the node that crosses a road lane - the straight W-E line and the turns to and
	// from the diagonal arm alike - must share a node with it, and each road hold reserves
	// every conflict its lane runs through.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadCrossingFixture Crossing = FRoadCrossingFixture::Lay(*Net);
	const FRoadNodeId NorthEast = Net->AddNode(FVector2D(14142.0, 14142.0));
	const FRoadSegmentId Diagonal = Net->AddStraightSegment(Crossing.Centre, NorthEast, TestProfiles::Taxiway());
	TestGraph::Derive(*Net);

	TestEqual(TEXT("no vehicle line crosses an aircraft line without sharing a node there"),
		CrossingUnsharedCrossings(*Net), 0);

	// THE STOP LINE CLEARS THE WORST STRIP: the diagonal meets the road at 45 degrees, so its
	// strip edge lies (half width + strip + road half width x cos 45) / sin 45 down the road -
	// further than the square one. The road's own width counts at an angle: the bar's near
	// corner reaches the strip first (Airside.Build.RoadCrossingAtSixtyDegrees).
	const URoadProfile* Taxiway = Net->ProfileFor(*Net->GetSegment(Diagonal));
	const URoadProfile* Road = Net->ProfileFor(*Net->GetSegment(Crossing.South));
	const double StripEdge = (Taxiway->GetTotalWidth() * 0.5 + TaxiwayStrip::StripWidthOf(*Net, Diagonal)
		+ Road->GetTotalWidth() * 0.5 * FMath::Cos(FMath::DegreesToRadians(45.0)))
		/ FMath::Sin(FMath::DegreesToRadians(45.0));
	const TArray<FGuidelineNodeId> Conflicts = CrossingConflictNodes(*Net);
	for (const TPair<FRoadSegmentId, const TCHAR*> Arm : { TPair<FRoadSegmentId, const TCHAR*>(Crossing.South, TEXT("south")),
		TPair<FRoadSegmentId, const TCHAR*>(Crossing.North, TEXT("north")) })
	{
		for (const FGuidelineNodeId Hold : CrossingCheckArm(*this, *Net, Arm.Key, Crossing.Centre, StripEdge, Arm.Value))
		{
			const TArray<FGuidelineNodeId>& Protects = Net->GetGuidelineNode(Hold)->ProtectsConflicts;
			TestTrue(*FString::Printf(TEXT("the %s stop line reserves every aircraft line its lane crosses: %d"),
				Arm.Value, Protects.Num()), Protects.Num() >= 2);
			for (const FGuidelineNodeId Conflict : Protects)
			{
				TestTrue(TEXT("each of them a conflict node"), Conflicts.Contains(Conflict));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadCrossingStubClampedTest,
	"Airside.Build.RoadCrossingStubClamped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadCrossingStubClampedTest::RunTest(const FString& Parameters)
{
	// Review Focus 5: a road stub shorter than the strip it would stand back from. The stop
	// line is clamped to the arm's share - ExitGeometry::ArmShare, the runway exits' own clamp -
	// so no end crosses its own far end, and the clamp is said once.
	//
	// 50 M, NOT THE PLAN'S 10: a 10 m service-road stub is shorter than its own junction cut
	// (19.5 m here) and does not solve at all, so it derives no end to clamp.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Taxiway = TestProfiles::Taxiway();
	URoadProfile* Road = URoadProfile::MakeServiceRoadTransient();
	Net->DefaultProfile = Taxiway;
	const double StubLength = 5000.0;
	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadSegmentId West = Net->AddStraightSegment(Net->AddNode(FVector2D(-20000.0, 0.0)), Centre, Taxiway);
	Net->AddStraightSegment(Centre, Net->AddNode(FVector2D(20000.0, 0.0)), Taxiway);
	const FRoadSegmentId Stub = Net->AddStraightSegment(Net->AddNode(FVector2D(0.0, -StubLength)), Centre, Road);
	const FRoadSegmentId North = Net->AddStraightSegment(Centre, Net->AddNode(FVector2D(0.0, 20000.0)), Road);

	AddExpectedMessage(TEXT("Crossing setback clamped"), EAutomationExpectedMessageFlags::Contains, 1);
	TestGraph::Derive(*Net);

	const double StripEdge = Taxiway->GetTotalWidth() * 0.5 + TaxiwayStrip::StripWidthOf(*Net, West);
	TestTrue(TEXT("the fixture's stub really is shorter than the strip needs"), ExitGeometry::ArmShare * StubLength < StripEdge);
	CrossingCheckArm(*this, *Net, Stub, Centre, ExitGeometry::ArmShare * StubLength, TEXT("stub"));
	CrossingCheckArm(*this, *Net, North, Centre, StripEdge, TEXT("north"));
	TestEqual(TEXT("no vehicle line crosses an aircraft line without sharing a node there"),
		CrossingUnsharedCrossings(*Net), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadCrossingAtSixtyDegreesTest,
	"Airside.Build.RoadCrossingAtSixtyDegrees",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadCrossingAtSixtyDegreesTest::RunTest(const FString& Parameters)
{
	// FINAL REVIEW, IMPORTANT 5: at an oblique crossing the ROAD'S OWN WIDTH reaches the strip
	// before its centreline does - a stop line placed at (half width + strip) / sin(angle) had
	// its corner half-road x cos(angle) inside the strip (2.3 m at 60 degrees). Measured on the
	// bar the line stands for: both of its corners, across the whole road, outside the strip.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Taxiway = TestProfiles::Taxiway();
	URoadProfile* Road = URoadProfile::MakeServiceRoadTransient();
	Net->DefaultProfile = Taxiway;
	const FVector2D Axis(FMath::Cos(FMath::DegreesToRadians(60.0)), FMath::Sin(FMath::DegreesToRadians(60.0)));
	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadSegmentId West = Net->AddStraightSegment(Net->AddNode(FVector2D(-20000.0, 0.0)), Centre, Taxiway);
	Net->AddStraightSegment(Centre, Net->AddNode(FVector2D(20000.0, 0.0)), Taxiway);
	const FRoadSegmentId South = Net->AddStraightSegment(Net->AddNode(-Axis * 20000.0), Centre, Road);
	const FRoadSegmentId North = Net->AddStraightSegment(Centre, Net->AddNode(Axis * 20000.0), Road);
	TestGraph::Derive(*Net);

	const double StripEdge = Taxiway->GetTotalWidth() * 0.5 + TaxiwayStrip::StripWidthOf(*Net, West);
	const double RoadHalf = Road->GetTotalWidth() * 0.5;
	const double Sine = FMath::Sin(FMath::DegreesToRadians(60.0));
	const double Cosine = FMath::Cos(FMath::DegreesToRadians(60.0));
	const double Expected = (StripEdge + RoadHalf * Cosine) / Sine;
	for (const TPair<FRoadSegmentId, const TCHAR*> Arm : { TPair<FRoadSegmentId, const TCHAR*>(South, TEXT("south")),
		TPair<FRoadSegmentId, const TCHAR*>(North, TEXT("north")) })
	{
		CrossingCheckArm(*this, *Net, Arm.Key, Centre, Expected, Arm.Value);
		const FVector2D Out = Net->GetOutgoingTangent(Arm.Key, Centre).GetSafeNormal();
		const FVector2D Across(-Out.Y, Out.X);
		const FVector2D OnLine = Out * Expected;
		for (const double Side : { -1.0, 1.0 })
		{
			const FVector2D Corner = OnLine + Across * RoadHalf * Side;
			TestTrue(*FString::Printf(TEXT("%s: the stop line's corner (%.0f, %.0f) is outside the strip (edge %.0f)"),
				Arm.Value, Corner.X, Corner.Y, StripEdge), FMath::Abs(Corner.Y) >= StripEdge - 1.0);
		}
	}
	TestEqual(TEXT("no vehicle line crosses an aircraft line without sharing a node there"),
		CrossingUnsharedCrossings(*Net), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEmergencyNeverTurnsAtAConflictTest,
	"Airside.Build.EmergencyNeverTurnsAtAConflict",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEmergencyNeverTurnsAtAConflictTest::RunTest(const FString& Parameters)
{
	// FINAL REVIEW, IMPORTANT 6: a conflict node is where a road lane and a taxiway line CROSS,
	// not a junction. Both classes' lines admitted Emergency, the weld joined them there, and the
	// route search has no heading check - so a fire truck's shortest U-turn was a right angle
	// onto the taxiway at one lane's conflict and another back off it at the other's. Every
	// Emergency route between the crossing's arm ends must go through a conflict node on ONE
	// class of line.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	FRoadCrossingFixture::Lay(*Net);
	TestGraph::Derive(*Net);
	const TArray<FGuidelineNodeId> Conflicts = CrossingConflictNodes(*Net);
	if (!TestTrue(TEXT("the crossing has conflict nodes"), Conflicts.Num() > 0)) { return false; }

	TArray<FGuidelineNodeId> Ends;
	const TArray<FGuidelineNode>& Nodes = Net->GetGuidelineNodes();
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		if (Nodes[Index].bAlive && Nodes[Index].Origin.IsSet()) { Ends.Add(Net->GuidelineNodeIdAt(Index)); }
	}
	int32 Routes = 0;
	for (const FGuidelineNodeId From : Ends)
	{
		for (const FGuidelineNodeId To : Ends)
		{
			if (From == To) { continue; }
			const FRoutePlan Plan = TestGraph::Probe(*Net, From, To, ETraversalClass::Emergency);
			if (!Plan.IsValid()) { continue; }
			++Routes;
			for (int32 Step = 1; Step < Plan.Steps.Num(); ++Step)
			{
				if (!Conflicts.Contains(Plan.Steps[Step - 1].To)) { continue; }
				const FGuidelineEdge* In = Net->GetGuidelineEdge(Plan.Steps[Step - 1].Edge);
				const FGuidelineEdge* Out = Net->GetGuidelineEdge(Plan.Steps[Step].Edge);
				const bool bInRoad = In && In->AllowedTraffic.Allows(ETraversalClass::GroundVehicle);
				const bool bOutRoad = Out && Out->AllowedTraffic.Allows(ETraversalClass::GroundVehicle);
				TestEqual(*FString::Printf(TEXT("route %d -> %d stays on one class of line through conflict node %d"),
					From.Index, To.Index, Plan.Steps[Step - 1].To.Index), bInRoad, bOutRoad);
			}
		}
	}
	TestTrue(*FString::Printf(TEXT("emergency routes exist to judge: %d"), Routes), Routes > 0);
	return true;
}

namespace RoadCrossingTest
{
	/** Warnings on one category, captured unbuffered (FLogLineSpy's own override, #216). */
	struct FCrossingWarningSpy : public FLogLineSpy
	{
		explicit FCrossingWarningSpy(FName InCategory) : FLogLineSpy(InCategory) {}

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& InCategory) override
		{
			if (InCategory == Category && Verbosity == ELogVerbosity::Warning)
			{
				++Count;
				CapturedLines.Add(FString(V));
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadCrossingWarnsNothingTest,
	"Airside.Build.RoadCrossingWarnsNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadCrossingWarnsNothingTest::RunTest(const FString& Parameters)
{
	// FINAL REVIEW, IMPORTANT 8: a plain right-angle crossing of generous arms is a junction
	// nobody need redraw, and "turn path radius 502 uu, but ... needs 502 ... Draw them longer"
	// fired at every one, every rebuild (184 lines in one test log; none on main). Warnings on
	// the builder's own category over two derives: none about the turns.
	RoadCrossingTest::FCrossingWarningSpy Spy(TEXT("LogAirside"));
	GLog->AddOutputDevice(&Spy);
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	FRoadCrossingFixture::Lay(*Net);
	TestGraph::Derive(*Net);
	TestGraph::Derive(*Net);
	GLog->RemoveOutputDevice(&Spy);
	int32 Radius = 0;
	for (const FString& Line : Spy.CapturedLines)
	{
		if (Line.Contains(TEXT("turn path radius")))
		{
			++Radius;
			if (Radius == 1) { AddInfo(Line); }
		}
	}
	TestEqual(TEXT("no turn-radius warning at a plain crossing"), Radius, 0);
	return true;
}

#endif
