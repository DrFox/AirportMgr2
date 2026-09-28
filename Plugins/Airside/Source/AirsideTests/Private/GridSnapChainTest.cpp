#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/BuildSession.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/SnapGuideSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE GRID THROUGH THE SEAM BOTH DRIVERS SHARE. Every test here goes through
 * FBuildSession::MakeContext, never FSnapGuideChain::Resolve alone - the grid is only a feature
 * if a tool's GuidedCursor() lands on it, and the session is what puts it there.
 */
namespace GridSnapChainFixture
{
	struct FGridSession
	{
		FAirsideTestWorld TestWorld;
		FBuildSession Session;
		FBuildSessionTunables Tunables;

		FToolContext At(const FVector2D& Where, bool bSuspend = false) const
		{
			return Session.MakeContext(TestWorld.Actor, Where, Tunables, false, false, bSuspend);
		}
	};

	int32 ToolIndexFor(const TCHAR* Id)
	{
		const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
		for (int32 Index = 0; Index < Registry.Num(); ++Index)
		{
			if (Registry[Index].Id == FName(Id)) { return Index; }
		}
		return INDEX_NONE;
	}

	/**
	 * An east-west taxiway ending at x = -4000, and the Taxiway tool idle (a free start, so the
	 * chain runs before the first click). Collinear forced ON: it is the one guide the 1-winner
	 * test needs, and a default that moved would silently stop that test measuring anything.
	 */
	bool Begin(FGridSession& Out, const TCHAR* ToolId, EGridStep Step)
	{
		if (Out.TestWorld.World == nullptr || Out.TestWorld.Actor == nullptr) { return false; }
		IRoadEditTarget* Target = Out.TestWorld.Actor;
		const int32 West = Target->PlaceNode(FVector2D(-20000.0, 0.0));
		const int32 East = Target->PlaceNode(FVector2D(-4000.0, 0.0));
		Target->ConnectNodes(West, East, ERoadKind::Taxiway, INDEX_NONE);

		const int32 Index = ToolIndexFor(ToolId);
		if (Index == INDEX_NONE) { return false; }

		Out.Tunables = Out.TestWorld.Actor->MakeTunables(10000.0);
		Out.Tunables.GuideSources.bCollinear = true;
		Out.Tunables.GuideSources.bTaxiway = true;
		Out.Tunables.GuideSources.GridStep = Step;
		Out.Session.SelectTool(Index);
		return Out.Session.GetActiveTool() != nullptr;
	}

	/** Far from every line the road could propose: 3270 uu off its extension, 8130 east of its end. */
	const FVector2D InTheOpen(4130.0, 3270.0);

	/** Within Collinear's 300 uu of the road's extension, and NOT on a 5 m grid line in X. */
	const FVector2D NearTheExtension(4130.0, 120.0);
}

/** No guide in reach: the point is the nearest grid point, and the context says so. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridSnapNoGuideRoundsTest,
	"Airside.Tool.GridSnap.NoGuideRoundsToGrid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridSnapNoGuideRoundsTest::RunTest(const FString& Parameters)
{
	GridSnapChainFixture::FGridSession S;
	if (!TestTrue(TEXT("an idle taxiway tool, 5 m grid"), GridSnapChainFixture::Begin(S, TEXT("Taxiway"), EGridStep::FiveMetres))) { return false; }

	const FToolContext Context = S.At(GridSnapChainFixture::InTheOpen);
	if (!TestEqual(TEXT("precondition: no guide holds out here"), Context.Guide.Winners.Num(), 0)) { return false; }

	TestEqual(TEXT("the point is the nearest 5 m grid point"), Context.GuidedCursor(), FVector2D(4000.0, 3500.0));
	TestEqual(TEXT("the context carries the step, for the overlay and the plot tools"), Context.GridFrame.StepUu, 500.0);
	TestEqual(TEXT("and the 5 m overlay radius"), Context.GridOverlayRadiusUu, 6000.0);
	return true;
}

/**
 * ONE GUIDE: the point stays ON the guide and slides to a grid crossing along it - "on the
 * road's line, at a round 5 m" - and with the grid off the guide alone decides, exactly as before.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridSnapOneGuideSlidesTest,
	"Airside.Tool.GridSnap.OneGuideSlidesToCrossing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridSnapOneGuideSlidesTest::RunTest(const FString& Parameters)
{
	GridSnapChainFixture::FGridSession Off;
	if (!TestTrue(TEXT("grid off"), GridSnapChainFixture::Begin(Off, TEXT("Taxiway"), EGridStep::Off))) { return false; }
	const FToolContext Plain = Off.At(GridSnapChainFixture::NearTheExtension);
	if (!TestEqual(TEXT("precondition: the extension guide holds"), Plain.Guide.Winners.Num(), 1)) { return false; }
	TestTrue(TEXT("grid off: on the extension, where the cursor is along it"),
		FMath::IsNearlyEqual(Plain.GuidedCursor().Y, 0.0, 1e-6) && FMath::IsNearlyEqual(Plain.GuidedCursor().X, 4130.0, 1e-6));
	TestEqual(TEXT("grid off: no step on the context"), Plain.GridFrame.StepUu, 0.0);

	GridSnapChainFixture::FGridSession On;
	if (!TestTrue(TEXT("grid 5 m"), GridSnapChainFixture::Begin(On, TEXT("Taxiway"), EGridStep::FiveMetres))) { return false; }
	const FToolContext Gridded = On.At(GridSnapChainFixture::NearTheExtension);
	TestEqual(TEXT("the guide still wins"), Gridded.Guide.Winners.Num(), 1);
	TestTrue(TEXT("still ON the extension - the guide beats the grid"),
		FMath::IsNearlyEqual(Gridded.GuidedCursor().Y, 0.0, 1e-6));
	TestTrue(TEXT("and at the 5 m crossing along it"),
		FMath::IsNearlyEqual(Gridded.GuidedCursor().X, 4000.0, 1e-6));
	return true;
}

/**
 * ALT FREES THE POINT FROM THE GRID TOO, and a tool that consults neither the chain nor the
 * grid - Select - gets no step, which is what keeps the overlay off it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridSnapSuspendAndSelectTest,
	"Airside.Tool.GridSnap.SuspendAndSelectHaveNoGrid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridSnapSuspendAndSelectTest::RunTest(const FString& Parameters)
{
	GridSnapChainFixture::FGridSession S;
	if (!TestTrue(TEXT("taxiway, 5 m"), GridSnapChainFixture::Begin(S, TEXT("Taxiway"), EGridStep::FiveMetres))) { return false; }
	const FToolContext Held = S.At(GridSnapChainFixture::InTheOpen, /*bSuspend=*/true);
	TestEqual(TEXT("Alt held: the point is the raw cursor"), Held.GuidedCursor(), GridSnapChainFixture::InTheOpen);
	TestEqual(TEXT("Alt held: no step, so no overlay"), Held.GridFrame.StepUu, 0.0);

	GridSnapChainFixture::FGridSession Select;
	if (!TestTrue(TEXT("select, 5 m"), GridSnapChainFixture::Begin(Select, TEXT("Select"), EGridStep::FiveMetres))) { return false; }
	const FToolContext Picking = Select.At(GridSnapChainFixture::InTheOpen);
	TestEqual(TEXT("Select: no step"), Picking.GridFrame.StepUu, 0.0);
	TestEqual(TEXT("Select: the cursor is not moved"), Picking.GuidedCursor(), GridSnapChainFixture::InTheOpen);
	return true;
}

/**
 * A STEP CHANGE IS A DIFFERENT FRAME. GetFrameContext replays the cached context while the
 * tunables compare equal; a GridStep missing from operator== would keep the old snap after the
 * player pressed the button.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridSnapTunablesCompareStepTest,
	"Airside.Tool.GridSnap.TunablesCompareStep",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridSnapTunablesCompareStepTest::RunTest(const FString& Parameters)
{
	FBuildSessionTunables A;
	FBuildSessionTunables B;
	TestTrue(TEXT("equal to begin with"), A == B);
	B.GuideSources.GridStep = EGridStep::OneMetre;
	TestFalse(TEXT("differing only in GridStep is unequal"), A == B);

	// Through the cache itself, which is the consumer of that equality.
	GridSnapChainFixture::FGridSession S;
	if (!TestTrue(TEXT("taxiway, off"), GridSnapChainFixture::Begin(S, TEXT("Taxiway"), EGridStep::Off))) { return false; }
	const FVector2D First = S.Session.GetFrameContext(S.TestWorld.Actor, GridSnapChainFixture::InTheOpen, S.Tunables, false, false, false).GuidedCursor();
	S.Tunables.GuideSources.GridStep = EGridStep::FiveMetres;
	const FVector2D Second = S.Session.GetFrameContext(S.TestWorld.Actor, GridSnapChainFixture::InTheOpen, S.Tunables, false, false, false).GuidedCursor();
	TestEqual(TEXT("off: the raw cursor"), First, GridSnapChainFixture::InTheOpen);
	TestEqual(TEXT("same cursor, new step: the cache did not replay the old frame"), Second, FVector2D(4000.0, 3500.0));
	return true;
}

/** The cycle the bar button walks, and its figures - the one place they live. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridSnapCycleTest,
	"Airside.Tool.GridSnap.CycleAndFigures",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridSnapCycleTest::RunTest(const FString& Parameters)
{
	FSnapGuideSettings Settings;
	TestEqual(TEXT("a new airport has no grid"), Settings.GridStep, EGridStep::Off);
	TestEqual(TEXT("off is 0 uu"), Settings.GridStepUu(), 0.0);

	const double Steps[] = { 100.0, 500.0, 1000.0, 0.0 };
	const double Radii[] = { 2000.0, 6000.0, 12000.0, 0.0 };
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Settings.CycleGridStep();
		TestEqual(*FString::Printf(TEXT("press %d: step"), Index + 1), Settings.GridStepUu(), Steps[Index]);
		TestEqual(*FString::Printf(TEXT("press %d: radius"), Index + 1), Settings.GridOverlayRadiusUu(), Radii[Index]);
	}
	TestEqual(TEXT("four presses come back to off"), Settings.GridStep, EGridStep::Off);
	return true;
}

/**
 * FOLLOW, THROUGH THE SESSION - grid-follows-snap design. The fixture's road turned to 30
 * degrees: a cursor near its extension wins Collinear, and the grid turns to the road.
 */
namespace GridFollowFixture
{
	FVector2D Dir() { return FVector2D(FMath::Cos(PI / 6.0), FMath::Sin(PI / 6.0)); }
	FVector2D Perp() { return FVector2D(-Dir().Y, Dir().X); }

	/** The fixture's geometry, turned: road from -20000 to -4000 along Dir. */
	FVector2D Turned(const FVector2D& Along) { return Dir() * Along.X + Perp() * Along.Y; }

	bool Begin(GridSnapChainFixture::FGridSession& Out, EGridOrientation Orientation, EGridStep Step = EGridStep::FiveMetres)
	{
		if (Out.TestWorld.World == nullptr || Out.TestWorld.Actor == nullptr) { return false; }
		IRoadEditTarget* Target = Out.TestWorld.Actor;
		const int32 West = Target->PlaceNode(Turned(FVector2D(-20000.0, 0.0)));
		const int32 East = Target->PlaceNode(Turned(FVector2D(-4000.0, 0.0)));
		Target->ConnectNodes(West, East, ERoadKind::Taxiway, INDEX_NONE);

		const int32 Index = GridSnapChainFixture::ToolIndexFor(TEXT("Taxiway"));
		if (Index == INDEX_NONE) { return false; }
		Out.Tunables = Out.TestWorld.Actor->MakeTunables(10000.0);
		Out.Tunables.GuideSources.bCollinear = true;
		Out.Tunables.GuideSources.bTaxiway = true;
		Out.Tunables.GuideSources.GridStep = Step;
		Out.Tunables.GuideSources.GridOrientation = Orientation;
		Out.Session.SelectTool(Index);
		return Out.Session.GetActiveTool() != nullptr;
	}

	const FVector2D NearTheExtension() { return Turned(GridSnapChainFixture::NearTheExtension); }
	const FVector2D InTheOpen() { return Turned(GridSnapChainFixture::InTheOpen); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridFollowTurnsToTheRoadTest,
	"Airside.Tool.GridSnap.FollowTurnsToTheRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridFollowTurnsToTheRoadTest::RunTest(const FString& Parameters)
{
	using namespace GridFollowFixture;
	GridSnapChainFixture::FGridSession Follow;
	if (!TestTrue(TEXT("30-degree taxiway, Follow"), Begin(Follow, EGridOrientation::Follow))) { return false; }
	const FToolContext Turned = Follow.At(NearTheExtension());
	if (!TestEqual(TEXT("precondition: the extension guide holds"), Turned.Guide.Winners.Num(), 1)) { return false; }
	TestTrue(TEXT("the grid turned to the road"), FMath::IsNearlyEqual(Turned.GridFrame.AxisDegrees(), 30.0, 1e-6));
	const FVector2D Point = Turned.GuidedCursor();
	TestTrue(TEXT("the point stays on the extension"), FMath::Abs(FVector2D::DotProduct(Point, Perp())) < 1e-6);
	TestTrue(TEXT("at a crossing along it, one step apart from the world origin's cross line"),
		FMath::IsNearlyEqual(FVector2D::DotProduct(Point, Dir()), 4000.0, 1e-6));

	GridSnapChainFixture::FGridSession World;
	if (!TestTrue(TEXT("30-degree taxiway, World"), Begin(World, EGridOrientation::World))) { return false; }
	const FToolContext Square = World.At(NearTheExtension());
	TestTrue(TEXT("World: axis-aligned regardless"), Square.GridFrame.IsWorldAligned() && Square.GridFrame.IsOn());
	return true;
}

/** Held: through Alt, and out into the open, and across a change of step. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridFollowHoldsTest,
	"Airside.Tool.GridSnap.FollowHoldsThroughAltAndStep",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridFollowHoldsTest::RunTest(const FString& Parameters)
{
	using namespace GridFollowFixture;
	GridSnapChainFixture::FGridSession S;
	if (!TestTrue(TEXT("30-degree taxiway, Follow"), Begin(S, EGridOrientation::Follow))) { return false; }
	S.At(NearTheExtension());

	const FToolContext Held = S.At(InTheOpen(), /*bSuspend=*/true);
	TestFalse(TEXT("Alt: no grid"), Held.GridFrame.IsOn());

	const FToolContext Open = S.At(InTheOpen());
	if (!TestEqual(TEXT("precondition: nothing to follow out here"), Open.Guide.Winners.Num(), 0)) { return false; }
	TestTrue(TEXT("after Alt, in the open: still turned to the road"), FMath::IsNearlyEqual(Open.GridFrame.AxisDegrees(), 30.0, 1e-6));

	S.Tunables.GuideSources.GridStep = EGridStep::TenMetres;
	const FToolContext Wider = S.At(InTheOpen());
	TestTrue(TEXT("a new step keeps the direction"), FMath::IsNearlyEqual(Wider.GridFrame.AxisDegrees(), 30.0, 1e-6));
	TestEqual(TEXT("and takes the new pitch"), Wider.GridFrame.StepUu, 1000.0);
	return true;
}

#endif
