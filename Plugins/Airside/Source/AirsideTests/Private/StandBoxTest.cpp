#include "CoreMinimal.h"
#include "Algo/Reverse.h"
#include "Misc/AutomationTest.h"
#include "Solve/IcaoCode.h"
#include "Solve/RoadGeom.h"
#include "Solve/StandBox.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandBoxRoundTripTest,
	"Airside.Solve.StandBox.RoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandBoxRoundTripTest::RunTest(const FString& Parameters)
{
	// THE BOX A LETTER BUILDS MUST READ BACK AS THAT LETTER - PoseFor and BoxAt are meant to
	// be exact inverses of each other, so every letter's own floor-sized entrance must round
	// trip to the exact corners the letter's width/depth imply, and LetterOf on those corners
	// must name the same letter again.
	//
	// CODE A DROPPED (2026-09-27 merge): its width/depth read B's own figures now
	// (IcaoCode::StandLetterFor), so a box built "at A's floor" reads back as B, not A - this
	// test is about a letter reading back as ITSELF, which A can no longer do.
	for (const EIcaoCode Letter : { EIcaoCode::B, EIcaoCode::C, EIcaoCode::D, EIcaoCode::E, EIcaoCode::F })
	{
		const double W = IcaoCode::StandWidthForLetter(Letter);
		const double D = IcaoCode::StandDepthForLetter(Letter);
		const FVector2D A(0.0, 0.0);
		const FVector2D B(W, 0.0);
		const FVector2D Inward(0.0, 1.0);

		const FLetterEnvelope Envelope = IcaoCode::FloorEnvelopeForLetter(Letter);
		const StandBox::FStandPose Pose = StandBox::PoseFor(A, B, Inward, {}, Letter, Envelope);
		TArray<FVector2D> Corners;
		StandBox::BoxAt(Pose, Letter, Envelope, Corners);

		const TCHAR* LetterName = IcaoCode::ToLetter(Letter);
		TestEqual(*FString::Printf(TEXT("%s box has four corners"), LetterName), Corners.Num(), 4);
		if (Corners.Num() == 4)
		{
			// Floating arithmetic through GetSafeNormal, not a weld - 0.01 uu is legitimate here.
			TestTrue(*FString::Printf(TEXT("%s corner 0 is the entrance start"), LetterName),
				Corners[0].Equals(FVector2D(0.0, 0.0), 0.01));
			TestTrue(*FString::Printf(TEXT("%s corner 1 is the entrance end"), LetterName),
				Corners[1].Equals(FVector2D(W, 0.0), 0.01));
			TestTrue(*FString::Printf(TEXT("%s corner 2 is inward of the entrance end"), LetterName),
				Corners[2].Equals(FVector2D(W, D), 0.01));
			TestTrue(*FString::Printf(TEXT("%s corner 3 is inward of the entrance start"), LetterName),
				Corners[3].Equals(FVector2D(0.0, D), 0.01));
		}

		TestTrue(*FString::Printf(TEXT("%s box winds positive - the pad triangulator's winding"), LetterName),
			RoadGeom::PolygonArea(Corners) > 0.0);

		const TOptional<EIcaoCode> ReadBack = StandBox::LetterOf(Corners);
		TestTrue(*FString::Printf(TEXT("%s box reads back a letter at all"), LetterName), ReadBack.IsSet());
		if (ReadBack.IsSet())
		{
			TestEqual(*FString::Printf(TEXT("%s box reads back as %s"), LetterName, LetterName),
				ReadBack.GetValue(), Letter);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandBoxMeasuredFromTheFarEdgeTest,
	"Airside.Solve.StandBox.MeasuredFromTheFarEdge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandBoxMeasuredFromTheFarEdgeTest::RunTest(const FString& Parameters)
{
	// THE AIRCRAFT USES THE WHOLE DEPTH (user, 2026-09-27): a stand drawn deeper than its
	// letter's floor parks its aircraft FarSetback in from the FAR edge, so the extra depth
	// lies behind the tail, toward the taxiway, and the service ground and far-edge contacts
	// stay at the drawn far edge. At the floor depth the pose is exactly where it always was.
	const EIcaoCode Letter = EIcaoCode::C;
	const FLetterEnvelope Envelope = IcaoCode::FloorEnvelopeForLetter(Letter);
	const double W = IcaoCode::StandWidthForLetter(Letter);
	const double Floor = IcaoCode::StandDepthForLetter(Letter);
	const FVector2D Inward(0.0, 1.0);

	for (const double Extra : { 0.0, 1500.0 })
	{
		const double D = Floor + Extra;
		// Wound CLOCKWISE on purpose: which corner comes first must not matter.
		const TArray<FVector2D> Outline = { {0.0, 0.0}, {0.0, D}, {W, D}, {W, 0.0} };
		const StandBox::FStandPose Pose = StandBox::PoseFor({0.0, 0.0}, {W, 0.0}, Inward, Outline, Letter, Envelope);
		TestEqual(FString::Printf(TEXT("%.0f uu deep: stop mark is FarSetback in from the far edge"), D),
			D - Pose.Position.Y, StandBox::FarSetback(Letter, Envelope), 0.01);
		TestEqual(FString::Printf(TEXT("%.0f uu deep: centred across the entrance"), D), Pose.Position.X, 0.5 * W, 0.01);
	}

	// FLOOR DEPTH IS UNCHANGED: the two measurements coincide there, so a floor-sized stand
	// placed before this change stands exactly where it did.
	const TArray<FVector2D> FloorBox = { {0.0, 0.0}, {W, 0.0}, {W, Floor}, {0.0, Floor} };
	const StandBox::FStandPose AtFloor = StandBox::PoseFor({0.0, 0.0}, {W, 0.0}, Inward, FloorBox, Letter, Envelope);
	TestEqual(TEXT("at the floor the stop mark is still EntranceSetback from the entrance"),
		AtFloor.Position.Y, StandBox::EntranceSetback(Letter, Envelope), 0.01);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandBoxTailToEntranceTest,
	"Airside.Solve.StandBox.TailToEntrance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandBoxTailToEntranceTest::RunTest(const FString& Parameters)
{
	// THE NOSE POINTS AWAY FROM THE TAXIWAY: the pose's Facing is exactly Inward, and the
	// stop mark sits EntranceSetback (MaxTailAft + wingtip clearance) along it from the
	// entrance edge's midpoint - since this task (far-side entry), the tail's own clearance
	// off the entrance, not the old Depth - NoseFwd that put most of the depth's slack
	// behind the tail.
	const EIcaoCode Letter = EIcaoCode::C;
	const FVector2D A(0.0, 0.0);
	const FVector2D B(IcaoCode::StandWidthForLetter(Letter), 0.0);
	const FVector2D Inward(0.0, 1.0);

	const FLetterEnvelope Envelope = IcaoCode::FloorEnvelopeForLetter(Letter);
	const StandBox::FStandPose Pose = StandBox::PoseFor(A, B, Inward, {}, Letter, Envelope);
	TestTrue(TEXT("Facing equals Inward"), Pose.Facing.Equals(Inward, 1e-9));

	const double Expected = StandBox::EntranceSetback(Letter, Envelope);
	const double Actual = FVector2D::DotProduct(Pose.Position - A, Inward);
	TestTrue(TEXT("stop mark is EntranceSetback in from the entrance, along Inward"),
		FMath::IsNearlyEqual(Actual, Expected, 0.01));

	return true;
}

/**
 * THE POSE AND FAR EDGE AGREE, PER LETTER (this task, far-side entry): the tail is pulled in
 * to EntranceSetback = MaxTailAft + wingtip clearance from the entrance, never flush with the
 * far edge, and the slack the far-side entry needs is left ahead of the nose instead - the
 * far edge (Depth) still carries the service ground, now beyond the nose rather than the tail.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandBoxTailAtEntranceTest,
	"Airside.Solve.StandBox.TailAtEntrance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandBoxTailAtEntranceTest::RunTest(const FString& Parameters)
{
	for (const EIcaoCode Letter : { EIcaoCode::A, EIcaoCode::B, EIcaoCode::C, EIcaoCode::D, EIcaoCode::E, EIcaoCode::F })
	{
		const TCHAR* LetterName = IcaoCode::ToLetter(Letter);
		const FVector2D A(0.0, 0.0);
		const FVector2D B(IcaoCode::StandWidthForLetter(Letter), 0.0);
		const FVector2D Inward(0.0, 1.0);
		const FLetterEnvelope Envelope = IcaoCode::FloorEnvelopeForLetter(Letter);
		const double Depth = IcaoCode::StandDepthForLetter(Letter);

		const StandBox::FStandPose Pose = StandBox::PoseFor(A, B, Inward, {}, Letter, Envelope);

		const double ExpectedSetback = Envelope.MaxTailAft + IcaoCode::WingtipClearanceForLetter(Letter);
		TestEqual(*FString::Printf(TEXT("%s stop mark sits MaxTailAft + clearance in from the entrance"), LetterName),
			Pose.Position.Y, ExpectedSetback);

		TArray<FVector2D> Corners;
		StandBox::BoxAt(Pose, Letter, Envelope, Corners);
		if (TestEqual(*FString::Printf(TEXT("%s box has four corners"), LetterName), Corners.Num(), 4))
		{
			TestEqual(*FString::Printf(TEXT("%s box's far edge sits at Depth"), LetterName), Corners[2].Y, Depth);
			TestEqual(*FString::Printf(TEXT("%s box's far edge sits at Depth (other corner)"), LetterName), Corners[3].Y, Depth);
		}

		// SLACK AHEAD OF THE NOSE, THE WHOLE POINT OF THIS TASK: the nose sits short of the far
		// edge, where the mirrored template lays the far-side entry's service ground.
		const double NoseY = Pose.Position.Y + Envelope.MaxNoseFwd;
		TestTrue(*FString::Printf(TEXT("%s nose sits short of the far edge - slack lies ahead of it (nose %.1f, depth %.1f)"),
			LetterName, NoseY, Depth), NoseY < Depth);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandBoxFarSideTest,
	"Airside.Solve.StandBox.FarSide",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandBoxFarSideTest::RunTest(const FString& Parameters)
{
	// A STAND DRAWN BELOW THE TAXIWAY, width dragged backwards (B before A) and Inward
	// pointing down: PoseFor must follow whatever Inward it is handed, not assume "up".
	const EIcaoCode Letter = EIcaoCode::C;
	const FVector2D A(IcaoCode::StandWidthForLetter(Letter), 0.0);
	const FVector2D B(0.0, 0.0);
	const FVector2D Inward(0.0, -1.0);

	const StandBox::FStandPose Pose =
		StandBox::PoseFor(A, B, Inward, {}, Letter, IcaoCode::FloorEnvelopeForLetter(Letter));
	TestTrue(TEXT("Facing equals Inward"), Pose.Facing.Equals(Inward, 1e-9));
	TestTrue(TEXT("Position.Y is negative - the pose sits below the taxiway"), Pose.Position.Y < 0.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandBoxNoLetterTest,
	"Airside.Solve.StandBox.NoLetter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandBoxNoLetterTest::RunTest(const FString& Parameters)
{
	// TOO SMALL FOR ANY LETTER: a 20 x 20 m rect is under Code A's floor in both dimensions,
	// so LetterOf must be unset rather than guessing at Code A - the same refusal
	// LetterForStandSize itself makes.
	const TArray<FVector2D> TooSmall = {
		FVector2D(0.0, 0.0), FVector2D(2000.0, 0.0), FVector2D(2000.0, 2000.0), FVector2D(0.0, 2000.0)
	};
	TestFalse(TEXT("20 x 20 m is smaller than any letter"), StandBox::LetterOf(TooSmall).IsSet());

	// THE SPEC TABLE'S OWN EXAMPLE: a wide-but-shallow rect reads as the letter its DEPTH
	// allows, not its width - 67 x 40 m is wide enough for Code D (81 m floor) but nowhere
	// near deep enough, so it reads as whatever letter IcaoCode::LetterForStandSize actually
	// gives a 40 m depth. Computed from the table rather than retyped, so this test cannot
	// drift from the table it is pinning.
	const double WidthUu = 6700.0;
	// 40 m, not 30, since the 2026-09-26 depth rise put Code A's floor at 32 m.
	const double DepthUu = 4000.0;
	const TOptional<EIcaoCode> Expected = IcaoCode::Parse(IcaoCode::LetterForStandSize(WidthUu, DepthUu));
	TestTrue(TEXT("67 x 40 m is wide-but-shallow and still gets a letter"), Expected.IsSet());

	const TArray<FVector2D> WideButShallow = {
		FVector2D(0.0, 0.0), FVector2D(WidthUu, 0.0), FVector2D(WidthUu, DepthUu), FVector2D(0.0, DepthUu)
	};
	const TOptional<EIcaoCode> Actual = StandBox::LetterOf(WideButShallow);
	TestTrue(TEXT("67 x 40 m reads back a letter"), Actual.IsSet());
	if (Expected.IsSet() && Actual.IsSet())
	{
		TestEqual(TEXT("67 x 40 m reads as the letter its DEPTH allows, not its width"),
			Actual.GetValue(), Expected.GetValue());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandBoxMeasuresWholeUuTest,
	"Airside.Solve.StandBox.MeasuresWholeUu",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandBoxMeasuresWholeUuTest::RunTest(const FString& Parameters)
{
	// A DIAGONAL RECTANGLE EXACTLY AT CODE C'S FLOOR, built the way the stand tool builds one:
	// a corner plus a unit direction times a whole-uu length. Off the axes the unit vector is
	// 1 give or take an ulp, so the raw edge length lands an ulp either side of the floor -
	// and LetterForStandSize's exact >= reads the low side as the letter below.
	const EIcaoCode Letter = EIcaoCode::C;
	const double W = IcaoCode::StandWidthForLetter(Letter);
	const double D = IcaoCode::StandDepthForLetter(Letter);

	int32 Inexact = 0;
	for (int32 Step = 1; Step < 360; ++Step)
	{
		const double Angle = FMath::DegreesToRadians(Step * 0.73 + 0.1234);
		const FVector2D Along = FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)).GetSafeNormal();
		const FVector2D Inward = RoadGeom::PerpCCW(Along);
		const FVector2D Origin(1234.5678, -987.6543);
		const TArray<FVector2D> Rect = {
			Origin, Origin + Along * W, Origin + Along * W + Inward * D, Origin + Inward * D };

		// COUNTED, so the test says whether the raw lengths really were inexact here - a sweep
		// where every length came out exact would pass without having measured the rounding.
		if ((Rect[1] - Rect[0]).Length() != W || (Rect[2] - Rect[1]).Length() != D) { ++Inexact; }

		TestEqual(TEXT("WidthOf is a whole uu, so a floor drawn exactly reads as the floor"),
			StandBox::WidthOf(Rect), W);
		TestEqual(TEXT("DepthOf is a whole uu, for the same reason"), StandBox::DepthOf(Rect), D);

		const TOptional<EIcaoCode> Read = StandBox::LetterOf(Rect);
		if (TestTrue(TEXT("the diagonal floor rect reads as a letter"), Read.IsSet()))
		{
			TestEqual(TEXT("and it is the letter whose floor it was drawn at"), *Read, Letter);
		}
	}
	TestTrue(TEXT("the sweep met raw lengths an ulp off the floor, so the rounding was exercised"),
		Inexact > 0);
	return true;
}

/**
 * THE ENTRANCE EDGE IS THE ONE WHOSE MIDPOINT LIES FURTHEST BEHIND THE STOP MARK ALONG ITS FACING (#450's leftover).
 *
 * The search UStandDefinitionCache::PoseFromOutline ran on every load, moved here for the one writer that still needs it
 * (PlaceEntity and EnsureStandFrontages, for a stand given no edge). Pinned on a box built by BoxAt - whose entrance is edge 0 by construction -
 * at every heading, in BOTH windings (the facade reverses a clockwise outline, which puts the drawn FAR edge at 0 -> 1, so "index 0" is not the
 * entrance there), and on the degenerate outlines.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandBoxEntranceEdgeTest,
	"Airside.Solve.StandBox.EntranceEdgeIsTheRearmostMidpoint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandBoxEntranceEdgeTest::RunTest(const FString& Parameters)
{
	const EIcaoCode Letter = EIcaoCode::C;
	const FLetterEnvelope Envelope = IcaoCode::FloorEnvelopeForLetter(Letter);
	int32 Headings = 0;
	for (int32 Step = 0; Step < 24; ++Step)
	{
		const double Heading = FMath::DegreesToRadians(Step * 15.0 + 3.0);
		StandBox::FStandPose Pose;
		Pose.Position = FVector2D(1234.5, -6789.0);
		Pose.Facing = FVector2D(FMath::Cos(Heading), FMath::Sin(Heading));
		TArray<FVector2D> Box;
		StandBox::BoxAt(Pose, Letter, Envelope, Box);

		TestEqual(TEXT("BoxAt's own entrance is its edge 0, and the search finds it at every heading"),
			StandBox::EntranceEdgeOf(Box, Pose.Position, Pose.Facing), 0);

		// THE FACADE'S REVERSAL: the same box wound the other way. The entrance is the same two points, now at 2 -> 3 (Box[1], Box[0] reversed).
		TArray<FVector2D> Reversed = Box;
		Algo::Reverse(Reversed);
		const int32 Edge = StandBox::EntranceEdgeOf(Reversed, Pose.Position, Pose.Facing);
		if (TestTrue(TEXT("a reversed box still has an entrance"), Edge != INDEX_NONE))
		{
			const FVector2D A = Reversed[Edge];
			const FVector2D B = Reversed[(Edge + 1) % Reversed.Num()];
			TestTrue(TEXT("and it is the same two points (in the other order), not index 0 - which is the far edge now"),
				(A == Box[1] && B == Box[0]) && Edge != 0);
		}
		++Headings;
	}
	TestEqual(TEXT("every heading was measured"), Headings, 24);

	// AN OUTLINE OF UNDER THREE POINTS has no edge to name - INDEX_NONE, never a read past the array.
	TestEqual(TEXT("no outline: none"), StandBox::EntranceEdgeOf(TArray<FVector2D>(), FVector2D::ZeroVector, FVector2D(1.0, 0.0)), static_cast<int32>(INDEX_NONE));
	const TArray<FVector2D> Two = { FVector2D(0.0, 0.0), FVector2D(100.0, 0.0) };
	TestEqual(TEXT("two points: none"), StandBox::EntranceEdgeOf(Two, FVector2D::ZeroVector, FVector2D(1.0, 0.0)), static_cast<int32>(INDEX_NONE));

	// AN EXACT TIE KEEPS THE FIRST EDGE, as the reader did: a diamond facing +X has its two rear edges (2 and 3) meeting at the rear vertex, their
	// midpoints exactly level along X.
	const TArray<FVector2D> Diamond = { FVector2D(0.0, -100.0), FVector2D(100.0, 0.0), FVector2D(0.0, 100.0), FVector2D(-100.0, 0.0) };
	TestEqual(TEXT("two rear edges tie exactly: the first one wins"),
		StandBox::EntranceEdgeOf(Diamond, FVector2D::ZeroVector, FVector2D(1.0, 0.0)), 2);
	return true;
}

#endif
