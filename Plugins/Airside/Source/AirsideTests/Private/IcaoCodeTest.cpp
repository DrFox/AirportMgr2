#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/IcaoCode.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FIcaoCodeTest,
	"Airside.Solve.IcaoCode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FIcaoCodeTest::RunTest(const FString& Parameters)
{
	// 1. LETTER FOR WINGSPAN. Boundaries pinned so a future edit to the table cannot drift
	//    one letter without a test noticing - these are the same figures InspectFacts's
	//    panel test checks against a real stand.
	{
		TestEqual(TEXT("14 m is A"), IcaoCode::LetterForWingspan(1400.0), FString(TEXT("A")));
		TestEqual(TEXT("18 m is B"), IcaoCode::LetterForWingspan(1800.0), FString(TEXT("B")));
		TestEqual(TEXT("30 m is C"), IcaoCode::LetterForWingspan(3000.0), FString(TEXT("C")));
		TestEqual(TEXT("45 m is D"), IcaoCode::LetterForWingspan(4500.0), FString(TEXT("D")));
		TestEqual(TEXT("60 m is E"), IcaoCode::LetterForWingspan(6000.0), FString(TEXT("E")));
		TestEqual(TEXT("70 m is F"), IcaoCode::LetterForWingspan(7000.0), FString(TEXT("F")));
	}

	// 2. MAX WINGSPAN FOR WIDTH. The same three cases RunwayAdmission's own test pins,
	//    unwired here to prove the table moved rather than got re-typed.
	{
		TestEqual(TEXT("code A admits 15 m"), IcaoCode::MaxWingspanForWidth(1800.0), 1500.0);
		TestEqual(TEXT("code F admits 80 m"), IcaoCode::MaxWingspanForWidth(6000.0), 8000.0);
		TestEqual(TEXT("an odd width takes the nearest code"), IcaoCode::MaxWingspanForWidth(4000.0), 6500.0);

		// 45 m serves both D and E - the WIDER figure wins, because that is the one the
		// width was actually chosen for (see the table's own comment).
		TestEqual(TEXT("45 m resolves to E, the wider of the two it serves"),
			IcaoCode::MaxWingspanForWidth(4500.0), 6500.0);

		// 26.5 m is equidistant between B (23 m) and C (30 m) - two DIFFERENT widths, not
		// one width shared by two letters, so this tie keeps the EARLIER, narrower row
		// rather than reusing the D/E rule. Pinned so that rule cannot drift to cover ties
		// it was never meant for.
		TestEqual(TEXT("an odd width exactly between two different codes keeps the earlier one"),
			IcaoCode::MaxWingspanForWidth(2650.0), 2400.0);
	}

	// 3. RADIUS FOR LETTER. Case-insensitive, and an unrecognised letter falls back to C
	//    rather than the widest or narrowest curve.
	{
		TestEqual(TEXT("A"), IcaoCode::RadiusForLetter(TEXT("A")), 1500.0);
		TestEqual(TEXT("lower-case b matches too"), IcaoCode::RadiusForLetter(TEXT("b")), 2000.0);
		TestEqual(TEXT("F"), IcaoCode::RadiusForLetter(TEXT("F")), 6000.0);
		TestEqual(TEXT("an empty code falls back to C"), IcaoCode::RadiusForLetter(TEXT("")), 2500.0);
		TestEqual(TEXT("an unrecognised code falls back to C"), IcaoCode::RadiusForLetter(TEXT("Z")), 2500.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandWidthIsDerivedFromClearanceTest,
	"Airside.Solve.StandWidthIsDerivedFromClearance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandWidthIsDerivedFromClearanceTest::RunTest(const FString& Parameters)
{
	// THE DERIVATION, NOT ITS OUTPUT. Asserting 5900 for Code C would pass just as well against
	// a typed 5900, which is the thing this change exists to stop. A stand's MINIMUM width is
	// the span band, plus twice the wingtip clearance, plus twice a service lane, plus the aft
	// edge's own allowance - every letter, to the centimetre - so that is what is asserted, and
	// the figures move together or the test fails.
	struct FCase { const TCHAR* Letter; double Span; double Clearance; double AftEdge; };
	const FCase Cases[] = {
		{ TEXT("A"), 1500.0, 300.0, 600.0 },
		{ TEXT("B"), 2400.0, 300.0, 600.0 },
		{ TEXT("C"), 3600.0, 450.0, 600.0 },
		{ TEXT("D"), 5200.0, 750.0, 600.0 },
		{ TEXT("E"), 6500.0, 750.0, 600.0 },
		{ TEXT("F"), 8000.0, 750.0, 600.0 },
	};

	for (const FCase& Case : Cases)
	{
		TestEqual(
			*FString::Printf(TEXT("stand %s is its span, clearances, lanes and aft edge"),
				Case.Letter),
			IcaoCode::StandWidthForLetter(Case.Letter),
			Case.Span + 2.0 * (Case.Clearance + IcaoCode::ServiceLaneWidth()) + Case.AftEdge,
			0.5);
	}

	// THE BANDS TILE. A width belonging to no letter, or to two, is what a stored maximum
	// beside the next row's minimum would eventually produce; derived, one letter's ceiling IS
	// the next one's floor, and every width above Code A's floor has exactly one answer.
	const TCHAR* Ladder[] = { TEXT("A"), TEXT("B"), TEXT("C"), TEXT("D"), TEXT("E") };
	for (const TCHAR* Letter : Ladder)
	{
		TestEqual(
			*FString::Printf(TEXT("%s's ceiling is the next letter's floor"), Letter),
			IcaoCode::MaxStandWidthForLetter(Letter),
			IcaoCode::StandWidthForLetter(
				FString::Chr(static_cast<TCHAR>(Letter[0] + 1))),
			0.5);
	}
	TestTrue(TEXT("Code F has no ceiling - nothing is too wide to be a stand"),
		IcaoCode::MaxStandWidthForLetter(TEXT("F")) > 1.0e9);

	// AND THE MIRROR. A stand's SIZE decides which airframes may use it, which is the mechanic:
	// a player who drags a bigger stand gets bigger aircraft as a consequence.
	//
	// BOTH DIMENSIONS, NEVER ONE. Width alone would call a 67 x 30 m stand Code D, when nothing
	// bigger than a King Air fits in 30 m of depth. The letter is the largest whose width AND
	// depth both fit, and the shallow case below is the one that discriminates.
	TestEqual(TEXT("59 x 55 m is a Code C stand - exactly its floor"),
		IcaoCode::LetterForStandSize(5900.0, 5500.0), FString(TEXT("C")));
	TestEqual(TEXT("76 x 55 m is still Code C - D needs 81 m of width"),
		IcaoCode::LetterForStandSize(7600.0, 5500.0), FString(TEXT("C")));
	TestEqual(TEXT("81 x 70 m is genuinely Code D"),
		IcaoCode::LetterForStandSize(8100.0, 7000.0), FString(TEXT("D")));
	TestEqual(TEXT("81 x 30 m is a Code B - D-wide but far too shallow"),
		IcaoCode::LetterForStandSize(8100.0, 3000.0), FString(TEXT("B")));
	TestEqual(TEXT("59 x 90 m is a Code C - deep, but the span binds"),
		IcaoCode::LetterForStandSize(5900.0, 9000.0), FString(TEXT("C")));

	// A HAIR UNDER THE FLOOR IS THE LETTER BELOW, which is the property a player dragging a
	// polygon actually feels: the stand does not "nearly" admit a 737, it admits a Dash 8.
	TestEqual(TEXT("a centimetre under Code C's floor is a Code B stand"),
		IcaoCode::LetterForStandSize(5899.0, 5500.0), FString(TEXT("B")));

	// Below the smallest stand there is no letter to give, and saying "A" would admit a Cessna
	// to a space it does not fit. Empty means "no stand of any letter fits this", which is a
	// real answer: such a stand is refused at placement.
	TestTrue(TEXT("under the smallest stand, no letter"),
		IcaoCode::LetterForStandSize(2000.0, 2000.0).IsEmpty());

	// Depth is authored rather than derived, so it is asserted by value - with its provenance
	// in the table, which is where a reader checks it against a real aerodrome.
	TestEqual(TEXT("a Code C stand is 55 m deep"), IcaoCode::StandDepthForLetter(TEXT("C")), 5500.0, 0.5);

	// AND HOW LONG AN AIRFRAME THE LETTER ADMITS, which is what a stand's ground geometry is
	// kept clear of. Code C's figure is MEASURED - it is the 737-800's tail, the longest type
	// this project ships - so it is pinned here against the same figure Build737 authors, and
	// the two cannot drift without a test saying so. The letters with no shipped type are
	// authored design values and are asserted only for their ORDER, which is the one thing
	// that must hold however the figures are revised.
	TestEqual(TEXT("Code C admits the 737-800's tail, and is measured from it"),
		IcaoCode::MaxTailAftForLetter(TEXT("C")), 3430.0, 0.5);

	double Previous = 0.0;
	for (const TCHAR* Letter : { TEXT("A"), TEXT("B"), TEXT("C"), TEXT("D"), TEXT("E"), TEXT("F") })
	{
		const double Aft = IcaoCode::MaxTailAftForLetter(Letter);
		TestTrue(
			*FString::Printf(TEXT("%s admits a longer airframe than the letter below it (%.0f after %.0f)"),
				Letter, Aft, Previous),
			Aft > Previous);
		Previous = Aft;
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWingKeepOutIsTheUnionOfAdmittedWingsTest,
	"Airside.Solve.WingKeepOutIsTheUnionOfAdmittedWings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWingKeepOutIsTheUnionOfAdmittedWingsTest::RunTest(const FString& Parameters)
{
	// THE SHAPE OF THE RULE, not one aeroplane's wing. A stand admits several airframes and
	// the ground is marked once for all of them, which is what a real apron paints under a
	// large swept wing. So the band has to be ordered, negative, and inside the airframe it
	// belongs to - every letter.
	for (const TCHAR* Letter : { TEXT("A"), TEXT("B"), TEXT("C"), TEXT("D"), TEXT("E"), TEXT("F") })
	{
		const double Fwd = IcaoCode::WingFwdForLetter(Letter);
		const double Aft = IcaoCode::WingAftForLetter(Letter);

		TestTrue(*FString::Printf(TEXT("%s's wing band runs aft to fwd (%.0f .. %.0f)"),
			Letter, Aft, Fwd), Aft < Fwd);
		TestTrue(*FString::Printf(TEXT("%s's wing is behind the stop mark (%.0f)"), Letter, Fwd),
			Fwd < 0.0);
		TestTrue(*FString::Printf(TEXT("%s's wing is inside its own airframe (%.0f vs %.0f)"),
			Letter, Aft, -IcaoCode::MaxTailAftForLetter(Letter)),
			Aft > -IcaoCode::MaxTailAftForLetter(Letter));
	}

	// CONTAINMENT, at the corners that decide it. Code C's box is x in [-2150, -950] out to
	// the span band's half, 1800.
	TestTrue(TEXT("under the wing root is inside"),
		IcaoCode::WingKeepOutContains(TEXT("C"), FVector2D(-1500.0, 700.0)));
	TestFalse(TEXT("forward of the leading edge is clear"),
		IcaoCode::WingKeepOutContains(TEXT("C"), FVector2D(-800.0, 700.0)));
	TestFalse(TEXT("aft of the trailing edge is clear"),
		IcaoCode::WingKeepOutContains(TEXT("C"), FVector2D(-2400.0, 700.0)));
	TestFalse(TEXT("outboard of the wingtip is clear"),
		IcaoCode::WingKeepOutContains(TEXT("C"), FVector2D(-1500.0, 1900.0)));

	// AND THE SEGMENT TEST, which is the one a route is judged by. The first case is the whole
	// point of clipping rather than sampling: both ENDS are clear and the middle is not.
	TestTrue(TEXT("a lane crossing under the wing is caught, though both ends are clear"),
		IcaoCode::WingKeepOutCrossedBy(TEXT("C"),
			FVector2D(-1500.0, -1900.0), FVector2D(-1500.0, 1900.0)));
	TestFalse(TEXT("the same crossing made aft of the trailing edge is clear"),
		IcaoCode::WingKeepOutCrossedBy(TEXT("C"),
			FVector2D(-2400.0, -1900.0), FVector2D(-2400.0, 1900.0)));
	TestFalse(TEXT("and forward of the leading edge is clear"),
		IcaoCode::WingKeepOutCrossedBy(TEXT("C"),
			FVector2D(-800.0, -1900.0), FVector2D(-800.0, 1900.0)));
	TestFalse(TEXT("a lane running along outside the tip is clear"),
		IcaoCode::WingKeepOutCrossedBy(TEXT("C"),
			FVector2D(-4000.0, 2450.0), FVector2D(500.0, 2450.0)));

	// A DIAGONAL FROM ONE CLEAR SIDE TO THE OTHER IS NOT CLEAR, and this is the case the
	// first draft of this test got wrong: both endpoints sit outside the box and the line
	// between them goes straight through it. Getting round the wing means going round the
	// TIP, which no single segment across the centreline can do.
	TestTrue(TEXT("a diagonal between two clear points still crosses"),
		IcaoCode::WingKeepOutCrossedBy(TEXT("C"),
			FVector2D(-2400.0, -1900.0), FVector2D(-800.0, 1900.0)));

	// AND CONTACT COUNTS. A lane laid exactly on the wingtip is a lane under the wingtip.
	TestTrue(TEXT("a lane laid exactly on the tip is not clear of it"),
		IcaoCode::WingKeepOutCrossedBy(TEXT("C"),
			FVector2D(-4000.0, 1800.0), FVector2D(500.0, 1800.0)));

	return true;
}

#endif
