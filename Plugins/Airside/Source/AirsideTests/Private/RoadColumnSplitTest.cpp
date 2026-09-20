#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/GuideArbiter.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/RoadNaming.h"
#include "Tool/SnapGuideChain.h"
#include "Tool/SnapGuideSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE ROAD COLUMN IS TWO COLUMNS - split 2026-09-20 - AND EACH SWITCHES ON ITS OWN.
 *
 * "RoadNaming::Describe already classifies three ways, so a label reading 'parallel to the
 * service road' appears under a button marked Road." The grid had collapsed two things nothing
 * else in the codebase joins. What makes the split real is not the enum member: it is that a
 * player can now silence one and keep the other, and that what they silence matches what the
 * label says. Both are measured here, on a field with one of each a few metres apart.
 */
namespace
{
	/** A road of the given kind, through the facade so it is given a profile to be judged by. */
	void LaySplit(ARoadNetworkActor* Actor, const FVector2D& From, const FVector2D& To,
		ERoadKind Kind)
	{
		IRoadEditTarget* Target = Actor;
		const int32 A = Target->PlaceNode(From);
		const int32 B = Target->PlaceNode(To);
		Target->ConnectNodes(A, B, Kind, INDEX_NONE);
	}

	/** Only Collinear, and only the columns named, so a winner came from nowhere else. */
	FSnapGuideSettings OnlyCollinear(bool bTaxiway, bool bServiceRoad)
	{
		FSnapGuideSettings Settings;
		Settings.bExtending = false;
		Settings.bLevelWith = false;
		Settings.bParallel = false;
		Settings.bCollinear = true;
		Settings.bAngledFrom = false;
		Settings.bMatchingGap = false;
		Settings.bTaxiway = bTaxiway;
		Settings.bServiceRoad = bServiceRoad;
		Settings.bRunway = false;
		Settings.bApron = false;
		Settings.bStand = false;
		Settings.bWorld = false;
		return Settings;
	}
}

/**
 * ONE CLASSIFICATION ANSWERS BOTH THE LABEL AND THE COLUMN.
 *
 * RoadNaming::Describe and RoadNaming::ReferenceOf are one function's two faces since the
 * split - Describe is BUILT on ReferenceOf - and this is what says so. Before that, four guide
 * sources hard-coded EReference::Road while Describe worked the classification out again, and
 * the two could not have disagreed only because one of them was a constant.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadColumnAndLabelAgreeTest,
	"Airside.Tool.RoadColumnAndLabelAgree",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadColumnAndLabelAgreeTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	LaySplit(Actor, FVector2D(-10000.0, 0.0), FVector2D(10000.0, 0.0), ERoadKind::Taxiway);
	LaySplit(Actor, FVector2D(-10000.0, 5000.0), FVector2D(10000.0, 5000.0), ERoadKind::ServiceRoad);
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	// PAIRED WITH THE WORD THE PLAYER READS, not asserted on its own. The whole complaint was
	// that the two could say different things about one segment.
	const TArray<TPair<FString, SnapGuide::EReference>> Expected = {
		{ TEXT("the taxiway"),      SnapGuide::EReference::Taxiway     },
		{ TEXT("the service road"), SnapGuide::EReference::ServiceRoad } };

	for (int32 Index = 0; Index < Expected.Num(); ++Index)
	{
		const FRoadSegmentId Id = Actor->Network->SegmentIdAt(Index);

		SnapGuide::EReference Column = SnapGuide::EReference::World;
		if (!TestTrue(*FString::Printf(TEXT("segment %d classifies"), Index),
			RoadNaming::ReferenceOf(*Actor->Network, Id, Column)))
		{
			continue;
		}

		TestEqual(*FString::Printf(TEXT("segment %d is named for what it admits"), Index),
			RoadNaming::Describe(*Actor->Network, Id), Expected[Index].Key);
		TestEqual(*FString::Printf(TEXT("and segment %d lands in the column that word names"), Index),
			static_cast<int32>(Column), static_cast<int32>(Expected[Index].Value));
	}

	// A RUNWAY IS NEITHER, and its classification must come first: a runway's cross-section
	// may well admit a ground vehicle, and the service-road test would then claim it.
	if (TestTrue(TEXT("a runway is laid"),
		TestGuide::LayRunway(Actor, FVector2D(-40000.0, 20000.0), FVector2D(40000.0, 20000.0))))
	{
		SnapGuide::EReference Column = SnapGuide::EReference::World;
		const FRoadSegmentId Strip = Actor->Network->SegmentIdAt(2);
		if (TestTrue(TEXT("the runway classifies"),
			RoadNaming::ReferenceOf(*Actor->Network, Strip, Column)))
		{
			TestEqual(TEXT("a runway is a runway before it is anything else"),
				static_cast<int32>(Column), static_cast<int32>(SnapGuide::EReference::Runway));
		}
	}

	return true;
}

/**
 * AND EACH COLUMN SWITCHES WITHOUT THE OTHER, which is the whole point of there being two.
 *
 * MEASURED ON ONE FIELD WITH ONE OF EACH, so "nothing answers" cannot be a field with nothing
 * in it. Collinear rather than Parallel because Collinear proposes ONE CANDIDATE PER SEGMENT -
 * Parallel takes only the nearest, so with the taxiway switched off it would answer with the
 * service road for a reason that has nothing to do with the gate under test.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEachRoadColumnSwitchesAloneTest,
	"Airside.Tool.EachRoadColumnSwitchesAlone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEachRoadColumnSwitchesAloneTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// A taxiway east from the origin and a service road east from 2 m north of it. Their
	// extensions lie 200 uu apart, which is inside EFit::Perpendicular's 300 uu corridor, so
	// a cursor between them is eligible for BOTH and only the gate can separate them.
	LaySplit(Actor, FVector2D(-10000.0, 0.0), FVector2D(0.0, 0.0), ERoadKind::Taxiway);
	LaySplit(Actor, FVector2D(-10000.0, 200.0), FVector2D(0.0, 200.0), ERoadKind::ServiceRoad);
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	const FSnapGuideChain Chain;
	const FGuideAnchor Anchor = TestGuide::BareAnchor(FVector2D(4000.0, 100.0));
	const FVector2D Cursor(4000.0, 100.0);

	// BOTH ON: something answers, so the fixture is a live one and the two legs below are
	// about the gate rather than about an empty field.
	const SnapGuide::FResult Both = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), OnlyCollinear(true, true));
	if (!TestTrue(TEXT("with both columns on, a road offers its line"), Both.bActive))
	{
		return false;
	}

	// TAXIWAY ONLY: the service road is nearer the cursor by 100 uu, so if the gate did
	// nothing it would be the winner. It must not appear at all.
	const SnapGuide::FResult TaxiwayOnly = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), OnlyCollinear(true, false));
	if (TestTrue(TEXT("with only Taxiway on, a line still holds"), TaxiwayOnly.bActive))
	{
		TestEqual(TEXT("and it is the taxiway's, not the service road's"),
			static_cast<int32>(TaxiwayOnly.Winners[0].Reference),
			static_cast<int32>(SnapGuide::EReference::Taxiway));
		TestEqual(TEXT("labelled as one, because the label and the column are one answer"),
			TaxiwayOnly.Winners[0].Description, FString(TEXT("in line with the taxiway")));
	}

	// SERVICE ROAD ONLY: the mirror, and the leg that fails if the split is cosmetic. Before
	// 2026-09-20 one flag governed both, so switching "Road" off silenced this too.
	const SnapGuide::FResult ServiceOnly = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), OnlyCollinear(false, true));
	if (TestTrue(TEXT("with only ServiceRoad on, a line still holds"), ServiceOnly.bActive))
	{
		TestEqual(TEXT("and it is the service road's"),
			static_cast<int32>(ServiceOnly.Winners[0].Reference),
			static_cast<int32>(SnapGuide::EReference::ServiceRoad));
		TestEqual(TEXT("labelled as one"),
			ServiceOnly.Winners[0].Description, FString(TEXT("in line with the service road")));
	}

	// AND NEITHER: the control that says the two legs above were gated, not merely lucky.
	const SnapGuide::FResult Neither = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), OnlyCollinear(false, false));
	TestFalse(TEXT("with both columns off, the same cursor is offered nothing"), Neither.bActive);

	return true;
}

/**
 * A MATCHING GAP IS MEASURED BETWEEN ROADS OF ONE KIND.
 *
 * ICAO separates taxiways by the wingspan admitted; what a service road keeps from the next
 * one is a question of what has to drive between them. A pair of one of each keeps a gap that
 * is neither standard, and offering it as "40 m, matching the taxiway" would put a number
 * under a button that does not govern where it came from - see SnapGuide::IsLegalCell.
 *
 * A NARROWING, and said out loud: before 2026-09-20 this source measured between any two
 * parallel non-runway roads.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatchingGapIsWithinOneKindTest,
	"Airside.Tool.MatchingGapIsWithinOneKind",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMatchingGapIsWithinOneKindTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// A taxiway on y = 0 and a SERVICE ROAD parallel to it 4000 uu north. Nothing else, so the
	// only pair on the field is a mixed one.
	LaySplit(Actor, FVector2D(-10000.0, 0.0), FVector2D(10000.0, 0.0), ERoadKind::Taxiway);
	LaySplit(Actor, FVector2D(-10000.0, 4000.0), FVector2D(10000.0, 4000.0), ERoadKind::ServiceRoad);
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	const FOffsetGuideSource Source;
	const FGuideAnchor Anchor = TestGuide::BareAnchor(FVector2D(0.0, -500.0));

	const TArray<SnapGuide::FCandidate> Mixed =
		TestGuide::ProposedBy(Source, *Actor->Network, Anchor, FVector2D(0.0, -500.0));
	TestEqual(TEXT("a taxiway and a service road are not a gap worth copying"),
		Mixed.Num(), 0);

	// THE CONTROL: make the neighbour a taxiway too and the same geometry DOES offer a gap.
	// Without this leg, a source that had simply stopped working would pass the assertion
	// above - the shape of failure this project has shipped before.
	FAirsideTestWorld SecondWorld;
	if (!TestNotNull(TEXT("a second world"), SecondWorld.World)) { return false; }
	ARoadNetworkActor* Pair = SecondWorld.Actor;
	if (!TestNotNull(TEXT("a second network actor"), Pair)) { return false; }

	LaySplit(Pair, FVector2D(-10000.0, 0.0), FVector2D(10000.0, 0.0), ERoadKind::Taxiway);
	LaySplit(Pair, FVector2D(-10000.0, 4000.0), FVector2D(10000.0, 4000.0), ERoadKind::Taxiway);

	const TArray<SnapGuide::FCandidate> OneKind =
		TestGuide::ProposedBy(Source, *Pair->Network, Anchor, FVector2D(0.0, -500.0));
	if (!TestTrue(TEXT("two taxiways the same distance apart do offer one"), OneKind.Num() > 0))
	{
		return false;
	}
	TestEqual(TEXT("under the Taxiway column, which is the reference's own"),
		static_cast<int32>(OneKind[0].Reference),
		static_cast<int32>(SnapGuide::EReference::Taxiway));

	return true;
}

#endif
