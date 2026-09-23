#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/IcaoCode.h"
#include "Solve/RoadGeom.h"
#include "Solve/StandBox.h"
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
	const int32 Index = Target->PlaceStandInPlot(Rect, A, B);
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

	// THE STOP MARK ITSELF: StandBox::PoseFor's own derivation - the template's tail is laid
	// on the entrance edge, so the stop mark sits Depth - NoseFwd in from the entrance
	// midpoint, along Inward (the nose end, not the tail, is what reaches inward).
	const FVector2D EntranceMid = (A + B) * 0.5;
	const double ExpectedOffset =
		IcaoCode::StandDepthForLetter(EIcaoCode::C) - IcaoCode::MaxNoseFwdForLetter(EIcaoCode::C);
	const double ActualOffset = FVector2D::DotProduct(Entity.Position - EntranceMid, Inward);
	TestTrue(TEXT("the stop mark sits Depth - NoseFwd in from the entrance, along Inward"),
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
	const int32 Index = Target->PlaceStandInPlot(Rect, Rect[0], Rect[1]);
	TestEqual(TEXT("a 20x20m rect is refused"), Index, INDEX_NONE);

	const FString Reason = Target->WhyStandRefused(Rect);
	TestTrue(TEXT("the reason names the missing width"), Reason.Contains(TEXT("more width")));

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
	const int32 FirstIndex = Target->PlaceStandInPlot(First, A, B);
	if (!TestTrue(TEXT("the first stand is placed"), FirstIndex != INDEX_NONE)) { return false; }

	// Shifted half a width over - well short of clearing the first rectangle, so the two
	// overlap rather than merely touch.
	const double Width = IcaoCode::StandWidthForLetter(EIcaoCode::C);
	FVector2D SecondA, SecondB;
	TArray<FVector2D> Second = FloorRect(EIcaoCode::C, 1000.0, SecondA, SecondB);
	for (FVector2D& P : Second) { P.X += Width * 0.5; }
	SecondA.X += Width * 0.5;
	SecondB.X += Width * 0.5;

	const int32 SecondIndex = Target->PlaceStandInPlot(Second, SecondA, SecondB);
	TestEqual(TEXT("the overlapping second stand is refused"), SecondIndex, INDEX_NONE);

	const FString Reason = Target->WhyStandRefused(Second);
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

	const int32 Index = Target->PlaceStandInPlot(Rect, A, B);
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

	const int32 Index = Target->PlaceStandInPlot(Rect, A, B);
	TestEqual(TEXT("a taxiway through the interior refuses the stand"), Index, INDEX_NONE);

	const FString Reason = Target->WhyStandRefused(Rect);
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
	const int32 Index = Target->PlaceStandInPlot(Rect, A, B);
	if (!TestTrue(TEXT("the stand is placed"), Index != INDEX_NONE)) { return false; }
	if (!TestEqual(TEXT("one live stand before undo"), LiveStandCount(Actor), 1)) { return false; }

	if (!TestTrue(TEXT("there is something to undo"), Actor->CanUndo())) { return false; }
	if (!TestTrue(TEXT("undo succeeds"), Actor->Undo())) { return false; }

	TestEqual(TEXT("no live stand after undo"), LiveStandCount(Actor), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotUnfitLetterRefusedTest,
	"Airside.Present.StandPlot.UnfitLetterRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotUnfitLetterRefusedTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotPlacementTest;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();

	// Task 1's own measured table (StandLayoutTest.cpp,
	// Airside.Entities.StandLayoutEveryLetterReport): A and B DO NOT FIT their own floor,
	// C through F FIT. This test names exactly the two that do not, so it fails loudly - not
	// silently - the day a template changes what fits.
	const EIcaoCode DoesNotFit[] = { EIcaoCode::A, EIcaoCode::B };
	IRoadEditTarget* Target = Actor;

	for (const EIcaoCode Letter : DoesNotFit)
	{
		FVector2D A, B;
		const TArray<FVector2D> Rect = FloorRect(Letter, 1000.0 * (static_cast<double>(Letter) + 1.0), A, B);

		const int32 Index = Target->PlaceStandInPlot(Rect, A, B);
		TestEqual(*FString::Printf(TEXT("Code %s's own floor is refused"), IcaoCode::ToLetter(Letter)),
			Index, INDEX_NONE);

		const FString Reason = Target->WhyStandRefused(Rect);
		TestTrue(*FString::Printf(TEXT("Code %s's reason says it cannot be built yet"),
				IcaoCode::ToLetter(Letter)),
			Reason.Contains(TEXT("cannot be built yet")));
	}

	return true;
}

// FIX ROUND 1: PlacesCodeC alone only proves the one letter with an authored asset. D, E and
// F all go through ResolveStandDefinitionFor's lazily-built cache instead (Task 1's table:
// they FIT their own floor, unlike A and B) - this loop is the same round trip for each of
// them, one stand per letter so none can overlap another.
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

	const EIcaoCode Fits[] = { EIcaoCode::D, EIcaoCode::E, EIcaoCode::F };
	IRoadEditTarget* Target = Actor;

	for (const EIcaoCode Letter : Fits)
	{
		FVector2D A, B;
		// Spread far enough apart (20 km per letter) that no two of the widest floors (F's is
		// under 190 m) can ever overlap, whatever the table's figures do later.
		const TArray<FVector2D> Rect =
			FloorRect(Letter, 20000.0 * (static_cast<double>(Letter) + 1.0), A, B);

		const int32 Index = Target->PlaceStandInPlot(Rect, A, B);
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

#endif
