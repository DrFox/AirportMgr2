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
	const double Width = IcaoCode::StandWidthForLetter(EIcaoCode::C);

	// A SERVICE road (GSE road), crossing the rectangle's back 4 m strip - the same strip
	// UEntityDefinition::BuildStandTemplate lays the template's own GSE road across.
	// Service roads are exempt from the taxiway-crossing refusal - see
	// URoadEditFacade::WhyStandRefused.
	IRoadEditTarget* Target = Actor;
	const int32 West = Target->PlaceNode(FVector2D(Width * 0.5, -500.0));
	const int32 East = Target->PlaceNode(FVector2D(Width * 0.5, 500.0));
	Target->ConnectNodes(West, East, ERoadKind::ServiceRoad, INDEX_NONE);

	const int32 Index = Target->PlaceStandInPlot(Rect, A, B);
	TestTrue(TEXT("the stand is placed despite the service road crossing its back strip"),
		Index != INDEX_NONE);

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

#endif
