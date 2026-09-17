#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/GuideArbiter.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A candidate pointing Degrees off +X, named after its own angle so a failure says
	 *  which one won rather than which index did. */
	SnapGuide::FCandidate GuideAt(double Degrees, SnapGuide::ESource Source)
	{
		SnapGuide::FCandidate Candidate;
		const double Radians = FMath::DegreesToRadians(Degrees);
		Candidate.Direction = FVector2D(FMath::Cos(Radians), FMath::Sin(Radians));
		Candidate.Description = FString::Printf(TEXT("%.1f degrees"), Degrees);
		Candidate.Source = Source;
		return Candidate;
	}

	/** A cursor Reach uu from the origin, Degrees off +X. */
	FVector2D CursorAt(double Degrees, double Reach = 1000.0)
	{
		const double Radians = FMath::DegreesToRadians(Degrees);
		return FVector2D(FMath::Cos(Radians), FMath::Sin(Radians)) * Reach;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideArbiterPicksTheNearestTest,
	"Airside.Solve.GuideArbiterPicksTheNearest",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideArbiterPicksTheNearestTest::RunTest(const FString& Parameters)
{
	const TArray<SnapGuide::FCandidate> Candidates = {
		GuideAt(0.0, SnapGuide::ESource::World),
		GuideAt(45.0, SnapGuide::ESource::World) };

	const SnapGuide::FResult Result = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, CursorAt(4.0), SnapGuide::FResult());

	if (!TestTrue(TEXT("a cursor 4 degrees off an axis is offered a guide"), Result.bActive))
	{
		return false;
	}

	// BY DESCRIPTION, not by index: this asserts the WINNER travelled out whole, which is
	// what the overlay's label is drawn from.
	TestEqual(TEXT("the nearer candidate wins on smallest angular error"),
		Result.Winner.Description, FString(TEXT("0.0 degrees")));

	// THE POINT IS THE PERPENDICULAR PROJECTION onto the winner's line through the origin -
	// the tool uses this directly as its corner, so an answer that merely pointed the right
	// way would put the corner somewhere the player did not drag it.
	TestTrue(TEXT("the constrained point lands exactly on the winning line"),
		FMath::IsNearlyZero(Result.Point.Y, 1.0e-6));
	TestTrue(TEXT("and at the cursor's own reach along it"),
		FMath::IsNearlyEqual(Result.Point.X, 1000.0 * FMath::Cos(FMath::DegreesToRadians(4.0)), 1.0e-6));

	// A GUIDE IS A LINE, NOT A RAY. The same axis must still claim a cursor dragged 180
	// degrees the other way, or a corner would lose its square the instant it crossed the
	// origin - which is the case the World source proposing four directions depends on.
	const SnapGuide::FResult Behind = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, CursorAt(184.0), SnapGuide::FResult());
	TestTrue(TEXT("a candidate and its opposite are one guide"), Behind.bActive);
	TestTrue(TEXT("and the point projects onto the far side"), Behind.Point.X < 0.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideArbiterBreaksTiesBySourceTest,
	"Airside.Solve.GuideArbiterBreaksTiesBySource",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideArbiterBreaksTiesBySourceTest::RunTest(const FString& Parameters)
{
	// THE SAME ANGLE FROM TWO SOURCES: a frontage that happens to lie on a world axis. This
	// is not contrived - a plot drawn off an east-west service road produces it every time.
	const SnapGuide::FCandidate Extending = GuideAt(45.0, SnapGuide::ESource::Extending);
	const SnapGuide::FCandidate World = GuideAt(45.0, SnapGuide::ESource::World);

	const TArray<SnapGuide::FCandidate> ExtendingFirst = { Extending, World };
	const TArray<SnapGuide::FCandidate> WorldFirst = { World, Extending };

	const SnapGuide::FResult A = SnapGuide::Arbitrate(
		ExtendingFirst, FVector2D::ZeroVector, CursorAt(48.0), SnapGuide::FResult());
	const SnapGuide::FResult B = SnapGuide::Arbitrate(
		WorldFirst, FVector2D::ZeroVector, CursorAt(48.0), SnapGuide::FResult());

	// IN BOTH ORDERS. A tiebreak that depended on the order candidates happened to be
	// gathered in would be a guide that changed with the network's iteration order - i.e.
	// with an edit nobody connected to guides at all.
	TestEqual(TEXT("a tie goes to the higher-priority source"),
		static_cast<int32>(A.Winner.Source), static_cast<int32>(SnapGuide::ESource::Extending));
	TestEqual(TEXT("and does so whichever order the candidates arrived in"),
		static_cast<int32>(B.Winner.Source), static_cast<int32>(SnapGuide::ESource::Extending));

	// CONTROL LEG: source order is the TIEBREAK and nothing else. A World candidate that is
	// genuinely nearer must still win, or the test above would pass on an arbiter that
	// simply always preferred Extending.
	const TArray<SnapGuide::FCandidate> NearerWorld = {
		GuideAt(45.0, SnapGuide::ESource::Extending),
		GuideAt(47.0, SnapGuide::ESource::World) };
	const SnapGuide::FResult C = SnapGuide::Arbitrate(
		NearerWorld, FVector2D::ZeroVector, CursorAt(48.0), SnapGuide::FResult());
	TestEqual(TEXT("but a nearer low-priority source still wins outright"),
		static_cast<int32>(C.Winner.Source), static_cast<int32>(SnapGuide::ESource::World));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideArbiterHoldsItsWinnerTest,
	"Airside.Solve.GuideArbiterHoldsItsWinner",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideArbiterHoldsItsWinnerTest::RunTest(const FString& Parameters)
{
	// THE FLICKER RULE, and the reason the arbiter is a separate unit at all. Two candidates
	// four degrees apart: the incumbent on the axis, the challenger just off it.
	const TArray<SnapGuide::FCandidate> Candidates = {
		GuideAt(0.0, SnapGuide::ESource::Extending),
		GuideAt(4.0, SnapGuide::ESource::World) };

	SnapGuide::FResult Previous;
	Previous.bActive = true;
	Previous.Winner = GuideAt(0.0, SnapGuide::ESource::Extending);

	const SnapGuide::FTuning Tuning;   // 7 degrees tolerance, 2 degrees stickiness

	// Cursor at 2.5 degrees: the incumbent is 2.5 off, the challenger 1.5 off. Better by 1,
	// comfortably inside the stickiness - so the guide holds. WELL CLEAR OF THE BOUNDARY on
	// purpose: this leg is about the rule, and a leg that sat on the discontinuity would be
	// measuring the last bit of an acos instead (see the boundary leg below, which is).
	const SnapGuide::FResult Held = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, CursorAt(2.5), Previous, Tuning);
	TestTrue(TEXT("a guide the cursor is still near stays active"), Held.bActive);
	TestEqual(TEXT("a challenger better by less than the stickiness does not take the guide"),
		static_cast<int32>(Held.Winner.Source), static_cast<int32>(SnapGuide::ESource::Extending));

	// EXACTLY THE STICKINESS: incumbent 3 off, challenger 1 off. A slow drag passes through
	// this every time, and the bare comparison decided it on floating-point noise - this
	// test caught that, and the arbiter now resolves the tie to the INCUMBENT. Pinned here
	// so the resolution is a stated rule rather than whatever the arithmetic happens to do.
	const SnapGuide::FResult Boundary = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, CursorAt(3.0), Previous, Tuning);
	TestEqual(TEXT("a challenger better by exactly the stickiness still does not take it"),
		static_cast<int32>(Boundary.Winner.Source), static_cast<int32>(SnapGuide::ESource::Extending));

	// Cursor at 3.9 degrees: incumbent 3.9 off, challenger 0.1 off - better by 3.8, and the
	// handover happens. One guide holding then handing over is the feel being protected;
	// never handing over would be as bad as flickering.
	const SnapGuide::FResult Taken = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, CursorAt(3.9), Previous, Tuning);
	TestEqual(TEXT("a challenger better by more than the stickiness does take it"),
		static_cast<int32>(Taken.Winner.Source), static_cast<int32>(SnapGuide::ESource::World));

	// AND AN INCUMBENT OUT OF TOLERANCE IS DROPPED however sticky it is: at 8.5 degrees the
	// held axis is past the 7-degree tolerance, so stickiness must not resurrect it.
	const SnapGuide::FResult Dropped = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, CursorAt(8.5), Previous, Tuning);
	TestTrue(TEXT("an incumbent out of tolerance is still dropped"), Dropped.bActive);
	TestEqual(TEXT("and the eligible challenger takes over"),
		static_cast<int32>(Dropped.Winner.Source), static_cast<int32>(SnapGuide::ESource::World));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideArbiterRefusesOutsideToleranceTest,
	"Airside.Solve.GuideArbiterRefusesOutsideTolerance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideArbiterRefusesOutsideToleranceTest::RunTest(const FString& Parameters)
{
	const TArray<SnapGuide::FCandidate> Candidates = { GuideAt(0.0, SnapGuide::ESource::World) };

	const SnapGuide::FResult Far = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, CursorAt(20.0), SnapGuide::FResult());
	TestFalse(TEXT("nothing within tolerance means no guide, not a nearest-anyway answer"),
		Far.bActive);

	// CONTROL LEG: the same candidate and the same call DOES fire in tolerance, so the
	// assertion above is measuring the tolerance and not a broken call.
	const SnapGuide::FResult Near = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, CursorAt(5.0), SnapGuide::FResult());
	TestTrue(TEXT("and the same candidate five degrees off does fire"), Near.bActive);

	// A CURSOR ON THE ORIGIN HAS NO DIRECTION. The first frame of every drag is exactly this,
	// and an arbiter that answered here would flash a guide on before the player moved.
	const SnapGuide::FResult Degenerate = SnapGuide::Arbitrate(
		Candidates, FVector2D(500.0, 500.0), FVector2D(500.0, 500.0), SnapGuide::FResult());
	TestFalse(TEXT("a cursor on top of the origin is offered nothing"), Degenerate.bActive);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideArbiterIsStableUnderRepetitionTest,
	"Airside.Solve.GuideArbiterIsStableUnderRepetition",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideArbiterIsStableUnderRepetitionTest::RunTest(const FString& Parameters)
{
	// FBuildSession::MakeContext runs about three times per frame - the driver's Tick, its
	// BuildReadout, and the HUD's own call - and each one feeds the last answer back in as
	// Previous. So "last frame's winner" is really "last call's", and a rule that drifted on
	// repetition would drift WITHIN one frame, which no amount of PIE would make legible.
	const TArray<SnapGuide::FCandidate> Candidates = {
		GuideAt(0.0, SnapGuide::ESource::Extending),
		GuideAt(4.0, SnapGuide::ESource::World) };

	const FVector2D Cursor = CursorAt(3.0);

	SnapGuide::FResult Carried = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, Cursor, SnapGuide::FResult());
	if (!TestTrue(TEXT("the first resolution answers"), Carried.bActive)) { return false; }

	const int32 FirstSource = static_cast<int32>(Carried.Winner.Source);
	const FVector2D FirstPoint = Carried.Point;

	for (int32 Repeat = 0; Repeat < 5; ++Repeat)
	{
		Carried = SnapGuide::Arbitrate(Candidates, FVector2D::ZeroVector, Cursor, Carried);
		TestEqual(FString::Printf(
			TEXT("resolving again at one cursor keeps the same winner (repeat %d)"), Repeat),
			static_cast<int32>(Carried.Winner.Source), FirstSource);
		TestTrue(FString::Printf(
			TEXT("and the same constrained point (repeat %d)"), Repeat),
			Carried.Point.Equals(FirstPoint, 1.0e-9));
	}

	return true;
}

#endif
