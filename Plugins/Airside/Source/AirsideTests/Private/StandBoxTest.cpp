#include "CoreMinimal.h"
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
	for (const EIcaoCode Letter : { EIcaoCode::A, EIcaoCode::B, EIcaoCode::C, EIcaoCode::D, EIcaoCode::E, EIcaoCode::F })
	{
		const double W = IcaoCode::StandWidthForLetter(Letter);
		const double D = IcaoCode::StandDepthForLetter(Letter);
		const FVector2D A(0.0, 0.0);
		const FVector2D B(W, 0.0);
		const FVector2D Inward(0.0, 1.0);

		const FLetterEnvelope Envelope = IcaoCode::FloorEnvelopeForLetter(Letter);
		const StandBox::FStandPose Pose = StandBox::PoseFor(A, B, Inward, Letter, Envelope);
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
	const StandBox::FStandPose Pose = StandBox::PoseFor(A, B, Inward, Letter, Envelope);
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

		const StandBox::FStandPose Pose = StandBox::PoseFor(A, B, Inward, Letter, Envelope);

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
		// edge, where the mirrored template (Task 3) will lay the far-side entry's service ground.
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
		StandBox::PoseFor(A, B, Inward, Letter, IcaoCode::FloorEnvelopeForLetter(Letter));
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

#endif
