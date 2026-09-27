#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Build/AnchorLink.h"
#include "Build/AnchorLinkFinder.h"
#include "Build/BuildCost.h"
#include "Build/StandLayoutBuild.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Model/ArrivalPlanner.h"
#include "Model/InspectFacts.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Solve/IcaoCode.h"
#include "Solve/PlotYard.h"
#include "Solve/RoadGeom.h"
#include "Solve/StandBox.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/RoadEditTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

// NAMED, NOT ANONYMOUS - the tests module is a UNITY build, and an anonymous-namespace
// helper of a common name (LayRoad, FloorRect, ...) compiles alone and collides with another
// file's copy the moment both land in the same translation unit. StandPlotPlacementTest is
// this file's own name, so nothing else can share it by accident.
namespace StandPlotPlacementTest
{
	/** A real taxiway SEGMENT along Y, the way PlotPlaceToolTest's own LayRoad does - the
	 *  gesture snaps against the ROAD graph, not a guideline, so a test that laid only a
	 *  guideline would find nothing to draw a stand off. */
	void LayTaxiway(ARoadNetworkActor* Actor, double Y)
	{
		IRoadEditTarget* Target = Actor;
		const int32 West = Target->PlaceNode(FVector2D(-10000.0, Y));
		const int32 East = Target->PlaceNode(FVector2D(10000.0, Y));
		Target->ConnectNodes(West, East, ERoadKind::Taxiway, INDEX_NONE);
	}

	/** A rectangle sized exactly Letter's own floor: entrance edge on Y = EntranceY running
	 *  +X from X = 0, dragged Inward = +Y (away from a taxiway laid at a smaller Y). Writes
	 *  the entrance points a caller needs for PlaceStandInPlot's second and third arguments. */
	TArray<FVector2D> FloorRect(EIcaoCode Letter, double EntranceY, FVector2D& OutA, FVector2D& OutB)
	{
		const double Width = IcaoCode::StandWidthForLetter(Letter);
		const double Depth = IcaoCode::StandDepthForLetter(Letter);
		OutA = FVector2D(0.0, EntranceY);
		OutB = FVector2D(Width, EntranceY);
		return { OutA, OutB, FVector2D(Width, EntranceY + Depth), FVector2D(0.0, EntranceY + Depth) };
	}

	/** How many entities on Actor's network are both alive and IsStand() - not a bare count,
	 *  per Check-Architecture's is-plotted-not-depot rule and RoadEntity.h's own warning
	 *  against reading kind from anything but IsStand()/IsDepot(). */
	int32 LiveStandCount(const ARoadNetworkActor* Actor)
	{
		int32 Count = 0;
		for (const FEntityInstance& Entity : Actor->Network->GetEntities())
		{
			if (Entity.bAlive && Entity.IsStand()) { ++Count; }
		}
		return Count;
	}

	/**
	 * Two points spanning past BOTH side edges of Letter's floor rect (drawn at EntranceY,
	 * Inward = +Y), within a 400 uu strip well clear of the entrance edge and every corner -
	 * unambiguously inside the interior, however the crossing segment is classified.
	 *
	 * NOT NEAR THE ENTRANCE EDGE ITSELF (fix round 1, review finding): a segment straddling
	 * Y = EntranceY touches a boundary RoadGeom::PointInPolygon's own header says is "not
	 * guaranteed either answer", and - the finding that actually mattered - a stand's own
	 * entrance taxiway necessarily runs ALONG that edge, so a test crossing near it cannot
	 * tell "the check correctly exempted this" from "the check never looked here at all".
	 * Deep inside the box, spanning both sides, is unambiguous either way.
	 */
	void CrossingRoadEnds(EIcaoCode Letter, double EntranceY, FVector2D& OutWest, FVector2D& OutEast)
	{
		const double Width = IcaoCode::StandWidthForLetter(Letter);
		const double Depth = IcaoCode::StandDepthForLetter(Letter);
		const double CrossY = EntranceY + Depth - 200.0;
		OutWest = FVector2D(-500.0, CrossY);
		OutEast = FVector2D(Width + 500.0, CrossY);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotPlacesCodeCTest,
	"Airside.Present.StandPlot.PlacesCodeC",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotPlacesCodeCTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotPlacementTest;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();

	// The taxiway at Y = 0; the stand's entrance edge a full 1000 uu clear of it, so the
	// entrance edge and the taxiway's own centreline never touch, let alone cross.
	LayTaxiway(Actor, 0.0);

	FVector2D A, B;
	const TArray<FVector2D> Rect = FloorRect(EIcaoCode::C, 1000.0, A, B);
	const FVector2D Inward(0.0, 1.0);

	IRoadEditTarget* Target = Actor;
	const int32 Index = Target->PlaceStandInPlot(Rect, A, B, EPavement::Tarmac);
	if (!TestTrue(TEXT("a Code C floor rect beside the taxiway is placed"), Index != INDEX_NONE))
	{
		return false;
	}

	const FEntityInstance& Entity = Actor->Network->GetEntities()[Index];
	// ONE LINE, BOTH NAMES: Check-Architecture's is-plotted-not-depot rule wants IsPlotted()
	// never read alone - see RoadEntity.h's own warning against it meaning "is a depot".
	TestTrue(TEXT("it is a stand with a drawn outline"), Entity.IsStand() && Entity.IsPlotted());

	const double ExpectedHeading = RoadGeom::Bearing(Inward);
	TestTrue(TEXT("heading faces Inward, away from the taxiway"),
		FMath::IsNearlyEqual(Entity.Heading, ExpectedHeading, 1e-6));

	// THE STOP MARK ITSELF: StandBox::PoseFor's own derivation - since the far-side-entry task,
	// the tail sits EntranceSetback (MaxTailAft + wingtip clearance) in from the entrance
	// midpoint, along Inward, not Depth - NoseFwd - the slack that used to sit behind the tail
	// now lies ahead of the nose instead (StandBox::EntranceSetback).
	const FVector2D EntranceMid = (A + B) * 0.5;
	const double ExpectedOffset =
		StandBox::EntranceSetback(EIcaoCode::C, IcaoCode::FloorEnvelopeForLetter(EIcaoCode::C));
	const double ActualOffset = FVector2D::DotProduct(Entity.Position - EntranceMid, Inward);
	TestTrue(TEXT("the stop mark sits EntranceSetback in from the entrance, along Inward"),
		FMath::IsNearlyEqual(ActualOffset, ExpectedOffset, 1e-6));

	const TOptional<EIcaoCode> OutlineLetter = StandBox::LetterOf(Rect);
	if (!TestTrue(TEXT("the outline reads as a letter at all"), OutlineLetter.IsSet())) { return false; }
	TestEqual(TEXT("the outline reads as Code C"),
		FString(IcaoCode::ToLetter(*OutlineLetter)), FString(TEXT("C")));

	const FString CapturedLetter = IcaoCode::LetterForWingspan(Entity.DesignWingspan);
	TestEqual(TEXT("and the captured DesignWingspan reads back as the same letter"),
		CapturedLetter, FString(TEXT("C")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotRefusesTooSmallTest,
	"Airside.Present.StandPlot.RefusesTooSmall",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotRefusesTooSmallTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();

	// 20 x 20 m - narrower than Code A's own 35 m floor, exactly at A's 20 m depth floor.
	const TArray<FVector2D> Rect = {
		FVector2D(0.0, 0.0), FVector2D(2000.0, 0.0),
		FVector2D(2000.0, 2000.0), FVector2D(0.0, 2000.0) };

	IRoadEditTarget* Target = Actor;
	const int32 Index = Target->PlaceStandInPlot(Rect, Rect[0], Rect[1], EPavement::Tarmac);
	TestEqual(TEXT("a 20x20m rect is refused"), Index, INDEX_NONE);

	const FString Reason = Target->WhyStandRefused(Rect, EPavement::Tarmac);
	TestTrue(TEXT("the reason names the missing width"), Reason.Contains(TEXT("more width")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotRefusesOldCodeADepthTest,
	"Airside.Present.StandPlot.RefusesOldCodeADepth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotRefusesOldCodeADepthTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();

	// A DRAG THAT WAS A CODE A STAND (50 x 36 m) is now refused, and the refusal names DEPTH -
	// the dimension that is short - so the player is not left guessing (Review Focus 4). 50 m of
	// width clears B's own 5000 uu floor (A and B always shared it); 3600 uu of depth falls short
	// of B's 3950 uu floor - the gap the 2026-09-27 merge opened where the old Code A rectangle
	// used to read as its own letter.
	const TArray<FVector2D> OldA = { {0,0}, {5000,0}, {5000,3600}, {0,3600} };
	IRoadEditTarget* Target = Actor;
	const FString Why = Target->WhyStandRefused(OldA, EPavement::Tarmac);   // signature from Task 8; until then (Outline)
	TestTrue(TEXT("refused"), !Why.IsEmpty());
	TestTrue(TEXT("for depth"), Why.Contains(TEXT("deep")) || Why.Contains(TEXT("depth")));
	TestFalse(TEXT("and not for width, which is enough"), Why.Contains(TEXT("wide")) || Why.Contains(TEXT("width")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotRefusesOverlapTest,
	"Airside.Present.StandPlot.RefusesOverlap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotRefusesOverlapTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotPlacementTest;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();
	LayTaxiway(Actor, 0.0);

	FVector2D A, B;
	const TArray<FVector2D> First = FloorRect(EIcaoCode::C, 1000.0, A, B);
	IRoadEditTarget* Target = Actor;
	const int32 FirstIndex = Target->PlaceStandInPlot(First, A, B, EPavement::Tarmac);
	if (!TestTrue(TEXT("the first stand is placed"), FirstIndex != INDEX_NONE)) { return false; }

	// Shifted half a width over - well short of clearing the first rectangle, so the two
	// overlap rather than merely touch.
	const double Width = IcaoCode::StandWidthForLetter(EIcaoCode::C);
	FVector2D SecondA, SecondB;
	TArray<FVector2D> Second = FloorRect(EIcaoCode::C, 1000.0, SecondA, SecondB);
	for (FVector2D& P : Second) { P.X += Width * 0.5; }
	SecondA.X += Width * 0.5;
	SecondB.X += Width * 0.5;

	const int32 SecondIndex = Target->PlaceStandInPlot(Second, SecondA, SecondB, EPavement::Tarmac);
	TestEqual(TEXT("the overlapping second stand is refused"), SecondIndex, INDEX_NONE);

	const FString Reason = Target->WhyStandRefused(Second, EPavement::Tarmac);
	TestTrue(TEXT("the reason names the overlap"), Reason.Contains(TEXT("overlaps stand")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotAllowsServiceRoadInBackStripTest,
	"Airside.Present.StandPlot.AllowsServiceRoadInBackStrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotAllowsServiceRoadInBackStripTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotPlacementTest;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();

	FVector2D A, B;
	const TArray<FVector2D> Rect = FloorRect(EIcaoCode::C, 0.0, A, B);

	// A SERVICE road, spanning past both side edges of the box within a strip well inside the
	// interior (CrossingRoadEnds' own comment says why not near the entrance edge). Service
	// roads are exempt from the taxiway-crossing refusal - see URoadEditFacade::WhyStandRefused.
	FVector2D West, East;
	CrossingRoadEnds(EIcaoCode::C, 0.0, West, East);
	IRoadEditTarget* Target = Actor;
	const int32 WestIndex = Target->PlaceNode(West);
	const int32 EastIndex = Target->PlaceNode(East);
	Target->ConnectNodes(WestIndex, EastIndex, ERoadKind::ServiceRoad, INDEX_NONE);

	const int32 Index = Target->PlaceStandInPlot(Rect, A, B, EPavement::Tarmac);
	TestTrue(TEXT("the stand is placed despite the service road crossing its interior"),
		Index != INDEX_NONE);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotRefusesTaxiwayThroughInteriorTest,
	"Airside.Present.StandPlot.RefusesTaxiwayThroughInterior",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotRefusesTaxiwayThroughInteriorTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotPlacementTest;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();

	FVector2D A, B;
	const TArray<FVector2D> Rect = FloorRect(EIcaoCode::C, 0.0, A, B);

	// THE SAME GEOMETRY AllowsServiceRoadInBackStrip uses, laid as a TAXIWAY instead - the
	// pair together proves WhyStandRefused's crossing check discriminates by kind, rather
	// than either refusing everything or (the bug this test exists to catch, per review) never
	// actually finding anything crossing at all.
	FVector2D West, East;
	CrossingRoadEnds(EIcaoCode::C, 0.0, West, East);
	IRoadEditTarget* Target = Actor;
	const int32 WestIndex = Target->PlaceNode(West);
	const int32 EastIndex = Target->PlaceNode(East);
	Target->ConnectNodes(WestIndex, EastIndex, ERoadKind::Taxiway, INDEX_NONE);

	const int32 Index = Target->PlaceStandInPlot(Rect, A, B, EPavement::Tarmac);
	TestEqual(TEXT("a taxiway through the interior refuses the stand"), Index, INDEX_NONE);

	const FString Reason = Target->WhyStandRefused(Rect, EPavement::Tarmac);
	TestTrue(TEXT("the reason names the taxiway"), Reason.Contains(TEXT("a taxiway crosses")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotUndoRemovesTest,
	"Airside.Present.StandPlot.UndoRemoves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotUndoRemovesTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotPlacementTest;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();
	LayTaxiway(Actor, 0.0);

	FVector2D A, B;
	const TArray<FVector2D> Rect = FloorRect(EIcaoCode::C, 1000.0, A, B);
	IRoadEditTarget* Target = Actor;
	const int32 Index = Target->PlaceStandInPlot(Rect, A, B, EPavement::Tarmac);
	if (!TestTrue(TEXT("the stand is placed"), Index != INDEX_NONE)) { return false; }
	if (!TestEqual(TEXT("one live stand before undo"), LiveStandCount(Actor), 1)) { return false; }

	if (!TestTrue(TEXT("there is something to undo"), Actor->CanUndo())) { return false; }
	if (!TestTrue(TEXT("undo succeeds"), Actor->Undo())) { return false; }

	TestEqual(TEXT("no live stand after undo"), LiveStandCount(Actor), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotEveryLetterBuildsTest,
	"Airside.Present.StandPlot.EveryLetterBuilds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotEveryLetterBuildsTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotPlacementTest;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();

	// REPLACES FStandPlotUnfitLetterRefusedTest (far-side-entry spec, 2026-09-26): A and B used to
	// be refused here because their bays were laid for the fuel truck - the first measured
	// table named them the two that did not fit. Every letter now has its own DESIGN VEHICLE
	// (UAirsideSettings::ResolveStandDesignVehicle: the utility tow for A/B, the fuel truck for
	// C-F) and A/B's floors were widened for the tow's lane, so
	// Airside.Entities.StandLayoutEveryLetterBuilds already pins every template fitting its own
	// floor at the Model/ level - WhyStandRefused's "cannot be built yet" branch this test used
	// to exercise is UNREACHABLE from here now, and asserting it would just pin a template that
	// happens to still be too small rather than the placement path. This test asks the question
	// one level up: the actor's own cache resolves a real UEntityDefinition for every letter, so
	// nothing between the template and PlaceStandInPlot silently drops one.
	//
	// CODE A DROPPED (2026-09-27 merge): ResolveStandDefinitionFor(A) is unreachable now by
	// construction - LetterOf never answers A any more, so FitsItsLetter(_, A) can never be true
	// (see IcaoCode.h's StandLetterFor) and this cache returns null for A on every call. Left
	// unreachable rather than special-cased, per the merge's own ruling.
	for (const EIcaoCode Letter : { EIcaoCode::B, EIcaoCode::C, EIcaoCode::D, EIcaoCode::E, EIcaoCode::F })
	{
		TestNotNull(*FString::Printf(TEXT("Code %s resolves a stand definition"), IcaoCode::ToLetter(Letter)),
			Actor->ResolveStandDefinitionFor(Letter));
	}

	return true;
}

// FIX ROUND 1: PlacesCodeC alone only proves the one letter with an authored asset. D, E and
// F all go through ResolveStandDefinitionFor's lazily-built cache instead - this loop is the
// same round trip for each of them, one stand per letter so none can overlap another.
//
// A AND B JOINED THE LOOP 2026-09-26 (far-side-entry spec): they used to be the two letters the
// first measured table named as not fitting their own floor - FStandPlotUnfitLetterRefusedTest pinned
// that - and now fit like every other letter (their own design vehicle, a widened floor for
// its lane), so the same round trip that already proves D/E/F proves them too.
//
// A DROPPED FROM THE LOOP AGAIN, 2026-09-27 (merge into B): the loop's own assertion is that a
// stand drawn at Letter's floor reads BACK as Letter, and a rectangle at A's floor now reads as
// B (IcaoCode::StandLetterFor) - that is B's case already in this same array, not a second one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotPlacesOtherLettersTest,
	"Airside.Present.StandPlot.PlacesOtherLetters",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotPlacesOtherLettersTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotPlacementTest;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();

	const EIcaoCode Fits[] = { EIcaoCode::B, EIcaoCode::D, EIcaoCode::E, EIcaoCode::F };
	IRoadEditTarget* Target = Actor;

	for (const EIcaoCode Letter : Fits)
	{
		FVector2D A, B;
		// Spread far enough apart (20 km per letter) that no two of the widest floors (F's is
		// under 190 m) can ever overlap, whatever the table's figures do later.
		const TArray<FVector2D> Rect =
			FloorRect(Letter, 20000.0 * (static_cast<double>(Letter) + 1.0), A, B);

		const int32 Index = Target->PlaceStandInPlot(Rect, A, B, EPavement::Tarmac);
		if (!TestTrue(*FString::Printf(TEXT("Code %s's own floor is placed"), IcaoCode::ToLetter(Letter)),
				Index != INDEX_NONE))
		{
			continue;
		}

		const FEntityInstance& Entity = Actor->Network->GetEntities()[Index];
		const TOptional<EIcaoCode> OutlineLetter = StandBox::LetterOf(Rect);
		const FString CapturedLetter = IcaoCode::LetterForWingspan(Entity.DesignWingspan);
		const FString ExpectedLetter = IcaoCode::ToLetter(Letter);

		TestTrue(*FString::Printf(TEXT("Code %s's outline reads back as %s"),
				IcaoCode::ToLetter(Letter), *ExpectedLetter),
			OutlineLetter.IsSet() && FString(IcaoCode::ToLetter(*OutlineLetter)) == ExpectedLetter);
		TestEqual(*FString::Printf(TEXT("Code %s's captured DesignWingspan reads back as %s"),
				IcaoCode::ToLetter(Letter), *ExpectedLetter),
			CapturedLetter, ExpectedLetter);
	}

	return true;
}

/**
 * Task 6: a stand old enough to have loaded with NO outline at all - a level saved before
 * Ruling 6 gave every point-placed stand one at placement - gets the Code C box its pose
 * implies, the moment EnsureStandOutlines runs (URoadNetwork::PostLoad in production).
 *
 * PLACEMENT ITSELF ALREADY GIVES AN OUTLINE, since Ruling 6, so this test builds its "legacy"
 * case by clearing the one placement just wrote (FRoadNetworkTestAccess::SetEntityOutlineForTest)
 * rather than by placing and expecting emptiness - the only way left to construct the case
 * EnsureStandOutlines exists for.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandOutlineLegacyGetsCodeCBoxTest,
	"Airside.Model.StandOutline.LegacyGetsCodeCBox",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandOutlineLegacyGetsCodeCBoxTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();

	IRoadEditTarget* Target = Actor;
	const double Heading = FMath::DegreesToRadians(90.0);
	const int32 Index = Target->PlaceStand(FVector2D(1000.0, 2000.0), Heading);
	if (!TestTrue(TEXT("a stand is placed"), Index != INDEX_NONE)) { return false; }
	const FEntityInstanceId StandId = Actor->Network->EntityIdAt(Index);

	// BACK TO EMPTY, simulating a save from before Ruling 6 - see the test's own comment above.
	FRoadNetworkTestAccess NetworkAccess(*Actor->Network);
	if (!TestTrue(TEXT("the outline is cleared to stand in for pre-Task-6 save data"),
			NetworkAccess.SetEntityOutlineForTest(StandId, {})))
	{
		return false;
	}

	const FEntityInstance* Entity = Actor->Network->GetEntity(StandId);
	if (!TestNotNull(TEXT("the stand resolves"), Entity)) { return false; }
	if (!TestEqual(TEXT("outline really is empty before the fix runs"), Entity->Outline.Num(), 0))
	{
		return false;
	}

	TestEqual(TEXT("EnsureStandOutlines fixes exactly the one legacy stand"),
		Actor->Network->EnsureStandOutlines(), 1);

	TArray<FVector2D> Expected;
	StandBox::FStandPose Pose;
	Pose.Position = Entity->Position;
	Pose.Facing = FVector2D(FMath::Cos(Entity->Heading), FMath::Sin(Entity->Heading));
	StandBox::BoxAt(Pose, EIcaoCode::C, IcaoCode::FloorEnvelopeForLetter(EIcaoCode::C), Expected);
	TestTrue(TEXT("the outline is the Code C box at the stand's own pose"),
		Entity->Outline == Expected);

	TestTrue(TEXT("the stop mark sits inside the outline it was just given"),
		RoadGeom::PointInPolygon(Entity->Outline, Entity->Position));

	TestEqual(TEXT("a second pass finds nothing left to fix - idempotent"),
		Actor->Network->EnsureStandOutlines(), 0);

	// A DEPOT NEVER MEETS IsStand() - EnsureStandOutlines' own guard, GiveStandOutlineIfMissing -
	// so one placed the legacy way and never drawn stays outline-less through both passes above.
	const int32 DepotIndex =
		Target->PlaceEntity(FVector2D(9000.0, 9000.0), 0.0, EPlaceableEntity::FuelDepot);
	if (!TestTrue(TEXT("a depot is placed"), DepotIndex != INDEX_NONE)) { return false; }
	const FEntityInstance& Depot = Actor->Network->GetEntities()[DepotIndex];
	TestTrue(TEXT("the depot is never a stand"), !Depot.IsStand());
	TestEqual(TEXT("and EnsureStandOutlines left its outline alone"), Depot.Outline.Num(), 0);

	return true;
}

/**
 * Ruling 6: outlines are given AT PLACEMENT, not only on load - a stand point-placed through
 * IRoadEditTarget::PlaceEntity is IsPlotted() from the moment it is placed, with the same Code
 * C box EnsureStandOutlines would have given it on the next load. A depot placed the identical
 * way stays outline-less, because only IsStand() ever gets one.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandOutlinePointPlacedStandGetsOutlineTest,
	"Airside.Model.StandOutline.PointPlacedStandGetsOutline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandOutlinePointPlacedStandGetsOutlineTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();

	IRoadEditTarget* Target = Actor;
	const double Heading = FMath::DegreesToRadians(30.0);
	const int32 StandIndex =
		Target->PlaceEntity(FVector2D(3000.0, -1500.0), Heading, EPlaceableEntity::Stand);
	if (!TestTrue(TEXT("a stand is placed"), StandIndex != INDEX_NONE)) { return false; }

	const FEntityInstance& Stand = Actor->Network->GetEntities()[StandIndex];
	// ONE LINE, BOTH NAMES - Check-Architecture's is-plotted-not-depot rule, same as
	// PlacesCodeC above.
	TestTrue(TEXT("it is a stand, plotted from the moment it is placed"),
		Stand.IsStand() && Stand.IsPlotted());

	TArray<FVector2D> Expected;
	StandBox::FStandPose Pose;
	Pose.Position = Stand.Position;
	Pose.Facing = FVector2D(FMath::Cos(Stand.Heading), FMath::Sin(Stand.Heading));
	StandBox::BoxAt(Pose, EIcaoCode::C, IcaoCode::FloorEnvelopeForLetter(EIcaoCode::C), Expected);
	TestTrue(TEXT("its outline is the Code C box at its own pose"), Stand.Outline == Expected);

	// A DEPOT PLACED THE SAME WAY STAYS OUTLINE-LESS - GiveStandOutlineIfMissing's IsStand()
	// guard, exercised through the real tool-facing entry point rather than URoadNetwork direct.
	const int32 DepotIndex =
		Target->PlaceEntity(FVector2D(9000.0, 9000.0), 0.0, EPlaceableEntity::FuelDepot);
	if (!TestTrue(TEXT("a depot is placed"), DepotIndex != INDEX_NONE)) { return false; }
	const FEntityInstance& Depot = Actor->Network->GetEntities()[DepotIndex];
	TestTrue(TEXT("it is a depot, and stays unplotted"), Depot.IsDepot() && !Depot.IsPlotted());
	TestEqual(TEXT("its outline stays empty"), Depot.Outline.Num(), 0);

	return true;
}

/**
 * FINAL REVIEW I5: A DRAWN STAND'S LEAD-IN IS SIZED BY ITS OWN LETTER. D, E and F have no
 * design aircraft (UAirsideSettings::ResolveLargestAircraftOfLetter returns null for every
 * letter today), and FAnchorLink read both the lead-in's radius and its span limit off that
 * aircraft - so a drawn F stand was painted with Code C's 25 m radius and no span limit at
 * all. The letter the stand was drawn as is captured (DesignWingspan), and is what admission
 * reads; the lead-in now reads it too.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotLeadInSizedByLetterTest,
	"Airside.Present.StandPlot.LeadInSizedByLetter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotLeadInSizedByLetterTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotPlacementTest;

	// THE PENDING LINK, measured before any join: FAnchorLink::Gather is where the radius and
	// the limit are decided, so reading them there asks exactly the question, with no road
	// geometry downstream able to hide the figure.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		UEntityDefinition* FStand = UEntityDefinition::MakeStandTransient(EIcaoCode::F);
		if (!TestTrue(TEXT("the Code F template has no design aircraft - the case under test"),
			FStand->DesignAircraft == nullptr)) { return false; }

		const FVector2D A(0.0, 0.0);
		const FVector2D B(IcaoCode::StandWidthForLetter(EIcaoCode::F), 0.0);
		const FLetterEnvelope FEnvelope = IcaoCode::FloorEnvelopeForLetter(EIcaoCode::F);
		const StandBox::FStandPose Pose = StandBox::PoseFor(A, B, FVector2D(0.0, 1.0), EIcaoCode::F, FEnvelope);
		FEntityPlacement Placement;
		Placement.Definition = FStand;
		Placement.Anchors = FStand->Anchors;
		Placement.Position = Pose.Position;
		Placement.Heading = RoadGeom::Bearing(Pose.Facing);
		Placement.PoseRole = FStand->PoseRole;
		StandBox::BoxAt(Pose, EIcaoCode::F, FEnvelope, Placement.Outline);
		Placement.DesignWingspan = IcaoCode::DesignSpanForLetter(EIcaoCode::F);
		const FEntityInstanceId Placed = Net->PlaceEntity(Placement);
		const FEntityInstance* Stand = Net->GetEntity(Placed);
		if (!TestNotNull(TEXT("the F stand is placed"), Stand)) { return false; }

		TArray<FPendingLink> Pending;
		TSet<FGuidelineNodeId> AnchorNodes;
		FAnchorLink::Gather(*Net, FAnchorLink::DefaultMaxLeadIn, FAnchorLink::DefaultServiceLinkRadius,
			Pending, AnchorNodes);
		const FPendingLink* LeadIn = Pending.FindByPredicate(
			[Stand](const FPendingLink& Link) { return Link.Node == Stand->PoseNode; });
		if (!TestNotNull(TEXT("the pose casts a lead-in"), LeadIn)) { return false; }

		TestEqual(TEXT("the lead-in sweeps at Code F's radius, not Code C's"),
			LeadIn->Radius, IcaoCode::RadiusForLetter(EIcaoCode::F));
		TestEqual(TEXT("and limits span to the widest Code F - the letter admission reads, not unlimited"),
			LeadIn->MaxWingspan, IcaoCode::MaxWingspanForLetter(EIcaoCode::F));
	}

	// THE SAME STAND, DRAWN THROUGH THE FACADE, TAKES AN A380 - the span limit above must not
	// be one that refuses the very aircraft the letter admits.
	{
		FAirsideTestWorld TestWorld;
		if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
		ARoadNetworkActor* Actor = TestWorld.Actor;
		if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
		Actor->ClearNetwork();

		// LONGER THAN LayTaxiway's 200 m: an F stand's entrance is ~190 m wide, and its
		// lead-in must meet the taxiway well clear of the far node for the entry sweeps.
		IRoadEditTarget* Target = Actor;
		const int32 West = Target->PlaceNode(FVector2D(-40000.0, 0.0));
		const int32 East = Target->PlaceNode(FVector2D(60000.0, 0.0));
		Target->ConnectNodes(West, East, ERoadKind::Taxiway, INDEX_NONE);

		FVector2D A, B;
		const TArray<FVector2D> Rect = FloorRect(EIcaoCode::F, 1000.0, A, B);
		const int32 Index = Target->PlaceStandInPlot(Rect, A, B, EPavement::Tarmac);
		if (!TestTrue(TEXT("a Code F floor rect beside the taxiway is placed"), Index != INDEX_NONE)) { return false; }
		Actor->RebuildMesh();
		const FGuidelineNodeId StandPose = Actor->Network->GetEntities()[Index].PoseNode;

		// FROM THE TAXIWAY'S WEST END - the nearest guideline node to it, whatever the builder
		// named it.
		FGuidelineNodeId From;
		double BestDistance = TNumericLimits<double>::Max();
		const TArray<FGuidelineNode>& Nodes = Actor->Network->GetGuidelineNodes();
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			const FGuidelineNodeId Id = Actor->Network->GuidelineNodeIdAt(NodeIndex);
			if (!Id.IsSet()) { continue; }
			const double Distance = FVector2D::Distance(Nodes[NodeIndex].Position, FVector2D(-39000.0, 0.0));
			if (Distance < BestDistance) { BestDistance = Distance; From = Id; }
		}
		if (!TestTrue(TEXT("a taxiway node to start from"), From.IsSet())) { return false; }

		FAirframe A380;
		A380.Wingspan = 7980.0;   // the A380-800's published 79.8 m
		const FGuidelineNodeId Chosen = ArrivalPlanner::ChooseStand(*Actor->Network, From, A380, nullptr, 0);
		TestTrue(TEXT("an A380 is given the drawn F stand"), Chosen.IsSet() && Chosen == StandPose);
	}
	return true;
}

/**
 * FINAL REVIEW (cheap minor): a depot plot may not be laid over a stand. WhyStandRefused
 * already refused a STAND over a depot; the reverse had no check at all, so a depot drawn
 * across a stand's apron placed, fenced and seated its tanks under the parked aircraft. The
 * same (edge-tolerant) OutlinesOverlap answers both directions, so a depot flush against a
 * stand's edge still places.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotDepotRefusesStandOverlapTest,
	"Airside.Present.StandPlot.DepotRefusesStandOverlap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotDepotRefusesStandOverlapTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotPlacementTest;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->ClearNetwork();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();

	FVector2D A, B;
	const TArray<FVector2D> Stand = FloorRect(EIcaoCode::C, 1000.0, A, B);
	IRoadEditTarget* Target = Actor;
	if (!TestTrue(TEXT("a Code C stand is placed"), Target->PlaceStandInPlot(Stand, A, B, EPavement::Tarmac) != INDEX_NONE)) { return false; }

	// A 30 x 24 m plot - DepotIsPickedByItsGround's own - with its frontage on Y = FrontY.
	const auto Plot = [](double X0, double FrontY)
	{
		return TArray<FVector2D>{ FVector2D(X0, FrontY), FVector2D(X0 + 3000.0, FrontY),
			FVector2D(X0 + 3000.0, FrontY + 2400.0), FVector2D(X0, FrontY + 2400.0) };
	};
	const TArray<EDepotModule> Modules = { EDepotModule::Shed, EDepotModule::Tank };

	// ACROSS THE STAND'S MIDDLE: refused.
	const TArray<FVector2D> Over = Plot(1000.0, 2000.0);
	TestEqual(TEXT("a depot plot laid over a stand is refused"),
		Target->PlaceEntityInPlot(Over, Over[0], Over[1], Modules, EPlaceableEntity::FuelDepot), INDEX_NONE);

	// FLUSH AGAINST THE STAND'S WEST EDGE: sharing an edge is not overlapping - and this is
	// also the control that proves the refusal above was for the overlap, not for anything
	// else about a plot of this size.
	const TArray<FVector2D> Flush = Plot(-3000.0, 2000.0);
	TestTrue(TEXT("the same plot flush beside the stand is placed"),
		Target->PlaceEntityInPlot(Flush, Flush[0], Flush[1], Modules, EPlaceableEntity::FuelDepot) != INDEX_NONE);
	return true;
}


/**
 * FINAL REVIEW C2/I7, THE LEVEL HALF: a D stand's definition is RF_Transient, so a level save
 * writes the stand's reference to it as null - and the actor's load path (PostRegisterAllComponents,
 * which runs after serialisation for a level load and a PIE duplicate alike) must put it back
 * before the rebuild that decides whether the stand is joined to anything. Re-registering is
 * that same path, as MeshFreshnessTest uses it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotRebindsAfterLevelLoadTest,
	"Airside.Present.StandPlot.RebindsAfterLevelLoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotRebindsAfterLevelLoadTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotPlacementTest;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->ClearNetwork();
	LayTaxiway(Actor, 0.0);

	FVector2D A, B;
	const TArray<FVector2D> Rect = FloorRect(EIcaoCode::D, 1000.0, A, B);
	IRoadEditTarget* Target = Actor;
	const int32 Index = Target->PlaceStandInPlot(Rect, A, B, EPavement::Tarmac);
	if (!TestTrue(TEXT("a Code D stand is placed"), Index != INDEX_NONE)) { return false; }
	const FEntityInstanceId Id = Actor->Network->EntityIdAt(Index);

	UEntityDefinition* Placed = Actor->Network->GetEntity(Id)->Definition;
	if (!TestNotNull(TEXT("placed with a definition"), Placed)) { return false; }
	TestTrue(TEXT("which is RF_Transient - a level save writes the reference as null, never a frozen copy"),
		Placed->HasAnyFlags(RF_Transient));

	// WHAT A SAVED LEVEL LOADS AS: the reference nulled by the save.
	Actor->Network->SetEntityDefinition(Id, nullptr);

	Actor->ReregisterAllComponents();

	const FEntityInstance* Loaded = Actor->Network->GetEntity(Id);
	if (!TestNotNull(TEXT("the stand is still there"), Loaded)) { return false; }
	TestNotNull(TEXT("the load path rebound its definition from the outline's letter"), Loaded->Definition.Get());
	const FGuidelineNode* Pose = Actor->Network->GetGuidelineNode(Loaded->PoseNode);
	TestTrue(TEXT("BEFORE the rebuild - its lead-in is joined, so the Inspector says reachable"),
		Pose != nullptr && Pose->Incident.Num() > 0);
	return true;
}

/**
 * AN OLD SAVE'S STAND IS RE-POSED ON LOAD (spec §1; final review, 2026-09-27). A Code C stand
 * drawn at the old 55 m floor, saved with the stop mark where that geometry put it (Depth -
 * MaxNoseFwd in from the entrance), now reads as Code B: the depth floors rose on 2026-09-26. The
 * rebind re-pointed it at B's definition but left the old pose, so B's bays - laid off the pose -
 * sat 1100 uu beyond the drawn far edge. What a load must do is re-derive the pose from the
 * outline with PoseFor, for the letter the outline reads as NOW.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotOldPoseRederivedOnLoadTest,
	"Airside.Present.StandPlot.OldPoseRederivedOnLoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotOldPoseRederivedOnLoadTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotPlacementTest;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->ClearNetwork();
	LayTaxiway(Actor, 0.0);

	// THE OLD SAVE: C's width, the old 55 m depth, entrance on y = 1000 dragged +Y.
	constexpr double EntranceY = 1000.0;
	constexpr double Width = 5900.0;
	constexpr double OldDepth = 5500.0;
	const TArray<FVector2D> Outline = {
		FVector2D(0.0, EntranceY), FVector2D(Width, EntranceY),
		FVector2D(Width, EntranceY + OldDepth), FVector2D(0.0, EntranceY + OldDepth) };
	if (!TestEqual(TEXT("the premise: the old 55 m C box reads as Code B now"),
			StandBox::LetterOf(Outline).Get(EIcaoCode::F), EIcaoCode::B))
	{
		return false;
	}

	UEntityDefinition* OldDefinition = Actor->ResolveStandDefinitionFor(EIcaoCode::C);
	if (!TestNotNull(TEXT("Code C has a definition"), OldDefinition)) { return false; }
	const FVector2D Facing(0.0, 1.0);
	FEntityPlacement Placement;
	Placement.Definition = OldDefinition;
	Placement.Anchors = OldDefinition->Anchors;
	// THE OLD STOP MARK, the nose MaxNoseFwd short of the far edge - where the pre-2026-09-26
	// PoseFor put it.
	Placement.Position = FVector2D(0.5 * Width, EntranceY)
		+ Facing * (OldDepth - IcaoCode::FloorEnvelopeForLetter(EIcaoCode::C).MaxNoseFwd);
	Placement.Heading = RoadGeom::Bearing(Facing);
	Placement.DesignWingspan = IcaoCode::DesignSpanForLetter(EIcaoCode::C);
	Placement.PoseRole = OldDefinition->PoseRole;
	Placement.Trucks = OldDefinition->Trucks;
	Placement.Outline = Outline;
	const FEntityInstanceId Id = Actor->Network->PlaceEntity(Placement);
	if (!TestTrue(TEXT("the old stand is in the model"), Id.IsSet())) { return false; }

	// THE LOAD PATH - see RebindsAfterLevelLoad.
	Actor->ReregisterAllComponents();

	const FEntityInstance* Loaded = Actor->Network->GetEntity(Id);
	if (!TestNotNull(TEXT("the stand is still there"), Loaded)) { return false; }
	UEntityDefinition* BDefinition = Actor->ResolveStandDefinitionFor(EIcaoCode::B);
	TestTrue(TEXT("rebound to Code B's definition"), Loaded->Definition.Get() == BDefinition && BDefinition != nullptr);

	const StandBox::FStandPose Expected = StandBox::PoseFor(Outline[0], Outline[1],
		PlotYard::InwardOf(Outline, Outline[0], Outline[1]), EIcaoCode::B,
		UAirsideSettings::ResolveLetterEnvelope(EIcaoCode::B));
	TestTrue(*FString::Printf(TEXT("the pose is B's PoseFor off the outline: (%.1f, %.1f), expected (%.1f, %.1f)"),
			Loaded->Position.X, Loaded->Position.Y, Expected.Position.X, Expected.Position.Y),
		Loaded->Position.Equals(Expected.Position, 0.01));
	TestTrue(TEXT("facing unchanged - the outline did not turn"),
		FMath::IsNearlyEqual(Loaded->Heading, RoadGeom::Bearing(Expected.Facing), 1.0e-9));

	// AND ITS LETTER FOR ADMISSION (re-review, 2026-09-27): the captured DesignWingspan is what
	// UStandAllocator, ArrivalPlanner, the inspector and the marking read, and it was left at C's -
	// a stand drawn and re-posed as B went on admitting, labelling and painting a C.
	TestEqual(TEXT("the captured span reads as Code B, the letter an allocator sees"),
		IcaoCode::CodeForWingspan(Loaded->DesignWingspan), EIcaoCode::B);
	TestFalse(TEXT("so a Code C airframe is refused there (IcaoCode::StandAdmits, the allocator's test)"),
		IcaoCode::StandAdmits(Loaded->DesignWingspan, IcaoCode::DesignSpanForLetter(EIcaoCode::C)));
	TestTrue(TEXT("and a Code B airframe is still admitted"),
		IcaoCode::StandAdmits(Loaded->DesignWingspan, IcaoCode::DesignSpanForLetter(EIcaoCode::B)));

	const FGuidelineNode* PoseNode = Actor->Network->GetGuidelineNode(Loaded->PoseNode);
	TestTrue(TEXT("the pose node moved with it - it is what an arrival is routed to"),
		PoseNode != nullptr && PoseNode->Position.Equals(Loaded->Position, 0.01));

	// B'S ANCHORS, at B's offsets from the new pose: the rebind re-captures them, because C's sit
	// where C's wing band put them.
	if (BDefinition != nullptr)
	{
		const double Cos = FMath::Cos(Loaded->Heading);
		const double Sin = FMath::Sin(Loaded->Heading);
		for (const FEntityAnchor& Anchor : BDefinition->Anchors)
		{
			const FVector2D World(
				Loaded->Position.X + Anchor.LocalPosition.X * Cos - Anchor.LocalPosition.Y * Sin,
				Loaded->Position.Y + Anchor.LocalPosition.X * Sin + Anchor.LocalPosition.Y * Cos);
			const FGuidelineNode* Node = Actor->Network->GetAnchorNode(Id, Anchor.Id);
			TestTrue(*FString::Printf(TEXT("anchor '%s' sits at B's offset (%.0f, %.0f)"),
					*Anchor.Id.ToString(), World.X, World.Y),
				Node != nullptr && Node->Position.Equals(World, 0.01));
		}
	}

	// AND THE BAYS THE REBUILD LAID ARE INSIDE WHAT WAS DRAWN - the symptom the player saw.
	const FStandLayoutBuild::FResult Built = FStandLayoutBuild::Build(*Actor->Network);
	const TArray<FGuidelineNodeId>* Entries = Built.Entries.Find(Id);
	if (TestTrue(TEXT("the stand's bays are laid"), Entries != nullptr && Entries->Num() > 0))
	{
		for (const FGuidelineNodeId& Entry : *Entries)
		{
			const FGuidelineNode* Node = Actor->Network->GetGuidelineNode(Entry);
			TestTrue(*FString::Printf(TEXT("entry %d at (%.0f, %.0f) is inside the drawn outline"),
					Entry.Index, Node != nullptr ? Node->Position.X : 0.0, Node != nullptr ? Node->Position.Y : 0.0),
				Node != nullptr && RoadGeom::PointInPolygon(Outline, Node->Position));
		}
	}
	return true;
}

namespace StandPlotPlacementTest
{
	/** FLogLineSpy for WARNINGS: the base keeps Log verbosity only, and the corner warning this
	 *  file asserts on is a Warning. Deriving keeps the base's unbuffered override (#216). */
	struct FWarningLineSpy : public FLogLineSpy
	{
		explicit FWarningLineSpy(FName InCategory) : FLogLineSpy(InCategory) {}

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& InCategory) override
		{
			if (InCategory == Category && Verbosity == ELogVerbosity::Warning)
			{
				++Count;
				CapturedLines.Add(FString(V));
			}
		}
	};

	/** One lead-in off a declared entry, as laid: where its sweeps meet the road, how far toward
	 *  the far lane any of it reaches, and whether it is a HARD join (no sweeps - its end sits on
	 *  the road itself, which Join's no-room branch makes). */
	struct FLeadIn
	{
		TArray<double> SweepEndYs;
		double Deepest = -TNumericLimits<double>::Max();
		bool bHardJoin = false;
	};

	/** A stand drawn at Letter's floor off a taxiway at y = 0, entrance on y = 1000 facing +Y, and
	 *  a real two-lane SERVICE ROAD laid through the facade along X with its centre at RoadY. */
	struct FStandAndRoad
	{
		FEntityInstanceId Stand;
		double FarEdgeY = 0.0;
		double HalfWidth = 0.0;
		TArray<double> LaneYs;
		TMap<int32, TArray<FLeadIn>> LeadsByEntry;
	};

	bool BuildStandAndRoad(FAutomationTestBase& Test, ARoadNetworkActor* Actor, EIcaoCode Letter,
		TFunctionRef<double(double FarEdgeY, double HalfWidth)> RoadYFor, FStandAndRoad& Out)
	{
		Actor->ClearNetwork();
		LayTaxiway(Actor, 0.0);
		FVector2D A, B;
		const TArray<FVector2D> Rect = FloorRect(Letter, 1000.0, A, B);
		IRoadEditTarget* Target = Actor;
		const int32 Index = Target->PlaceStandInPlot(Rect, A, B, EPavement::Tarmac);
		if (!Test.TestTrue(TEXT("the stand is placed"), Index != INDEX_NONE)) { return false; }
		Out.Stand = Actor->Network->EntityIdAt(Index);

		// THE GAME'S OWN SERVICE ROAD PROFILE, through the actor's one resolver - the one
		// URoadEditFacade::MakeTunables hands the plot ghost for its ServiceEdge offset.
		const URoadProfile* Profile = Actor->ResolveServiceRoadProfile();
		if (!Test.TestNotNull(TEXT("a service road profile resolves"), Profile)) { return false; }
		Out.HalfWidth = Profile->GetMaxHalfWidth();
		Out.FarEdgeY = 1000.0 + IcaoCode::StandDepthForLetter(Letter);
		const double RoadY = RoadYFor(Out.FarEdgeY, Out.HalfWidth);
		const int32 West = Target->PlaceNode(FVector2D(-12000.0, RoadY));
		const int32 East = Target->PlaceNode(FVector2D(12000.0, RoadY));
		if (!Test.TestTrue(TEXT("a service road connects"), Target->ConnectNodes(West, East, ERoadKind::ServiceRoad))) { return false; }

		URoadNetwork& Net = *Actor->Network;
		const FRoadSegmentId Road = Net.SegmentIdAt(Net.GetSegments().Num() - 1);
		for (const FGuidelineEdge& Edge : Net.GetGuidelineEdges())
		{
			const FGuidelineNode* NodeA = Edge.bAlive ? Net.GetGuidelineNode(Edge.A) : nullptr;
			if (NodeA != nullptr && Edge.DerivedFrom == Road)
			{
				bool bKnown = false;
				for (const double Y : Out.LaneYs) { bKnown |= FMath::IsNearlyEqual(Y, NodeA->Position.Y, 1.0); }
				if (!bKnown) { Out.LaneYs.Add(NodeA->Position.Y); }
			}
		}
		Out.LaneYs.Sort();
		Test.AddInfo(FString::Printf(TEXT("Code %s: far edge y %.0f, road half-width %.0f, road centre y %.0f, lanes %s"),
			IcaoCode::ToLetter(Letter), Out.FarEdgeY, Out.HalfWidth, RoadY,
			*FString::JoinBy(Out.LaneYs, TEXT(", "), [](double Y) { return FString::Printf(TEXT("%.0f"), Y); })));

		const FStandLayoutBuild::FResult Built = FStandLayoutBuild::Build(Net);
		const TArray<FGuidelineNodeId>* Entries = Built.Entries.Find(Out.Stand);
		if (!Test.TestTrue(TEXT("the stand's bays are laid"), Entries != nullptr && Entries->Num() > 0)) { return false; }

		// A LINK IS UNOWNED AND UNDERIVED - neither the stand's layout nor a road's lane.
		auto Unowned = [&Net](FGuidelineEdgeId EdgeId)
		{
			const FGuidelineEdge* Edge = Net.GetGuidelineEdge(EdgeId);
			return Edge != nullptr && Edge->bAlive && !Edge->StandGeometryOwner.IsSet() && !Edge->DerivedFrom.IsSet();
		};
		for (const FGuidelineNodeId& Entry : *Entries)
		{
			TArray<FLeadIn>& Leads = Out.LeadsByEntry.Add(Entry.Index);
			for (const FGuidelineEdgeId& LeadId : Net.GetGuidelineNode(Entry)->Incident)
			{
				if (!Unowned(LeadId)) { continue; }
				const FGuidelineEdge* LeadEdge = Net.GetGuidelineEdge(LeadId);
				const FGuidelineNodeId LeadEndId = LeadEdge->A == Entry ? LeadEdge->B : LeadEdge->A;
				FLeadIn Lead;
				TArray<FGuidelineEdgeId> Link = { LeadId };
				for (const FGuidelineEdgeId& EdgeId : Net.GetGuidelineNode(LeadEndId)->Incident)
				{
					if (EdgeId != LeadId && Unowned(EdgeId)) { Link.Add(EdgeId); }
				}
				// NO SWEEPS: the lead-in's end is ON the road - the hard join.
				Lead.bHardJoin = Link.Num() == 1;
				if (Lead.bHardJoin)
				{
					Lead.SweepEndYs.Add(Net.GetGuidelineNode(LeadEndId)->Position.Y);
				}
				for (const FGuidelineEdgeId& EdgeId : Link)
				{
					TArray<FVector2D> Points;
					Net.SampleGuideline(EdgeId, Points);
					for (const FVector2D& Point : Points) { Lead.Deepest = FMath::Max(Lead.Deepest, Point.Y); }
					if (EdgeId == LeadId) { continue; }
					const FGuidelineEdge* Sweep = Net.GetGuidelineEdge(EdgeId);
					Lead.SweepEndYs.Add(Net.GetGuidelineNode(Sweep->A == LeadEndId ? Sweep->B : Sweep->A)->Position.Y);
				}
				Leads.Add(Lead);
			}
		}
		return true;
	}

	bool AllOn(const FLeadIn& Lead, double LaneY)
	{
		for (const double Y : Lead.SweepEndYs) { if (!FMath::IsNearlyEqual(Y, LaneY, 1.0)) { return false; } }
		return Lead.SweepEndYs.Num() > 0;
	}
}

/**
 * A REAL SERVICE ROAD, CENTRED ON THE GHOST'S ServiceEdge LINE (user ruling 2026-09-27, "kerb on
 * the edge"). The line sits the road's half-width out past the far edge, so the road's near kerb
 * lies on the edge and its near lane a lane-offset beyond it - where every entry, a corner run
 * inside the edge, has the whole run it was inset for. Laid through the facade with the content
 * profile: two lanes, not the bare guideline every older entry test lays.
 *
 * Every entry joins the NEAR lane through a swept lead-in (not a hard join, which locks the lane -
 * a known issue, see RoadOnTheFarEdgeIsTheKnownCrossingCase) that never enters the far lane, and
 * no link logs the "cut that corner" warning.
 *
 * AND A SECOND LEAD-IN PER ENTRY, ACCEPTED: FAnchorLink::Build joins the sibling lane too (spec
 * 2026-09-23 §5, Airside.Build.TwoWay.AnchorBothLanes), and that one crosses the near lane - the
 * known gap Build's own comment records. What is held is that the NEAREST join is the near lane.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotRoadAlongTheServiceEdgeJoinsTheNearLaneTest,
	"Airside.Present.StandPlot.RoadAlongTheServiceEdgeJoinsTheNearLane",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotRoadAlongTheServiceEdgeJoinsTheNearLaneTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotPlacementTest;

	// B AND C: laid for the tow and the truck, whose entries sit different corner runs inside.
	// WAS A AND C, moved to B by the 2026-09-27 merge - A's own floor rectangle reads back as B
	// now (IcaoCode::StandLetterFor), so drawing "at A's floor" here would have quietly built
	// and labelled a B stand anyway; B says so honestly.
	for (const EIcaoCode Letter : { EIcaoCode::B, EIcaoCode::C })
	{
		const TCHAR* Code = IcaoCode::ToLetter(Letter);
		FAirsideTestWorld LetterWorld;
		if (!TestNotNull(TEXT("a world"), LetterWorld.World)) { return false; }
		if (!TestNotNull(TEXT("actor constructed"), LetterWorld.Actor)) { return false; }

		FWarningLineSpy Corner(FName(TEXT("LogAirside")));
		GLog->AddOutputDevice(&Corner);
		FStandAndRoad Laid;
		const bool bBuilt = BuildStandAndRoad(*this, LetterWorld.Actor, Letter,
			[](double FarEdgeY, double HalfWidth) { return FarEdgeY + HalfWidth; }, Laid);
		GLog->RemoveOutputDevice(&Corner);
		if (!bBuilt || !TestEqual(TEXT("the service road is two lanes"), Laid.LaneYs.Num(), 2)) { continue; }
		const double NearLaneY = Laid.LaneYs[0];
		const double Between = 0.5 * (Laid.LaneYs[0] + Laid.LaneYs[1]);
		TestTrue(*FString::Printf(TEXT("Code %s: the road's near kerb is on the far edge, not inside the stand"), Code),
			FMath::IsNearlyEqual(Between - Laid.HalfWidth, Laid.FarEdgeY, 1.0));

		for (const TPair<int32, TArray<FLeadIn>>& Entry : Laid.LeadsByEntry)
		{
			bool bNearAndSwept = false;
			for (const FLeadIn& Lead : Entry.Value)
			{
				TestFalse(*FString::Printf(TEXT("Code %s: entry %d has no hard join - a hard join locks its lane"), Code, Entry.Key),
					Lead.bHardJoin);
				bNearAndSwept |= !Lead.bHardJoin && AllOn(Lead, NearLaneY) && Lead.Deepest <= Between;
			}
			TestTrue(*FString::Printf(TEXT("Code %s: entry %d joins the NEAR lane (y %.0f) through a sweep that never enters the far lane (split at y %.0f)"),
				Code, Entry.Key, NearLaneY, Between), bNearAndSwept);
		}

		// THE CONTROL: the spy hears LogAirside warnings at all - the TugStand fixture's "has no
		// service bay" line is logged by every link pass - so its silence on corners is a finding.
		bool bHeardTugStand = false;
		for (const FString& Line : Corner.CapturedLines) { bHeardTugStand |= Line.Contains(TEXT("TugStand")); }
		TestTrue(*FString::Printf(TEXT("Code %s: the warning spy hears LogAirside (TugStand's own line)"), Code), bHeardTugStand);
		for (const FString& Line : Corner.CapturedLines)
		{
			TestFalse(*FString::Printf(TEXT("Code %s: no link cuts a corner: %s"), Code, *Line), Line.Contains(TEXT("cut that corner")));
		}
	}
	return true;
}

/**
 * THE KNOWN CROSSING CASE (user ruling 2026-09-27): a two-lane road centred ON the far edge
 * itself, which the ghost no longer invites. Its near lane sits a lane-offset INSIDE the far edge,
 * so the half-plane refuses it and every entry joins the FAR lane across it - a truck turning in
 * across the near lane. Kept to show what that road does, not to endorse it: the stand is still
 * serviceable, and every lead-in lands on the far lane.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotRoadOnTheFarEdgeIsTheKnownCrossingCaseTest,
	"Airside.Present.StandPlot.RoadOnTheFarEdgeIsTheKnownCrossingCase",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotRoadOnTheFarEdgeIsTheKnownCrossingCaseTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotPlacementTest;

	// WAS A AND C, moved to B by the 2026-09-27 merge - see
	// FStandPlotRoadAlongTheServiceEdgeJoinsTheNearLaneTest's own note.
	for (const EIcaoCode Letter : { EIcaoCode::B, EIcaoCode::C })
	{
		const TCHAR* Code = IcaoCode::ToLetter(Letter);
		FAirsideTestWorld LetterWorld;
		if (!TestNotNull(TEXT("a world"), LetterWorld.World)) { return false; }
		if (!TestNotNull(TEXT("actor constructed"), LetterWorld.Actor)) { return false; }

		FStandAndRoad Laid;
		if (!BuildStandAndRoad(*this, LetterWorld.Actor, Letter,
				[](double FarEdgeY, double) { return FarEdgeY; }, Laid)
			|| !TestEqual(TEXT("the service road is two lanes"), Laid.LaneYs.Num(), 2))
		{
			continue;
		}
		const double FarLaneY = Laid.LaneYs[1];
		TestTrue(*FString::Printf(TEXT("Code %s: the premise - the near lane (y %.0f) is inside the far edge (y %.0f)"),
			Code, Laid.LaneYs[0], Laid.FarEdgeY), Laid.LaneYs[0] < Laid.FarEdgeY);

		for (const TPair<int32, TArray<FLeadIn>>& Entry : Laid.LeadsByEntry)
		{
			TestTrue(*FString::Printf(TEXT("Code %s: entry %d is joined"), Code, Entry.Key), Entry.Value.Num() > 0);
			for (const FLeadIn& Lead : Entry.Value)
			{
				TestTrue(*FString::Printf(TEXT("Code %s: entry %d joins only the FAR lane (y %.0f)"), Code, Entry.Key, FarLaneY),
					AllOn(Lead, FarLaneY));
			}
		}

		FStandFacts Facts;
		if (TestTrue(TEXT("the stand describes"),
				InspectFacts::DescribeStand(nullptr, *LetterWorld.Actor->Network, Laid.Stand.Index, Facts)))
		{
			TestTrue(*FString::Printf(TEXT("Code %s: still serviceable"), Code), Facts.bServiceable);
		}
	}
	return true;
}

namespace StandPlotPlacementTest
{
	/**
	 * A purse holding exactly Funds - enough for one quote and not a unit more. A FAKE AND NOT
	 * ULedger for BuildPurseTest's reason: ULedger lives in AirportOps, which this module may
	 * not see, and what is measured here is the facade's afford gate, not a ledger's arithmetic.
	 */
	struct FExactFundsPurse : IBuildPurse
	{
		double Funds = 0.0;

		/** Every quote Credit was handed, summed at BaseAmount - the facade's refund QUOTE.
		 *  RefundFraction is ULedger's (per line, at its Pricing), out of this module's reach. */
		double Credited = 0.0;

		virtual bool CanAfford(const FBuildQuote& Quote) const override { return Quote.BaseAmount() <= Funds; }
		virtual int32 Charge(const FBuildQuote& Quote) override { Funds -= Quote.BaseAmount(); return 1; }
		virtual void Reverse(int32) override {}
		virtual void Credit(const FBuildQuote& Quote) override { Credited += Quote.BaseAmount(); }
		virtual FText Describe(const FBuildQuote& Quote) const override { return FText::AsNumber(Quote.BaseAmount()); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotAffordsWithTheChosenPavementTest,
	"Airside.Present.StandPlot.AffordsWithTheChosenPavement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotAffordsWithTheChosenPavementTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotPlacementTest;

	// DECLARED BEFORE THE WORLD, so it outlives the facade that holds a raw pointer to it.
	FExactFundsPurse Purse;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->ClearNetwork();
	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestNotNull(TEXT("the actor has an edit facade"), Facade)) { return false; }

	// THE PAVEMENT THE TOOL CHOSE is the pavement placed, priced and afforded (Review Focus 5):
	// with exactly enough money for a grass stand, the afford gate must pass for grass and
	// refuse tarmac, or the ghost would say "cannot afford" for a click that would have paid.
	const TArray<FVector2D> B = { {0,0}, {5000,0}, {5000,3950}, {0,3950} };
	const UEntityDefinition* Definition = Actor->ResolveStandDefinitionFor(EIcaoCode::B);
	if (!TestNotNull(TEXT("a Code B stand definition resolves"), Definition)) { return false; }
	const double GrassPrice = Facade->QuoteStand(*Definition, B, EPavement::Grass).BaseAmount();
	const double TarmacPrice = Facade->QuoteStand(*Definition, B, EPavement::Tarmac).BaseAmount();
	if (!TestTrue(TEXT("the premise: grass is cheaper than tarmac, or the gate below proves nothing"),
		GrassPrice < TarmacPrice)) { return false; }

	Purse.Funds = GrassPrice;
	Facade->SetPurse(&Purse);
	ON_SCOPE_EXIT { Facade->SetPurse(nullptr); };

	TestTrue(TEXT("grass is affordable"), Facade->WhyStandRefused(B, EPavement::Grass).IsEmpty());
	TestTrue(TEXT("tarmac is not"), Facade->WhyStandRefused(B, EPavement::Tarmac).Contains(TEXT("afford")));
	const int32 Placed = Facade->PlaceStandInPlot(B, B[0], B[1], EPavement::Grass);
	if (!TestTrue(TEXT("the grass stand is placed"), Placed != INDEX_NONE)) { return false; }
	TestEqual(TEXT("and the placed stand is grass"), Actor->Network->GetEntities()[Placed].Pavement, EPavement::Grass);
	TestEqual(TEXT("and it was charged the grass price, to the unit"), Purse.Funds, 0.0, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotDemolishRefundsThePadTest,
	"Airside.Present.StandPlot.DemolishRefundsThePad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotDemolishRefundsThePadTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotPlacementTest;

	// R11 (shared-pavement final review): DeleteEntity quoted the definition alone, so a drawn
	// stand refunded its equipment and kept the pad it was charged for. Through the ACTOR'S
	// facade, placed and demolished the way the tools do, so this goes red if DeleteEntity's
	// quote drops the pad line - not a hand-built quote asserted against itself.
	FExactFundsPurse Purse; // before the world - the facade holds a raw pointer to it

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->ClearNetwork();
	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestNotNull(TEXT("the actor has an edit facade"), Facade)) { return false; }

	const TArray<FVector2D> B = { {0,0}, {5000,0}, {5000,3950}, {0,3950} };
	const UEntityDefinition* Definition = Actor->ResolveStandDefinitionFor(EIcaoCode::B);
	if (!TestNotNull(TEXT("a Code B stand definition resolves"), Definition)) { return false; }
	const double EntityPrice = BuildCost::ForEntity(*Definition).BaseAmount();
	const double TarmacPad = Facade->QuoteStand(*Definition, B, EPavement::Tarmac).BaseAmount() - EntityPrice;
	if (!TestTrue(TEXT("the premise: a pad costs something, or a refund without it looks the same"),
		TarmacPad > 0.0)) { return false; }

	Purse.Funds = 1.0e12;
	Facade->SetPurse(&Purse);
	ON_SCOPE_EXIT { Facade->SetPurse(nullptr); };

	const int32 Placed = Facade->PlaceStandInPlot(B, B[0], B[1], EPavement::Grass);
	if (!TestTrue(TEXT("the grass stand is placed"), Placed != INDEX_NONE)) { return false; }
	if (!TestTrue(TEXT("and demolished"), Facade->DeleteEntity(Placed))) { return false; }

	// The QUOTE the ledger scraps, entity plus the pad at grass's 0.4 - ULedger::Credit then
	// applies RefundFraction to every line, the pad's (null source) included.
	TestEqual(TEXT("the demolition credits the equipment AND the grass pad (entity + 0.4 x tarmac pad)"),
		Purse.Credited, EntityPrice + 0.4 * TarmacPad, 1e-6);
	return true;
}

#endif
