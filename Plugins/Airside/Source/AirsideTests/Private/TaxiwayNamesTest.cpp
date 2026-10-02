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
		/** Lay, then normalise - one click of the road tool, which commits one segment and notifies once. */
		FRoadSegmentId Click(FRoadNodeId A, FRoadNodeId B) { const FRoadSegmentId S = Lay(A, B); Normalise(); return S; }
		TArray<FTaxiwayRename> Normalise() { return Net->NormaliseTaxiways(Rules); }
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

/** Assign: a taxiway is named the next free letter; roads and runways are not named. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesAssignTest, "Airside.Model.TaxiwayNames.Assign",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesAssignTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	const FRoadSegmentId A = N.Lay(N.Node(0.0, 0.0), N.Node(50000.0, 0.0));
	TestEqual(TEXT("unnamed until normalised"), N.NameOf(A), FString());
	TestEqual(TEXT("a first taxiway splits nothing"), N.Normalise().Num(), 0);
	TestEqual(TEXT("the first taxiway is A"), N.NameOf(A), FString(TEXT("A")));
	const FRoadSegmentId B = N.Click(N.Node(0.0, 100000.0), N.Node(50000.0, 100000.0));
	TestEqual(TEXT("an unconnected 500 m taxiway is the next letter"), N.NameOf(B), FString(TEXT("B")));
	const FRoadSegmentId Road = N.Lay(N.Node(0.0, 200000.0), N.Node(50000.0, 200000.0), N.Road);
	const FRoadSegmentId Strip = N.Lay(N.Node(0.0, 300000.0), N.Node(50000.0, 300000.0), N.Runway);
	N.Normalise();
	TestEqual(TEXT("a service road is unnamed"), N.Net->TaxiwayOf(Road), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("a runway keeps its designator, not a letter"), N.Net->TaxiwayOf(Strip), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("two taxiways and nothing else"), N.Alive(), 2);
	TestEqual(TEXT("a second normalise of the same network changes nothing"), N.Normalise().Num(), 0);
	TestEqual(TEXT("and mints nothing"), N.Net->GetTaxiways().Num(), 2);
	return true;
}

/**
 * REVIEW FOCUS 1 - CLICK BY CLICK (RoadDrawTool commits one segment per click): carrying on from the END of a taxiway
 * inherits it, through a bend within FTaxiwayNamingRules::BendDegrees (plan D1); a 90 degree corner starts a new name;
 * at a JUNCTION only RoadGeom::InLineDegrees carries on.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesClickByClickTest, "Airside.Model.TaxiwayNames.ClickByClickDrawing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesClickByClickTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	const FRoadNodeId P0 = N.Node(0.0, 0.0);
	const FRoadNodeId P1 = N.Node(40000.0, 0.0);
	const FRoadNodeId P2 = N.Node(80000.0, 0.0);
	const FVector2D Bend(FMath::Cos(FMath::DegreesToRadians(30.0)), FMath::Sin(FMath::DegreesToRadians(30.0)));
	const FVector2D At3 = FVector2D(80000.0, 0.0) + Bend * 40000.0;
	const FRoadNodeId P3 = N.Node(At3.X, At3.Y);
	const FVector2D Corner(-Bend.Y, Bend.X);   // 90 degrees from the bend's direction
	const FVector2D At4 = At3 + Corner * 40000.0;
	const FRoadNodeId P4 = N.Node(At4.X, At4.Y);

	const FRoadSegmentId S1 = N.Click(P0, P1);
	const FRoadSegmentId S2 = N.Click(P1, P2);
	TestEqual(TEXT("in line from A's end: A"), N.NameOf(S2), FString(TEXT("A")));
	const FRoadSegmentId S3 = N.Click(P2, P3);
	TestEqual(TEXT("through a 30 degree bend: still A"), N.NameOf(S3), FString(TEXT("A")));
	const FRoadSegmentId S4 = N.Click(P3, P4);
	TestEqual(TEXT("round a 90 degree corner: a new letter"), N.NameOf(S4), FString(TEXT("B")));
	TestEqual(TEXT("the first click's segment is still A"), N.NameOf(S1), FString(TEXT("A")));
	TestEqual(TEXT("four clicks, two taxiways"), N.Alive(), 2);

	// AT A JUNCTION: the first taxiway runs north-south through Q; D ends at Q from the west. F leaves Q 30 degrees off
	// D's line - within BendDegrees but NOT within the junction's 10 - so it is new; E then carries straight on from D.
	FTaxiwayNamesNet J;
	const FRoadNodeId Q = J.Node(200000.0, 0.0);
	J.Click(J.Node(200000.0, -50000.0), Q);
	const FRoadSegmentId C = J.Click(Q, J.Node(200000.0, 50000.0));
	const FRoadSegmentId D = J.Click(J.Node(150000.0, 0.0), Q);
	TestEqual(TEXT("the north-south one is A"), J.NameOf(C), FString(TEXT("A")));
	TestEqual(TEXT("D meets A's middle: a new letter"), J.NameOf(D), FString(TEXT("B")));
	const FVector2D Off = FVector2D(200000.0, 0.0) + FVector2D(FMath::Cos(FMath::DegreesToRadians(30.0)),
		FMath::Sin(FMath::DegreesToRadians(30.0))) * 50000.0;
	const FRoadSegmentId F = J.Click(Q, J.Node(Off.X, Off.Y));
	TestEqual(TEXT("30 degrees off B's line at a junction is NOT in line (10 deg there): a new letter"), J.NameOf(F), FString(TEXT("C")));
	const FRoadSegmentId E = J.Click(Q, J.Node(250000.0, 0.0));
	TestEqual(TEXT("straight on through the junction from B's end: B"), J.NameOf(E), FString(TEXT("B")));
	return true;
}

/** Connector: a short chain anchored at both ends is its parent's next number; leaving a runway, the taxiway is the parent. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesConnectorTest, "Airside.Model.TaxiwayNames.Connector",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesConnectorTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	const FRoadNodeId N0 = N.Node(0.0, 0.0);
	const FRoadNodeId N1 = N.Node(50000.0, 0.0);
	const FRoadNodeId N2 = N.Node(100000.0, 0.0);
	const FRoadSegmentId A = N.Click(N0, N1);
	N.Click(N1, N2);
	const FRoadNodeId R0 = N.Node(0.0, 25000.0);
	const FRoadNodeId R1 = N.Node(50000.0, 25000.0);
	const FRoadNodeId R2 = N.Node(100000.0, 25000.0);
	N.Lay(R0, R1, N.Runway);
	N.Lay(R1, R2, N.Runway);
	N.Normalise();

	const FRoadSegmentId Stub = N.Click(N1, N.Node(50000.0, -20000.0));
	TestEqual(TEXT("a 200 m stub off A to a dead end is A's first connector (plan D2)"), N.NameOf(Stub), FString(TEXT("A1")));
	const FRoadSegmentId Link = N.Click(R2, N2);
	TestEqual(TEXT("a 250 m link FROM the runway TO A is A's: the taxiway is the parent"), N.NameOf(Link), FString(TEXT("A2")));
	const FRoadSegmentId Long = N.Click(R1, N.Node(50000.0, -60000.0));
	TestEqual(TEXT("an 850 m link is no connector: a letter"), N.NameOf(Long), FString(TEXT("B")));
	TestEqual(TEXT("A lists its two connectors"), N.Net->TaxiwayConnectorCount(N.Net->TaxiwayOf(A)), 2);
	const FTaxiway* Connector = N.Net->GetTaxiway(N.Net->TaxiwayOf(Stub));
	if (TestNotNull(TEXT("the stub's taxiway"), Connector))
	{
		TestTrue(TEXT("derived, not stored: its own Name is empty"), Connector->Name.IsEmpty());
		TestEqual(TEXT("parented on A"), Connector->ParentId, N.Net->TaxiwayOf(A));
	}
	return true;
}

/** Insert a node / split a segment: both halves keep the taxiway, before any normalise. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesSplitTest, "Airside.Model.TaxiwayNames.SplitKeepsTheTaxiway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesSplitTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	const FRoadSegmentId S = N.Click(N.Node(0.0, 0.0), N.Node(100000.0, 0.0));
	const int32 Was = N.Net->TaxiwayOf(S);
	const FRoadNodeId Mid = N.Net->SplitSegment(S, FVector2D(50000.0, 0.0));
	const FRoadNode* MidNode = N.Net->GetNode(Mid);
	if (!TestNotNull(TEXT("the split made a node"), MidNode)) { return false; }
	const TArray<FRoadSegmentId> Halves = MidNode->Incident;
	TestEqual(TEXT("two halves"), Halves.Num(), 2);
	for (const FRoadSegmentId& Half : Halves)
	{
		TestEqual(TEXT("the split itself carries the taxiway to each half"), N.Net->TaxiwayOf(Half), Was);
	}
	TestEqual(TEXT("so normalising renames nothing"), N.Normalise().Num(), 0);
	TestEqual(TEXT("and it is still one taxiway"), N.Alive(), 1);
	return true;
}

/** Two taxiways meeting in line each keep their own; the junction reads "A/B". */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesInLineMeetTest, "Airside.Model.TaxiwayNames.InLineMeetingKeepsBoth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesInLineMeetTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	const FRoadNodeId A1 = N.Node(50000.0, 0.0);
	const FRoadSegmentId A = N.Click(N.Node(0.0, 0.0), A1);
	const FRoadNodeId B0 = N.Node(60000.0, 0.0);
	const FRoadSegmentId B = N.Click(B0, N.Node(110000.0, 0.0));
	TestTrue(TEXT("merged end to end"), N.Net->MergeNodes(A1, B0));
	TestEqual(TEXT("meeting in line renames nothing"), N.Normalise().Num(), 0);
	TestEqual(TEXT("A is A"), N.NameOf(A), FString(TEXT("A")));
	TestEqual(TEXT("B is B"), N.NameOf(B), FString(TEXT("B")));
	TestEqual(TEXT("the junction is named by both"), N.Net->JunctionName(A1), FString(TEXT("A/B")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
