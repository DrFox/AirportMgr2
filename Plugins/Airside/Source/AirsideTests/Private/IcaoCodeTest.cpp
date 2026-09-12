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

#endif
