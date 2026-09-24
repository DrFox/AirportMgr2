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

	// 3. RADIUS FOR LETTER, keyed on the enum. Case-insensitivity and the fallback for a
	//    string nobody recognises are Parse's rules now, not this table's - an EIcaoCode
	//    cannot BE an unrecognised letter, so there is nothing here left to fall back from.
	//    See FIcaoCodeParseTest.
	{
		TestEqual(TEXT("A"), IcaoCode::RadiusForLetter(EIcaoCode::A), 1500.0);
		TestEqual(TEXT("B"), IcaoCode::RadiusForLetter(EIcaoCode::B), 2000.0);
		TestEqual(TEXT("F"), IcaoCode::RadiusForLetter(EIcaoCode::F), 6000.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FIcaoCodeParseTest,
	"Airside.Solve.IcaoCodeParse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FIcaoCodeParseTest::RunTest(const FString& Parameters)
{
	// THE ONLY PLACE A TYPO BECOMES A DECISION now, so its rule is pinned by name: TRIMMED
	// then UPPERCASED, once, and anything left over that is not exactly one of A-F is
	// nullopt rather than a guess. Every table function used to make this decision itself,
	// silently, on every call - see the header for why that was the defect.
	TestTrue(TEXT("lower-case parses"), IcaoCode::Parse(TEXT("c")).IsSet());
	TestEqual(TEXT("lower-case parses to the right letter"),
		IcaoCode::Parse(TEXT("c")).Get(EIcaoCode::A), EIcaoCode::C);
	TestTrue(TEXT("upper-case parses"), IcaoCode::Parse(TEXT("C")).IsSet());
	TestEqual(TEXT("surrounding whitespace is trimmed, not just leading"),
		IcaoCode::Parse(TEXT(" C ")).Get(EIcaoCode::A), EIcaoCode::C);
	TestFalse(TEXT("an unrecognised letter is nullopt, not a guess at Code C"),
		IcaoCode::Parse(TEXT("X")).IsSet());
	TestFalse(TEXT("empty is nullopt"), IcaoCode::Parse(TEXT("")).IsSet());
	TestFalse(TEXT("more than one character is nullopt, even if it starts with a real letter"),
		IcaoCode::Parse(TEXT("CC")).IsSet());

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
	struct FCase { EIcaoCode Code; const TCHAR* Letter; double Span; double Clearance; double AftEdge; };
	const FCase Cases[] = {
		{ EIcaoCode::A, TEXT("A"), 1500.0, 300.0, 600.0 },
		{ EIcaoCode::B, TEXT("B"), 2400.0, 300.0, 600.0 },
		{ EIcaoCode::C, TEXT("C"), 3600.0, 450.0, 600.0 },
		{ EIcaoCode::D, TEXT("D"), 5200.0, 750.0, 600.0 },
		{ EIcaoCode::E, TEXT("E"), 6500.0, 750.0, 600.0 },
		{ EIcaoCode::F, TEXT("F"), 8000.0, 750.0, 600.0 },
	};

	for (const FCase& Case : Cases)
	{
		TestEqual(
			*FString::Printf(TEXT("stand %s is its span, clearances, lanes and aft edge"),
				Case.Letter),
			IcaoCode::StandWidthForLetter(Case.Code),
			Case.Span + 2.0 * (Case.Clearance + IcaoCode::ServiceLaneWidth()) + Case.AftEdge,
			0.5);
	}

	// THE BANDS TILE. A width belonging to no letter, or to two, is what a stored maximum
	// beside the next row's minimum would eventually produce; derived, one letter's ceiling IS
	// the next one's floor, and every width above Code A's floor has exactly one answer.
	const EIcaoCode Ladder[] = { EIcaoCode::A, EIcaoCode::B, EIcaoCode::C, EIcaoCode::D, EIcaoCode::E };
	for (const EIcaoCode Code : Ladder)
	{
		// The enum's own ordinal gives "the next letter" now that FString::Chr(Letter[0] + 1)
		// has nothing to increment - one more reason the table functions are safer keyed this
		// way: there is no letter past F for this to walk off the end of, by construction.
		const EIcaoCode Next = static_cast<EIcaoCode>(static_cast<uint8>(Code) + 1);
		TestEqual(
			*FString::Printf(TEXT("%s's ceiling is the next letter's floor"), IcaoCode::ToLetter(Code)),
			IcaoCode::MaxStandWidthForLetter(Code),
			IcaoCode::StandWidthForLetter(Next),
			0.5);
	}
	TestTrue(TEXT("Code F has no ceiling - nothing is too wide to be a stand"),
		IcaoCode::MaxStandWidthForLetter(EIcaoCode::F) > 1.0e9);

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
	TestEqual(TEXT("a Code C stand is 55 m deep"), IcaoCode::StandDepthForLetter(EIcaoCode::C), 5500.0, 0.5);

	// AND HOW LONG AN AIRFRAME THE LETTER ADMITS, which is what a stand's ground geometry is
	// kept clear of. Code C's figure is MEASURED - it is the 737-800's tail, the longest type
	// this project ships - so it is pinned here against the same figure Build737 authors, and
	// the two cannot drift without a test saying so. It was 3430 until 2026-09-19, when
	// Build737 was found to be carrying the A320's nose overhang; correcting the nose to
	// Boeing's 13 FT 5 IN moved the tail with it, because the tail is the nose less the
	// published length. A test that pins a figure is only as good as the figure's source -
	// this one held the two in step faithfully for months while both were wrong.
	// The letters with no shipped type are
	// authored design values and are asserted only for their ORDER, which is the one thing
	// that must hold however the figures are revised.
	TestEqual(TEXT("Code C admits the 737-800's tail, and is measured from it"),
		IcaoCode::MaxTailAftForLetter(EIcaoCode::C), 3538.0, 0.5);

	double Previous = 0.0;
	for (const EIcaoCode Code : { EIcaoCode::A, EIcaoCode::B, EIcaoCode::C, EIcaoCode::D, EIcaoCode::E, EIcaoCode::F })
	{
		const double Aft = IcaoCode::MaxTailAftForLetter(Code);
		TestTrue(
			*FString::Printf(TEXT("%s admits a longer airframe than the letter below it (%.0f after %.0f)"),
				IcaoCode::ToLetter(Code), Aft, Previous),
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
	for (const EIcaoCode Code : { EIcaoCode::A, EIcaoCode::B, EIcaoCode::C, EIcaoCode::D, EIcaoCode::E, EIcaoCode::F })
	{
		const double Fwd = IcaoCode::WingFwdForLetter(Code);
		const double Aft = IcaoCode::WingAftForLetter(Code);
		const TCHAR* Letter = IcaoCode::ToLetter(Code);

		TestTrue(*FString::Printf(TEXT("%s's wing band runs aft to fwd (%.0f .. %.0f)"),
			Letter, Aft, Fwd), Aft < Fwd);
		TestTrue(*FString::Printf(TEXT("%s's wing is behind the stop mark (%.0f)"), Letter, Fwd),
			Fwd < 0.0);
		TestTrue(*FString::Printf(TEXT("%s's wing is inside its own airframe (%.0f vs %.0f)"),
			Letter, Aft, -IcaoCode::MaxTailAftForLetter(Code)),
			Aft > -IcaoCode::MaxTailAftForLetter(Code));
	}

	// CONTAINMENT, at the corners that decide it. Code C's box is x in [-2150, -950] out to
	// the span band's half, 1800.
	TestTrue(TEXT("under the wing root is inside"),
		IcaoCode::WingKeepOutContains(EIcaoCode::C, FVector2D(-1500.0, 700.0)));
	TestFalse(TEXT("forward of the leading edge is clear"),
		IcaoCode::WingKeepOutContains(EIcaoCode::C, FVector2D(-800.0, 700.0)));
	TestFalse(TEXT("aft of the trailing edge is clear"),
		IcaoCode::WingKeepOutContains(EIcaoCode::C, FVector2D(-2400.0, 700.0)));
	TestFalse(TEXT("outboard of the wingtip is clear"),
		IcaoCode::WingKeepOutContains(EIcaoCode::C, FVector2D(-1500.0, 1900.0)));

	// AND THE SEGMENT TEST, which is the one a route is judged by. The first case is the whole
	// point of clipping rather than sampling: both ENDS are clear and the middle is not.
	TestTrue(TEXT("a lane crossing under the wing is caught, though both ends are clear"),
		IcaoCode::WingKeepOutCrossedBy(EIcaoCode::C,
			FVector2D(-1500.0, -1900.0), FVector2D(-1500.0, 1900.0)));
	TestFalse(TEXT("the same crossing made aft of the trailing edge is clear"),
		IcaoCode::WingKeepOutCrossedBy(EIcaoCode::C,
			FVector2D(-2400.0, -1900.0), FVector2D(-2400.0, 1900.0)));
	TestFalse(TEXT("and forward of the leading edge is clear"),
		IcaoCode::WingKeepOutCrossedBy(EIcaoCode::C,
			FVector2D(-800.0, -1900.0), FVector2D(-800.0, 1900.0)));
	TestFalse(TEXT("a lane running along outside the tip is clear"),
		IcaoCode::WingKeepOutCrossedBy(EIcaoCode::C,
			FVector2D(-4000.0, 2450.0), FVector2D(500.0, 2450.0)));

	// A DIAGONAL FROM ONE CLEAR SIDE TO THE OTHER IS NOT CLEAR, and this is the case the
	// first draft of this test got wrong: both endpoints sit outside the box and the line
	// between them goes straight through it. Getting round the wing means going round the
	// TIP, which no single segment across the centreline can do.
	TestTrue(TEXT("a diagonal between two clear points still crosses"),
		IcaoCode::WingKeepOutCrossedBy(EIcaoCode::C,
			FVector2D(-2400.0, -1900.0), FVector2D(-800.0, 1900.0)));

	// AND CONTACT COUNTS. A lane laid exactly on the wingtip is a lane under the wingtip.
	TestTrue(TEXT("a lane laid exactly on the tip is not clear of it"),
		IcaoCode::WingKeepOutCrossedBy(EIcaoCode::C,
			FVector2D(-4000.0, 1800.0), FVector2D(500.0, 1800.0)));

	return true;
}

// FIX ROUND 1 (drawn-stands Task 4 review, finding 1): MaxWingspanForLetter is the ONE real
// "too wide" ceiling this table has, so it must agree EXACTLY with the boundary
// LetterForWingspan itself reads off Rows - a hair under a letter's own ceiling is still that
// letter, and (for every letter but F, which has nothing above it to roll into) AT the
// ceiling has already rolled over to the next one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaxWingspanForLetterMatchesTheBandEdgesTest,
	"Airside.Solve.MaxWingspanForLetterMatchesTheBandEdges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMaxWingspanForLetterMatchesTheBandEdgesTest::RunTest(const FString& Parameters)
{
	const EIcaoCode Ladder[] = { EIcaoCode::A, EIcaoCode::B, EIcaoCode::C, EIcaoCode::D, EIcaoCode::E };
	for (const EIcaoCode Code : Ladder)
	{
		const double Ceiling = IcaoCode::MaxWingspanForLetter(Code);
		const EIcaoCode Next = static_cast<EIcaoCode>(static_cast<uint8>(Code) + 1);
		TestEqual(
			*FString::Printf(TEXT("%s: a hair under its own ceiling is still %s"),
				IcaoCode::ToLetter(Code), IcaoCode::ToLetter(Code)),
			IcaoCode::LetterForWingspan(Ceiling - 1.0), FString(IcaoCode::ToLetter(Code)));
		TestEqual(
			*FString::Printf(TEXT("%s: AT its own ceiling has rolled over to %s"),
				IcaoCode::ToLetter(Code), IcaoCode::ToLetter(Next)),
			IcaoCode::LetterForWingspan(Ceiling), FString(IcaoCode::ToLetter(Next)));
	}

	// CODE F IS THE ONE ROW WITH NOTHING ABOVE IT TO ROLL INTO - both a hair under its
	// ceiling and exactly AT it still read "F", which is exactly why StandAdmits cannot use
	// LetterForWingspan's return alone to tell "genuinely F" from "wider than anything built"
	// and has to ask this function about the ceiling directly instead.
	const double FCeiling = IcaoCode::MaxWingspanForLetter(EIcaoCode::F);
	TestEqual(TEXT("F: a hair under its own ceiling is still F"),
		IcaoCode::LetterForWingspan(FCeiling - 1.0), FString(TEXT("F")));
	TestEqual(TEXT("F: AT its own ceiling is still F - nothing to roll over to"),
		IcaoCode::LetterForWingspan(FCeiling), FString(TEXT("F")));
	TestEqual(TEXT("F's own ceiling is 80 m, the one real 'too wide' threshold this table has"),
		FCeiling, 8000.0);

	return true;
}

// DRAWN-STANDS TASK 5 REVIEW, FIX ROUND 1: a drawn stand's captured DesignWingspan must read
// back through LetterForWingspan as the LETTER IT WAS DRAWN TO, not the one above it - see
// DesignSpanForLetter's own header for why MaxWingspanForLetter's ceiling itself cannot be
// used for this directly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDesignSpanForLetterReadsBackAsItsOwnLetterTest,
	"Airside.Solve.DesignSpanForLetterReadsBackAsItsOwnLetter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDesignSpanForLetterReadsBackAsItsOwnLetterTest::RunTest(const FString& Parameters)
{
	const EIcaoCode Ladder[] = {
		EIcaoCode::A, EIcaoCode::B, EIcaoCode::C, EIcaoCode::D, EIcaoCode::E, EIcaoCode::F };
	for (const EIcaoCode Code : Ladder)
	{
		const double Span = IcaoCode::DesignSpanForLetter(Code);
		TestEqual(
			*FString::Printf(TEXT("%s: DesignSpanForLetter reads back as %s"),
				IcaoCode::ToLetter(Code), IcaoCode::ToLetter(Code)),
			IcaoCode::LetterForWingspan(Span), FString(IcaoCode::ToLetter(Code)));
		TestTrue(
			*FString::Printf(TEXT("%s: a stand designed for its own span admits an aircraft of "
				"exactly that span"), IcaoCode::ToLetter(Code)),
			IcaoCode::StandAdmits(Span, Span));
	}

	return true;
}

// FIX ROUND 1, finding 2: ONE admission rule for ArrivalPlanner::ChooseStand and
// UStandAllocator::Reserve, which used to each compare Stand.DesignWingspan against an
// aircraft's Wingspan as raw doubles and had started to disagree about a legacy-span stand.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandAdmitsComparesLettersTest,
	"Airside.Solve.StandAdmitsComparesLetters",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandAdmitsComparesLettersTest::RunTest(const FString& Parameters)
{
	// THE LEGACY CASE this rule exists for: a stand's captured span (3410, an older A320
	// measurement) and a 737-800's published span (3580) are BOTH Code C - LetterForWingspan(
	// 3410) and LetterForWingspan(3580) agree - so the stand admits the aircraft even though
	// 3410 < 3580 as raw numbers, which a raw-double compare would have refused.
	TestTrue(TEXT("a legacy-span stand still admits by letter, not by raw span"),
		IcaoCode::StandAdmits(3410.0, 3580.0));

	TestFalse(TEXT("a stand too small by letter is refused"),
		IcaoCode::StandAdmits(2000.0, 3580.0));

	// "Smallest that fits" is StandRank's question, not StandAdmits' - a bigger-lettered
	// stand than strictly needed still admits.
	TestTrue(TEXT("a bigger stand than needed still admits"),
		IcaoCode::StandAdmits(6000.0, 3580.0));

	// UNKNOWN, EITHER SIDE, ADMITS ANYTHING - the pre-letter-admission behaviour every caller
	// saw before this rule existed, preserved so no existing fixture's stands or airframes
	// (every one of them built with DesignWingspan/Wingspan left at its 0.0 default) changed
	// meaning under it.
	TestTrue(TEXT("an unmeasured stand admits anything"), IcaoCode::StandAdmits(0.0, 9000.0));
	TestTrue(TEXT("an airframe with no known span is admitted anywhere"),
		IcaoCode::StandAdmits(2000.0, 0.0));

	// WIDER THAN EVERY LETTER ADMITS IS NEVER ADMITTED - not even by a genuine Code F stand,
	// which is the one real refusal MaxWingspanForLetter(F) exists to make possible.
	const double FCeiling = IcaoCode::MaxWingspanForLetter(EIcaoCode::F);
	TestTrue(TEXT("a genuine Code F stand admits a genuine Code F aircraft"),
		IcaoCode::StandAdmits(FCeiling, FCeiling - 1.0));
	TestFalse(TEXT("nothing admits an aircraft wider than Code F allows, even a Code F stand"),
		IcaoCode::StandAdmits(FCeiling, FCeiling + 1000.0));

	// RANK: A=0 .. F=5, unknown ranks as C - the legacy default IcaoCode's own
	// RadiusForLetter fallback used before Parse existed.
	TestEqual(TEXT("Code A ranks 0"), IcaoCode::StandRank(1000.0), static_cast<int32>(EIcaoCode::A));
	TestEqual(TEXT("Code F ranks 5"), IcaoCode::StandRank(FCeiling - 1.0), static_cast<int32>(EIcaoCode::F));
	TestEqual(TEXT("unknown ranks as C, the legacy default"),
		IcaoCode::StandRank(0.0), static_cast<int32>(EIcaoCode::C));

	return true;
}

#endif
