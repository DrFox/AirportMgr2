#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadApron.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/GuideArbiter.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/SnapGuideChain.h"
#include "Tool/SnapGuideSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * A square apron with its south-west corner at the origin, 8000 uu on a side.
	 *
	 * COUNTER-CLOCKWISE, as FApronSurface::Outline declares itself. A clockwise outline would
	 * reverse every edge direction and so flip which side the flush candidates land on - the
	 * test would still pass, measuring a mirror image of the rule.
	 */
	bool LayApron(ARoadNetworkActor* Actor)
	{
		IRoadEditTarget* Target = Actor;

		// A NODE FIRST, to bring the network into being - URoadEditFacade creates it lazily
		// inside PlaceNode, and AddApron does not. Same trap TestGuide::LayRunway records.
		Target->PlaceNode(FVector2D(-100000.0, -100000.0));

		const TArray<FVector2D> Outline = {
			FVector2D(0.0, 0.0), FVector2D(8000.0, 0.0),
			FVector2D(8000.0, 8000.0), FVector2D(0.0, 8000.0) };
		return Target->AddApron(Outline) != INDEX_NONE;
	}

	/** Only the relation named, only the Apron column. Nothing else can answer. */
	FSnapGuideSettings OnlyApron(SnapGuide::ERelation Relation)
	{
		FSnapGuideSettings Settings;
		Settings.bExtending = false;
		Settings.bLevelWith = Relation == SnapGuide::ERelation::LevelWith;
		Settings.bParallel = Relation == SnapGuide::ERelation::Parallel;
		Settings.bCollinear = Relation == SnapGuide::ERelation::Collinear;
		Settings.bAngledFrom = Relation == SnapGuide::ERelation::AngledFrom;
		Settings.bMatchingGap = false;
		Settings.bRoad = false;
		Settings.bRunway = false;
		Settings.bApron = true;
		Settings.bStand = false;
		Settings.bWorld = false;
		return Settings;
	}

	/** An anchor dragging a ROAD centreline, with asymmetric half-widths to catch a mirror. */
	FGuideAnchor RoadAnchorAt(const FVector2D& Origin)
	{
		FGuideAnchor Anchor;
		Anchor.Origin = Origin;
		Anchor.Reference = FVector2D(1.0, 0.0);
		Anchor.ReferenceAt = Origin - FVector2D(2000.0, 0.0);
		Anchor.ReferenceName = TEXT("this road");
		Anchor.Point = EDragPoint::Centreline;

		// DELIBERATELY UNEQUAL. A mirrored pair would be wrong on every off-centre cross-section
		// and right on every symmetric one, so a symmetric fixture cannot tell the two apart.
		Anchor.HalfWidthLeft = 900.0;
		Anchor.HalfWidthRight = 500.0;
		return Anchor;
	}
}

/**
 * AN APRON EDGE IS A DIRECTION TO POINT ALONG.
 *
 * The Apron column had four declared cells and no source behind any of them until 2026-09-20 -
 * bApron was a switch with nothing under it, the same state bOffset shipped in.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApronEdgeOffersItsDirectionTest,
	"Airside.Tool.ApronEdgeOffersItsDirection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FApronEdgeOffersItsDirectionTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }
	if (!TestTrue(TEXT("the apron is laid"), LayApron(Actor))) { return false; }

	// SOUTH OF THE APRON'S SOUTH EDGE, which runs along +X. A cursor due east of the origin is
	// 0 degrees off that edge's direction, inside the 7 degree tolerance.
	const FGuideAnchor Anchor = RoadAnchorAt(FVector2D(2000.0, -3000.0));
	const FSnapGuideChain Chain;

	const SnapGuide::FResult Result = Chain.Resolve(*Actor->Network, Anchor,
		FVector2D(9000.0, -3000.0), SnapGuide::FResult(),
		OnlyApron(SnapGuide::ERelation::Parallel));

	if (!TestTrue(TEXT("an apron edge offers its direction"), Result.bActive)) { return false; }
	TestEqual(TEXT("against the Apron column"),
		static_cast<int32>(Result.Winners[0].Reference),
		static_cast<int32>(SnapGuide::EReference::Apron));
	TestEqual(TEXT("as a Parallel guide"),
		static_cast<int32>(Result.Winners[0].Relation),
		static_cast<int32>(SnapGuide::ERelation::Parallel));

	// ANGULAR, so no half-width applies: it constrains which way, never where.
	TestEqual(TEXT("judged by direction, so the drag's width is irrelevant"),
		static_cast<int32>(Result.Winners[0].Fit),
		static_cast<int32>(SnapGuide::EFit::Angular));

	return true;
}

/**
 * A ROAD LINES UP EDGE TO EDGE WITH AN APRON, NOT CENTRE TO EDGE.
 *
 * Raised in review of the grid design: "the centre of your road aligning with the edge of an
 * apron is probably not that useful, you want an edge of your road colinear with the edge of the
 * apron". This is the only cell where a CENTRELINE drag meets an extended BOUNDARY, so it is the
 * only place the displacement applies - design section 6.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCollinearAgainstAnApronIsFlushByHalfWidthTest,
	"Airside.Tool.CollinearAgainstAnApronIsFlushByHalfWidth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FCollinearAgainstAnApronIsFlushByHalfWidthTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }
	if (!TestTrue(TEXT("the apron is laid"), LayApron(Actor))) { return false; }

	const FGuideAnchor Anchor = RoadAnchorAt(FVector2D(2000.0, -3000.0));
	const FSnapGuideChain Chain;

	// THE SOURCE ASKED DIRECTLY, not through Resolve: the arbiter returns at most one winner per
	// fit kind, and the whole claim here is that TWO candidates exist for one edge.
	const FApronLineGuideSource Source;
	TArray<SnapGuide::FCandidate> Candidates;
	Source.Propose(*Actor->Network, Anchor, Candidates);

	// The apron's south edge runs along +X through y = 0. Its two flush lines sit at y = +900
	// and y = -500 - the road's own half-widths, which are NOT equal.
	int32 Left = 0;
	int32 Right = 0;
	for (const SnapGuide::FCandidate& Candidate : Candidates)
	{
		if (!FMath::IsNearlyZero(Candidate.Direction.Y, 1.0e-6)) { continue; }
		if (FMath::IsNearlyEqual(Candidate.Through.Y, 900.0, 1.0)) { ++Left; }
		if (FMath::IsNearlyEqual(Candidate.Through.Y, -500.0, 1.0)) { ++Right; }
	}

	TestEqual(TEXT("the road's left edge can sit flush with the apron"), Left, 1);
	TestEqual(TEXT("and its right edge, at its OWN half-width"), Right, 1);

	// NOT ON THE EDGE ITSELF. A candidate through y = 0 would be the centre-to-edge alignment
	// this test exists to rule out, and it is what the source proposed before the displacement.
	int32 Centred = 0;
	for (const SnapGuide::FCandidate& Candidate : Candidates)
	{
		if (FMath::IsNearlyZero(Candidate.Direction.Y, 1.0e-6)
			&& FMath::IsNearlyZero(Candidate.Through.Y, 1.0))
		{
			++Centred;
		}
	}
	TestEqual(TEXT("and the road's CENTRE is never offered on the apron's edge"), Centred, 0);

	// A BOUNDARY DRAG IS NOT DISPLACED - an apron corner against another apron's edge is
	// boundary against boundary, and one line is then the honest answer rather than two.
	FGuideAnchor Corner = Anchor;
	Corner.Point = EDragPoint::Boundary;
	Corner.HalfWidthLeft = 0.0;
	Corner.HalfWidthRight = 0.0;

	TArray<SnapGuide::FCandidate> Undisplaced;
	Source.Propose(*Actor->Network, Corner, Undisplaced);

	int32 OnTheEdge = 0;
	for (const SnapGuide::FCandidate& Candidate : Undisplaced)
	{
		if (FMath::IsNearlyZero(Candidate.Direction.Y, 1.0e-6)
			&& FMath::IsNearlyZero(Candidate.Through.Y, 1.0))
		{
			++OnTheEdge;
		}
	}
	TestEqual(TEXT("a boundary drag gets the edge itself, once"), OnTheEdge, 1);

	return true;
}

/**
 * THE APRON COLUMN OFF SILENCES EVERY RELATION - the 2026-09-20 report, in its new column.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApronColumnOffSilencesEveryRelationTest,
	"Airside.Tool.ApronColumnOffSilencesEveryRelation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FApronColumnOffSilencesEveryRelationTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }
	if (!TestTrue(TEXT("the apron is laid"), LayApron(Actor))) { return false; }

	const FGuideAnchor Anchor = RoadAnchorAt(FVector2D(2000.0, -3000.0));
	const FSnapGuideChain Chain;

	// EVERY RELATION ON, THE APRON COLUMN OFF, and no other column on either - so the apron is
	// the only thing on the field that could answer, and nothing may.
	FSnapGuideSettings Settings;
	Settings.bExtending = false;
	Settings.bLevelWith = true;
	Settings.bParallel = true;
	Settings.bCollinear = true;
	Settings.bAngledFrom = true;
	Settings.bMatchingGap = true;
	Settings.bRoad = false;
	Settings.bRunway = false;
	Settings.bApron = false;
	Settings.bStand = false;
	Settings.bWorld = false;

	const FVector2D Cursor(9000.0, -3000.0);
	const SnapGuide::FResult Off = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	TestFalse(TEXT("with the Apron column off, no relation answers"), Off.bActive);

	// CONTROL LEG: the drag was fine. Switch the column on and the apron answers.
	Settings.bApron = true;
	const SnapGuide::FResult On = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	if (!TestTrue(TEXT("with it on, the apron answers"), On.bActive)) { return false; }
	TestEqual(TEXT("and what answers is the apron"),
		static_cast<int32>(On.Winners[0].Reference),
		static_cast<int32>(SnapGuide::EReference::Apron));

	return true;
}

#endif
