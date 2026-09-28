#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Tool/BuildSession.h"
#include "Tool/GridFrameSource.h"
#include "Tool/SnapGuideSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * WHAT THE GRID FOLLOWS, source by source - grid-follows-snap design section 2. Hand-built
 * candidates and no world: the precedence is the thing under test, and the session wiring has
 * its own tests in GridSnapChainTest.
 */
namespace GridFrameSourceFixture
{
	const FVector2D RoadAt(1234.0, 777.0);

	FVector2D Deg(double Degrees)
	{
		const double R = FMath::DegreesToRadians(Degrees);
		return FVector2D(FMath::Cos(R), FMath::Sin(R));
	}

	/** A Parallel winner off a 30-degree taxiway at RoadAt, through a drag origin elsewhere. */
	SnapGuide::FCandidate Parallel(SnapGuide::ELabelKind Kind, double TurnDegrees = 0.0,
		SnapGuide::EReference Reference = SnapGuide::EReference::Taxiway)
	{
		SnapGuide::FCandidate C;
		C.Direction = Deg(30.0 + TurnDegrees);
		C.Through = FVector2D(-5000.0, 3000.0);
		C.ReferenceAt = RoadAt;
		C.Relation = SnapGuide::ERelation::Parallel;
		C.Reference = Reference;
		C.Label.Kind = Kind;
		return C;
	}

	SnapGuide::FResult WithWinner(const SnapGuide::FCandidate& C)
	{
		SnapGuide::FResult R;
		R.bActive = true;
		R.Winners.Add(C);
		R.Point = C.Through;
		return R;
	}

	FGridFrameInputs Inputs(double Step = 500.0)
	{
		FGridFrameInputs In;
		In.StepUu = Step;
		return In;
	}

	GridSnap::FGridFrame RoadFrame(double Step = 500.0)
	{
		return GridSnap::FGridFrame::Along(RoadAt, Deg(30.0), Step);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridFrameParallelWinnerTurnsTest,
	"Airside.Tool.GridFrame.ParallelWinnerTurns",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridFrameParallelWinnerTurnsTest::RunTest(const FString& Parameters)
{
	using namespace GridFrameSourceFixture;
	const SnapGuide::FResult Guide = WithWinner(Parallel(SnapGuide::ELabelKind::Along));
	FGridFrameInputs In = Inputs();
	In.Guide = &Guide;
	GridSnap::FGridFrame Held, Out;
	TestTrue(TEXT("the winner is the source"), GridFrameSource::Resolve(In, Held, Out) == EGridFrameSource::Winner);
	TestTrue(TEXT("the grid lies along the ROAD, through ReferenceAt, not through the drag origin"), Out.SameLines(RoadFrame()));
	TestTrue(TEXT("and is held"), Held.SameLines(Out));
	return true;
}

/** "Square to the taxiway" names the same road: the same grid, along it, not across it. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridFrameSquareToMatchesAlongTest,
	"Airside.Tool.GridFrame.SquareToMatchesAlong",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridFrameSquareToMatchesAlongTest::RunTest(const FString& Parameters)
{
	using namespace GridFrameSourceFixture;
	const SnapGuide::FResult Guide = WithWinner(Parallel(SnapGuide::ELabelKind::SquareTo, 90.0));
	FGridFrameInputs In = Inputs();
	In.Guide = &Guide;
	GridSnap::FGridFrame Held, Out;
	GridFrameSource::Resolve(In, Held, Out);
	TestTrue(TEXT("square-to gives the road's own grid"),
		FVector2D::Distance(Out.Origin, RoadFrame().Origin) < 1e-6 && FVector2D::Distance(Out.Axis, RoadFrame().Axis) < 1e-12);
	return true;
}

/** Turned to "45 degrees to the taxiway" the grid would be turned off the taxiway. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridFrameDiagonalIgnoredTest,
	"Airside.Tool.GridFrame.DiagonalAndGestureWinnersIgnored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridFrameDiagonalIgnoredTest::RunTest(const FString& Parameters)
{
	using namespace GridFrameSourceFixture;
	for (const SnapGuide::FCandidate& C : {
		Parallel(SnapGuide::ELabelKind::DegreesTo, 45.0),
		Parallel(SnapGuide::ELabelKind::Along, 0.0, SnapGuide::EReference::ThisGesture),
		Parallel(SnapGuide::ELabelKind::Along, 0.0, SnapGuide::EReference::World) })
	{
		const SnapGuide::FResult Guide = WithWinner(C);
		FGridFrameInputs In = Inputs();
		In.Guide = &Guide;
		GridSnap::FGridFrame Held, Out;
		TestTrue(TEXT("not a source: held"), GridFrameSource::Resolve(In, Held, Out) == EGridFrameSource::Held);
		TestTrue(TEXT("the (world) held frame stands"), Out.IsWorldAligned());
	}

	SnapGuide::FCandidate Angled = Parallel(SnapGuide::ELabelKind::AngledFromEnd, 45.0);
	Angled.Relation = SnapGuide::ERelation::AngledFrom;
	FVector2D T, D;
	TestFalse(TEXT("AngledFrom is not a line to follow"), GridFrameSource::LineOfWinner(Angled, T, D));
	return true;
}

/** The precedence below the winner: tool line, then anchor, then road snap. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridFramePrecedenceTest,
	"Airside.Tool.GridFrame.ToolLineAnchorRoadSnapInOrder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridFramePrecedenceTest::RunTest(const FString& Parameters)
{
	using namespace GridFrameSourceFixture;
	FGridFrameInputs In = Inputs();
	In.bToolLine = true;
	In.ToolThrough = RoadAt;
	In.ToolDirection = Deg(30.0);
	In.bAnchor = true;
	In.AnchorOrigin = FVector2D(0.0, 999.0);
	In.AnchorReference = Deg(10.0);
	In.bRoadSnap = true;
	In.RoadA = FVector2D(0.0, -999.0);
	In.RoadB = FVector2D(0.0, -999.0) + Deg(50.0) * 4000.0;

	GridSnap::FGridFrame Held, Out;
	TestTrue(TEXT("tool line beats anchor and road snap"), GridFrameSource::Resolve(In, Held, Out) == EGridFrameSource::ToolLine);
	TestTrue(TEXT("along the tool's line"), Out.SameLines(RoadFrame()));

	In.bToolLine = false;
	TestTrue(TEXT("then the anchor"), GridFrameSource::Resolve(In, Held, Out) == EGridFrameSource::Anchor);
	TestTrue(TEXT("at 10 degrees"), FMath::IsNearlyEqual(Out.AxisDegrees(), 10.0, 1e-9));

	In.bAnchor = false;
	TestTrue(TEXT("then the road snap"), GridFrameSource::Resolve(In, Held, Out) == EGridFrameSource::RoadSnap);
	TestTrue(TEXT("at 50 degrees"), FMath::IsNearlyEqual(Out.AxisDegrees(), 50.0, 1e-9));

	In.AnchorReference = FVector2D::ZeroVector;
	In.bAnchor = true;
	TestTrue(TEXT("an anchor with no reference is no anchor"), GridFrameSource::Resolve(In, Held, Out) == EGridFrameSource::RoadSnap);
	return true;
}

/** Nothing to follow holds the last frame - with the new step if the step changed. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridFrameHoldsTest,
	"Airside.Tool.GridFrame.NothingHoldsTheLast",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridFrameHoldsTest::RunTest(const FString& Parameters)
{
	using namespace GridFrameSourceFixture;
	FGridFrameInputs In = Inputs();
	In.bToolLine = true;
	In.ToolThrough = RoadAt;
	In.ToolDirection = Deg(30.0);
	GridSnap::FGridFrame Held, Out;
	GridFrameSource::Resolve(In, Held, Out);

	FGridFrameInputs Nothing = Inputs(1000.0);
	TestTrue(TEXT("nothing: held"), GridFrameSource::Resolve(Nothing, Held, Out) == EGridFrameSource::Held);
	TestTrue(TEXT("still along the road"), FVector2D::Distance(Out.Origin, RoadFrame().Origin) < 1e-9 && Out.Axis == RoadFrame().Axis);
	TestEqual(TEXT("at the new step"), Out.StepUu, 1000.0);
	return true;
}

/** World ignores everything, and leaves the held frame for Follow to resume. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridFrameWorldIgnoresTest,
	"Airside.Tool.GridFrame.WorldIgnoresEverything",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridFrameWorldIgnoresTest::RunTest(const FString& Parameters)
{
	using namespace GridFrameSourceFixture;
	const SnapGuide::FResult Guide = WithWinner(Parallel(SnapGuide::ELabelKind::Along));
	FGridFrameInputs In = Inputs();
	In.Guide = &Guide;
	GridSnap::FGridFrame Held, Out;
	GridFrameSource::Resolve(In, Held, Out);

	In.Orientation = EGridOrientation::World;
	TestTrue(TEXT("world"), GridFrameSource::Resolve(In, Held, Out) == EGridFrameSource::World);
	TestTrue(TEXT("axis-aligned"), Out.IsWorldAligned() && Out.StepUu == 500.0);
	TestTrue(TEXT("the held frame is untouched"), Held.SameLines(RoadFrame()));

	FGridFrameInputs Off = Inputs(0.0);
	TestTrue(TEXT("no step: no grid"), GridFrameSource::Resolve(Off, Held, Out) == EGridFrameSource::World);
	TestFalse(TEXT("and it is off"), Out.IsOn());
	return true;
}

/** The setting: Follow by default, one toggle, and a change the frame cache sees. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridFrameOrientationSettingTest,
	"Airside.Tool.GridFrame.OrientationSetting",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridFrameOrientationSettingTest::RunTest(const FString& Parameters)
{
	FSnapGuideSettings Settings;
	TestEqual(TEXT("Follow by default"), Settings.GridOrientation, EGridOrientation::Follow);
	Settings.ToggleGridOrientation();
	TestEqual(TEXT("toggle: World"), Settings.GridOrientation, EGridOrientation::World);
	Settings.ToggleGridOrientation();
	TestEqual(TEXT("toggle: Follow"), Settings.GridOrientation, EGridOrientation::Follow);

	FBuildSessionTunables A;
	FBuildSessionTunables B;
	B.GuideSources.GridOrientation = EGridOrientation::World;
	TestFalse(TEXT("differing only in GridOrientation is unequal - the frame cache replays otherwise"), A == B);
	return true;
}

#endif
