#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/TaxiwayStrip.h"
#include "Profiles/RoadProfile.h"
#include "Solve/TaxiwayLetters.h"
#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * TAXIWAY NAMES, world-free (spec docs/superpowers/specs/2026-10-02-taxiway-naming-design.md): one test per rule of the
 * assignment table and the single-chain invariant. Every mutator here is URoadNetwork's raw one, so each test calls
 * NormaliseTaxiways itself where the facade's NotifyChanged would (plan D9).
 */
namespace TaxiwayNamesTest
{
	/** Prefixed against the UNITY build. A transient network and the three kinds of road. */
	struct FTaxiwayNamesNet
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Taxi = TestProfiles::Taxiway();
		URoadProfile* Runway = TestProfiles::Runway();
		URoadProfile* Road = URoadProfile::MakeServiceRoadTransient();
		FTaxiwayNamingRules Rules;

		FRoadNodeId Node(double X, double Y) { return Net->AddNode(FVector2D(X, Y)); }
		FRoadSegmentId Lay(FRoadNodeId A, FRoadNodeId B, URoadProfile* Profile = nullptr)
		{
			return Net->AddStraightSegment(A, B, Profile != nullptr ? Profile : Taxi);
		}
		FString NameOf(FRoadSegmentId S) const { return Net->TaxiwayDisplayName(Net->TaxiwayOf(S)); }
		int32 Alive() const
		{
			int32 Count = 0;
			for (const FTaxiway& T : Net->GetTaxiways()) { Count += T.bAlive ? 1 : 0; }
			return Count;
		}
	};
}

/** I, O and X read as 1, 0 and a closed runway, so they are never issued; after Z come two letters (spec). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesLetterSequenceTest, "Airside.Model.TaxiwayNames.LetterSequence",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesLetterSequenceTest::RunTest(const FString&)
{
	TestEqual(TEXT("the first letter is A"), TaxiwayLetters::LetterAt(0), FString(TEXT("A")));
	TestEqual(TEXT("H is followed by J - I reads as 1"), TaxiwayLetters::LetterAt(8), FString(TEXT("J")));
	TestEqual(TEXT("N is followed by P - O reads as 0"), TaxiwayLetters::LetterAt(13), FString(TEXT("P")));
	TestEqual(TEXT("W is followed by Y - X is a closed runway"), TaxiwayLetters::LetterAt(21), FString(TEXT("Y")));
	TestEqual(TEXT("Z is the last single letter"), TaxiwayLetters::LetterAt(22), FString(TEXT("Z")));
	TestEqual(TEXT("then AA"), TaxiwayLetters::LetterAt(23), FString(TEXT("AA")));
	TestEqual(TEXT("then AB"), TaxiwayLetters::LetterAt(24), FString(TEXT("AB")));
	TestEqual(TEXT("AH is followed by AJ"), TaxiwayLetters::LetterAt(31), FString(TEXT("AJ")));
	TestEqual(TEXT("AZ is followed by BA"), TaxiwayLetters::LetterAt(46), FString(TEXT("BA")));
	TSet<FString> Seen;
	for (int32 Index = 0; Index < 600; ++Index)
	{
		const FString Letter = TaxiwayLetters::LetterAt(Index);
		if (!TestFalse(FString::Printf(TEXT("%s avoids I, O and X"), *Letter),
			Letter.Contains(TEXT("I")) || Letter.Contains(TEXT("O")) || Letter.Contains(TEXT("X")))) { return false; }
		bool bAlready = false;
		Seen.Add(Letter, &bAlready);
		if (!TestFalse(FString::Printf(TEXT("%s is issued once"), *Letter), bAlready)) { return false; }
	}
	TestTrue(TEXT("I, O and X are the avoided ones"), TaxiwayLetters::IsAvoided(TEXT('I')) && TaxiwayLetters::IsAvoided(TEXT('O'))
		&& TaxiwayLetters::IsAvoided(TEXT('X')) && !TaxiwayLetters::IsAvoided(TEXT('A')) && !TaxiwayLetters::IsAvoided(TEXT('1')));
	return true;
}

/** A network with no names reads as one: nothing names a segment, and an unknown id has no name. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesUnnamedTest, "Airside.Model.TaxiwayNames.UnnamedByDefault",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesUnnamedTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	const FRoadSegmentId S = N.Lay(N.Node(0.0, 0.0), N.Node(50000.0, 0.0));
	TestTrue(TEXT("control: the test profile is a taxiway"), TaxiwayStrip::HasStrip(*N.Net, S));
	TestEqual(TEXT("no taxiway yet"), N.Net->GetTaxiways().Num(), 0);
	TestEqual(TEXT("the segment names none"), N.Net->TaxiwayOf(S), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("an unknown id has no display name"), N.Net->TaxiwayDisplayName(INDEX_NONE), FString());
	TestEqual(TEXT("nor does an id past the end"), N.Net->TaxiwayDisplayName(7), FString());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
