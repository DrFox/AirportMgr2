#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Math/RandomStream.h"
#include "Model/GroundTraffic.h"
#include "Model/InspectFacts.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/RouteSearch.h"
#include "Model/TaxiwayLabels.h"
#include "Model/TaxiwayStrip.h"
#include "Profiles/RoadProfile.h"
#include "Solve/TaxiwayLetters.h"
#include "Testing/AirsideTestGraph.h"
#include "Testing/AirsideTestWorld.h"

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

/** Disconnected: the longer piece keeps the taxiway, the other gets the next free letter, and says so once. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesDisconnectTest, "Airside.Model.TaxiwayNames.DisconnectedPieceSplitsOff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesDisconnectTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	const FRoadNodeId N0 = N.Node(0.0, 0.0);
	const FRoadNodeId N1 = N.Node(40000.0, 0.0);
	const FRoadNodeId N2 = N.Node(50000.0, 0.0);
	const FRoadNodeId N3 = N.Node(70000.0, 0.0);
	const FRoadSegmentId Long = N.Click(N0, N1);
	const FRoadSegmentId Middle = N.Click(N1, N2);
	const FRoadSegmentId Short = N.Click(N2, N3);
	TestEqual(TEXT("setup: one taxiway A"), N.NameOf(Short), FString(TEXT("A")));
	TestTrue(TEXT("the middle goes"), N.Net->RemoveSegment(Middle));
	const TArray<FTaxiwayRename> Renames = N.Normalise();
	if (!TestEqual(TEXT("one split, one report"), Renames.Num(), 1)) { return false; }
	TestEqual(TEXT("B split off"), Renames[0].SplitOff, FString(TEXT("B")));
	TestEqual(TEXT("from A"), Renames[0].From, FString(TEXT("A")));
	TestEqual(TEXT("the 400 m piece keeps A"), N.NameOf(Long), FString(TEXT("A")));
	TestEqual(TEXT("the 200 m piece is B"), N.NameOf(Short), FString(TEXT("B")));
	return true;
}

/** Branch: A's end merged onto A's middle - the shortest branch at the node splits off as the next letter. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesBranchTest, "Airside.Model.TaxiwayNames.BranchSplitsOff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesBranchTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	TArray<FRoadNodeId> P;
	for (int32 Index = 0; Index <= 4; ++Index) { P.Add(N.Node(30000.0 * Index, 0.0)); }
	const FRoadSegmentId Tail = N.Click(P[0], P[1]);
	const FRoadSegmentId Loop = N.Click(P[1], P[2]);
	N.Click(P[2], P[3]);
	N.Click(P[3], P[4]);
	TestEqual(TEXT("setup: one taxiway"), N.Alive(), 1);
	TestTrue(TEXT("A's far end merged onto its second node"), N.Net->MergeNodes(P[1], P[4]));
	const TArray<FTaxiwayRename> Renames = N.Normalise();
	if (!TestEqual(TEXT("one branch split off"), Renames.Num(), 1)) { return false; }
	TestEqual(TEXT("the 300 m tail is the shortest branch: B"), N.NameOf(Tail), FString(TEXT("B")));
	TestEqual(TEXT("the loop keeps A"), N.NameOf(Loop), FString(TEXT("A")));
	return true;
}

/** Empty: a parent with surviving connectors keeps its letter reserved; with none it is retired and A returns to the pool. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesReservationTest, "Airside.Model.TaxiwayNames.CollapseAndLetterReservation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesReservationTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	const FRoadNodeId A0 = N.Node(0.0, 0.0);
	const FRoadNodeId A1 = N.Node(50000.0, 0.0);
	const FRoadNodeId A2 = N.Node(100000.0, 0.0);
	const FRoadSegmentId First = N.Click(A0, A1);
	const FRoadSegmentId Second = N.Click(A1, A2);
	const FRoadSegmentId Stub = N.Click(A1, N.Node(50000.0, -20000.0));
	const int32 AId = N.Net->TaxiwayOf(First);
	TestEqual(TEXT("setup: A1"), N.NameOf(Stub), FString(TEXT("A1")));
	N.Net->RemoveSegment(First);
	N.Net->RemoveSegment(Second);
	N.Normalise();
	TestNotNull(TEXT("A is kept with no segment: A1 still reads from it"), N.Net->GetTaxiway(AId));
	TestEqual(TEXT("A1 still reads A1"), N.NameOf(Stub), FString(TEXT("A1")));
	const FRoadSegmentId Fresh = N.Click(N.Node(0.0, 200000.0), N.Node(60000.0, 200000.0));
	TestEqual(TEXT("a new taxiway is B - A is reserved, or a second A1 could be minted"), N.NameOf(Fresh), FString(TEXT("B")));
	N.Net->RemoveSegment(Stub);
	N.Normalise();
	TestNull(TEXT("A goes with its last connector"), N.Net->GetTaxiway(AId));
	const FRoadSegmentId Again = N.Click(N.Node(0.0, 300000.0), N.Node(60000.0, 300000.0));
	TestEqual(TEXT("and A is back in the pool"), N.NameOf(Again), FString(TEXT("A")));
	return true;
}

/** A connector number is never reissued while its parent lives. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesNumbersTest, "Airside.Model.TaxiwayNames.ConnectorNumbersNeverReused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesNumbersTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	TArray<FRoadNodeId> A;
	for (int32 Index = 0; Index <= 3; ++Index) { A.Add(N.Node(50000.0 * Index, 0.0)); }
	for (int32 Index = 0; Index < 3; ++Index) { N.Click(A[Index], A[Index + 1]); }
	const FRoadSegmentId One = N.Click(A[1], N.Node(50000.0, -20000.0));
	const FRoadSegmentId Two = N.Click(A[2], N.Node(100000.0, -20000.0));
	TestEqual(TEXT("A1"), N.NameOf(One), FString(TEXT("A1")));
	TestEqual(TEXT("A2"), N.NameOf(Two), FString(TEXT("A2")));
	N.Net->RemoveSegment(One);
	N.Normalise();
	const FRoadSegmentId Three = N.Click(A[1], N.Node(50000.0, 20000.0));
	TestEqual(TEXT("the next is A3, never the retired A1"), N.NameOf(Three), FString(TEXT("A3")));
	return true;
}

/** REVIEW FOCUS 2 - a gesture is many commits: names are judged "as drawn so far" (plan D3). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesRejudgeTest, "Airside.Model.TaxiwayNames.DrawnSoFarIsRejudged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesRejudgeTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	{
		// A TWO-CLICK CONNECTOR: a stub off A's middle, then on to the runway 250 m away, in line.
		FTaxiwayNamesNet N;
		const FRoadNodeId Mid = N.Node(50000.0, 0.0);
		N.Click(N.Node(0.0, 0.0), Mid);
		N.Click(Mid, N.Node(100000.0, 0.0));
		const FRoadNodeId R = N.Node(50000.0, 25000.0);
		N.Lay(N.Node(0.0, 25000.0), R, N.Runway);
		N.Lay(R, N.Node(100000.0, 25000.0), N.Runway);
		N.Normalise();
		const FRoadNodeId Free = N.Node(50000.0, 15000.0);
		const FRoadSegmentId Click1 = N.Click(Mid, Free);
		const FRoadSegmentId Click2 = N.Click(Free, R);
		TestEqual(TEXT("click 1 is A1"), N.NameOf(Click1), FString(TEXT("A1")));
		TestEqual(TEXT("click 2 carries A1 on to the runway"), N.NameOf(Click2), FString(TEXT("A1")));
	}
	{
		// A STUB GROWN LONG: 200 m off A (A1), then straight on to 600 m - no connector any more.
		FTaxiwayNamesNet N;
		const FRoadNodeId Mid = N.Node(50000.0, 0.0);
		N.Click(N.Node(0.0, 0.0), Mid);
		N.Click(Mid, N.Node(100000.0, 0.0));
		const FRoadNodeId Free = N.Node(50000.0, -20000.0);
		const FRoadSegmentId Stub = N.Click(Mid, Free);
		TestEqual(TEXT("setup: A1"), N.NameOf(Stub), FString(TEXT("A1")));
		const FRoadSegmentId On = N.Lay(Free, N.Node(50000.0, -60000.0));
		const TArray<FTaxiwayRename> Renames = N.Normalise();
		TestEqual(TEXT("600 m is a letter now: B"), N.NameOf(On), FString(TEXT("B")));
		TestEqual(TEXT("the stub went with it"), N.NameOf(Stub), FString(TEXT("B")));
		TestEqual(TEXT("re-judging is not a split: no toast"), Renames.Num(), 0);
	}
	{
		// A LETTER FROM OPEN GROUND THAT LANDS ON A: 100 m alone is B (no taxiway at either end), and once it reaches
		// A's end, 250 m in all, it is A's connector - and B goes back in the pool.
		FTaxiwayNamesNet N;
		const FRoadNodeId End = N.Node(200000.0, 0.0);
		N.Click(N.Node(100000.0, 0.0), End);
		const FRoadNodeId Free = N.Node(200000.0, -15000.0);
		const FRoadSegmentId Click1 = N.Click(N.Node(200000.0, -25000.0), Free);
		TestEqual(TEXT("click 1 has no taxiway at either end: B"), N.NameOf(Click1), FString(TEXT("B")));
		N.Click(Free, End);
		TestEqual(TEXT("it reached A: A1"), N.NameOf(Click1), FString(TEXT("A1")));
		const FRoadSegmentId Next = N.Click(N.Node(0.0, 100000.0), N.Node(60000.0, 100000.0));
		TestEqual(TEXT("B is free again"), N.NameOf(Next), FString(TEXT("B")));
	}
	return true;
}

namespace TaxiwayNamesTest
{
	/** THE CONTRACT, measured (spec "Invariant test"): every live taxiway is ONE connected chain with at most 2 of its
	 *  segments at any node, display names are unique and non-empty, an empty one is kept only for its connectors, every
	 *  taxiway segment is named and nothing else is. Empty string when it holds; the first breach otherwise. */
	FString InvariantViolation(const URoadNetwork& Net)
	{
		TMap<FString, int32> Shown;
		for (const FTaxiway& T : Net.GetTaxiways())
		{
			if (!T.bAlive) { continue; }
			const FString Name = Net.TaxiwayDisplayName(T.Id);
			if (Name.IsEmpty()) { return FString::Printf(TEXT("taxiway %d has no display name"), T.Id); }
			if (const int32* Other = Shown.Find(Name)) { return FString::Printf(TEXT("%s is shown by %d and %d"), *Name, *Other, T.Id); }
			Shown.Add(Name, T.Id);
			TArray<FRoadSegmentId> Own;
			for (int32 Index = 0; Index < Net.GetSegments().Num(); ++Index)
			{
				const FRoadSegmentId Id = Net.SegmentIdAt(Index);
				if (Id.IsSet() && Net.TaxiwayOf(Id) == T.Id) { Own.Add(Id); }
			}
			if (Own.Num() == 0)
			{
				if (!Net.HasTaxiwayConnectors(T.Id)) { return FString::Printf(TEXT("%s is empty with no connector, still alive"), *Name); }
				continue;
			}
			TMap<int32, int32> PerNode;
			for (const FRoadSegmentId& Id : Own)
			{
				const FRoadSegment* S = Net.GetSegment(Id);
				for (const int32 Node : { S->A.Index, S->B.Index })
				{
					if (++PerNode.FindOrAdd(Node) > 2) { return FString::Printf(TEXT("%s has 3+ segments at node %d"), *Name, Node); }
				}
			}
			TSet<FRoadSegmentId> Reached = { Own[0] };
			for (bool bGrew = true; bGrew;)
			{
				bGrew = false;
				for (const FRoadSegmentId& Id : Own)
				{
					if (Reached.Contains(Id)) { continue; }
					const FRoadSegment* S = Net.GetSegment(Id);
					for (const FRoadSegmentId& R : Reached)
					{
						const FRoadSegment* Q = Net.GetSegment(R);
						if (S->A == Q->A || S->A == Q->B || S->B == Q->A || S->B == Q->B) { Reached.Add(Id); bGrew = true; break; }
					}
				}
			}
			if (Reached.Num() != Own.Num()) { return FString::Printf(TEXT("%s is in pieces (%d of %d reached)"), *Name, Reached.Num(), Own.Num()); }
		}
		for (int32 Index = 0; Index < Net.GetSegments().Num(); ++Index)
		{
			const FRoadSegmentId Id = Net.SegmentIdAt(Index);
			if (!Id.IsSet()) { continue; }
			const bool bTaxiway = TaxiwayStrip::HasStrip(Net, Id);
			if (bTaxiway && Net.TaxiwayOf(Id) == INDEX_NONE) { return FString::Printf(TEXT("taxiway segment %d is unnamed"), Index); }
			if (!bTaxiway && Net.GetSegment(Id)->TaxiwayId != INDEX_NONE) { return FString::Printf(TEXT("segment %d is no taxiway but is named"), Index); }
		}
		return FString();
	}

	/** Everything a normalise may write: every taxiway entry and every segment's TaxiwayId, as one comparable string. */
	FString NamingSnapshot(const URoadNetwork& Net)
	{
		FString Out;
		for (const FTaxiway& T : Net.GetTaxiways())
		{
			Out += FString::Printf(TEXT("[%d %d '%s' p%d c%d n%d %d] "), T.Id, T.bAlive ? 1 : 0, *T.Name, T.ParentId,
				T.ConnectorNumber, T.NextConnectorNumber, T.bPlayerNamed ? 1 : 0);
		}
		for (const FRoadSegment& S : Net.GetSegments()) { Out += FString::Printf(TEXT("%d,"), S.TaxiwayId); }
		return Out;
	}

	/** One seeded run: 300 random raw edits, normalised after each. Fills the per-slot names for the determinism check.
	 *  bCheckIdempotent normalises a SECOND time after each edit and reports any write or split it makes. */
	FString RandomEditRun(int32 Seed, TArray<FString>& OutNames, bool bCheckIdempotent = false)
	{
		FTaxiwayNamesNet N;
		FRandomStream Stream(Seed);
		for (int32 X = 0; X < 6; ++X)
		{
			for (int32 Y = 0; Y < 6; ++Y) { N.Node(X * 20000.0, Y * 20000.0); }
		}
		const auto AnyNode = [&N, &Stream]() { return N.Net->NodeIdAt(Stream.RandRange(0, N.Net->GetNodes().Num() - 1)); };
		const auto AnySegment = [&N, &Stream]()
		{
			const int32 Count = N.Net->GetSegments().Num();
			return Count == 0 ? FRoadSegmentId() : N.Net->SegmentIdAt(Stream.RandRange(0, Count - 1));
		};
		for (int32 Step = 0; Step < 300; ++Step)
		{
			const int32 Op = Stream.RandRange(0, 9);
			if (Op <= 4)
			{
				URoadProfile* Profile = Op == 4 ? N.Road : (Op == 3 && Stream.FRand() < 0.3f ? N.Runway : N.Taxi);
				const FRoadNodeId A = AnyNode();
				const FRoadNodeId B = AnyNode();
				if (A.IsSet() && B.IsSet() && A != B) { N.Lay(A, B, Profile); }
			}
			else if (Op == 5)
			{
				FVector2D A, B;
				const FRoadSegmentId S = AnySegment();
				if (S.IsSet() && N.Net->SegmentEnds(S, A, B)) { N.Net->SplitSegment(S, (A + B) * 0.5); }
			}
			else if (Op <= 7)
			{
				const FRoadSegmentId S = AnySegment();
				if (S.IsSet()) { N.Net->RemoveSegment(S); }
			}
			else if (Op == 8)
			{
				const FRoadNodeId A = AnyNode();
				const FRoadNodeId B = AnyNode();
				if (A.IsSet() && B.IsSet()) { N.Net->MergeNodes(A, B); }
			}
			else
			{
				const FRoadNodeId A = AnyNode();
				if (A.IsSet()) { N.Net->RemoveNode(A); }
			}
			N.Normalise();
			if (bCheckIdempotent)
			{
				const FString Before = NamingSnapshot(*N.Net);
				const int32 Splits = N.Normalise().Num();
				if (Splits != 0 || NamingSnapshot(*N.Net) != Before)
				{
					return FString::Printf(TEXT("seed %d step %d op %d: a second normalise announced %d split(s) / changed %s -> %s"),
						Seed, Step, Op, Splits, *Before, *NamingSnapshot(*N.Net));
				}
			}
			const FString Violation = InvariantViolation(*N.Net);
			if (!Violation.IsEmpty()) { return FString::Printf(TEXT("seed %d step %d op %d: %s"), Seed, Step, Op, *Violation); }
		}
		for (int32 Index = 0; Index < N.Net->GetSegments().Num(); ++Index)
		{
			const FRoadSegmentId Id = N.Net->SegmentIdAt(Index);
			OutNames.Add(Id.IsSet() ? N.NameOf(Id) : FString(TEXT("-")));
		}
		return FString();
	}
}

/** THE INVARIANT TEST (spec): a seeded random edit sequence, the contract measured after every mutator - and the same
 *  seed twice gives the same names (spec: "same network in, same names out"). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesInvariantTest, "Airside.Model.TaxiwayNames.InvariantUnderRandomEdits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesInvariantTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	{
		// CONTROL: the checker sees a breach - a taxiway left unnamed.
		FTaxiwayNamesNet N;
		N.Lay(N.Node(0.0, 0.0), N.Node(50000.0, 0.0));
		TestFalse(TEXT("control: an unnormalised taxiway is a breach the checker reports"), InvariantViolation(*N.Net).IsEmpty());
	}
	for (int32 Seed = 1; Seed <= 6; ++Seed)
	{
		TArray<FString> First;
		TArray<FString> Second;
		const FString Violation = RandomEditRun(Seed, First);
		if (!TestTrue(FString::Printf(TEXT("the contract holds after every edit (%s)"), *Violation), Violation.IsEmpty())) { return false; }
		RandomEditRun(Seed, Second);
		TestEqual(FString::Printf(TEXT("seed %d: same edits, same names"), Seed), First, Second);
	}
	return true;
}

namespace TaxiwayNamesTest
{
	/** A GATWICK-SHAPED airport (M_ScaleGatwick's pattern, 2026-10-02): a runway, two 3 km parallels A (y -200 m) and B
	 *  (y -400 m), three 200 m exits runway->A, two 200 m end links A->B round 90 degree corners, two 150 m stand stubs
	 *  off B ending at stands. Laid in that order, so segment indices are fixed. */
	struct FGatwickShape
	{
		FTaxiwayNamesNet N;
		TArray<FRoadSegmentId> ASegs, BSegs, Exits, EndLinks, Stubs;

		FGatwickShape()
		{
			TArray<FRoadNodeId> R, A, B;
			for (const double X : { 0.0, 60000.0, 150000.0, 240000.0, 300000.0 }) { R.Add(N.Node(X, 0.0)); }
			for (int32 I = 0; I + 1 < R.Num(); ++I) { N.Lay(R[I], R[I + 1], N.Runway); }
			for (const double X : { 0.0, 60000.0, 100000.0, 150000.0, 200000.0, 240000.0, 300000.0 }) { A.Add(N.Node(X, -20000.0)); }
			for (int32 I = 0; I + 1 < A.Num(); ++I) { ASegs.Add(N.Lay(A[I], A[I + 1])); }
			for (const double X : { 0.0, 50000.0, 100000.0, 150000.0, 200000.0, 250000.0, 300000.0 }) { B.Add(N.Node(X, -40000.0)); }
			for (int32 I = 0; I + 1 < B.Num(); ++I) { BSegs.Add(N.Lay(B[I], B[I + 1])); }
			Exits = { N.Lay(R[1], A[1]), N.Lay(R[2], A[3]), N.Lay(R[3], A[5]) };
			EndLinks = { N.Lay(A[0], B[0]), N.Lay(A[6], B[6]) };
			Stubs = { N.Lay(B[1], N.Node(50000.0, -55000.0)), N.Lay(B[2], N.Node(100000.0, -55000.0)) };
		}
	};
}

/** Backfill (spec): unnamed chains, longest first - the parallels take A and B, the links and stubs are connectors;
 *  logged once; a second load names nothing; and it is deterministic. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesBackfillTest, "Airside.Model.TaxiwayNames.BackfillGatwickShape",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesBackfillTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FGatwickShape G;
	FLogLineSpy Spy(TEXT("LogAirside"));
	GLog->AddOutputDevice(&Spy);
	const int32 Named = G.N.Net->EnsureTaxiwayNames(G.N.Rules);
	GLog->RemoveOutputDevice(&Spy);
	TestEqual(TEXT("two letters and seven connectors"), Named, 9);
	TestTrue(TEXT("logged as the spec words it"), Spy.CapturedLines.ContainsByPredicate([](const FString& L)
		{ return L.Contains(TEXT("TaxiwayNames: backfilled 2 taxiway(s), 7 connector(s)")); }));
	for (const FRoadSegmentId& S : G.ASegs) { TestEqual(TEXT("the first parallel is A"), G.N.NameOf(S), FString(TEXT("A"))); }
	for (const FRoadSegmentId& S : G.BSegs) { TestEqual(TEXT("the second parallel is B"), G.N.NameOf(S), FString(TEXT("B"))); }
	const TArray<FString> Expected = { TEXT("A1"), TEXT("A2"), TEXT("A3"), TEXT("A4"), TEXT("A5"), TEXT("B1"), TEXT("B2") };
	TArray<FString> Got;
	for (const FRoadSegmentId& S : G.Exits) { Got.Add(G.N.NameOf(S)); }
	for (const FRoadSegmentId& S : G.EndLinks) { Got.Add(G.N.NameOf(S)); }
	for (const FRoadSegmentId& S : G.Stubs) { Got.Add(G.N.NameOf(S)); }
	TestEqual(TEXT("exits A1-A3, end links A4-A5, stand stubs B1-B2"), Got, Expected);
	TestEqual(TEXT("a second load names nothing"), G.N.Net->EnsureTaxiwayNames(G.N.Rules), 0);

	FGatwickShape Again;
	Again.N.Net->EnsureTaxiwayNames(Again.N.Rules);
	FRoadNetworkTestAccess(*G.N.Net).ClearTaxiwayNamesForTest();
	G.N.Net->EnsureTaxiwayNames(G.N.Rules);
	for (int32 Index = 0; Index < G.N.Net->GetSegments().Num(); ++Index)
	{
		const FRoadSegmentId Id = G.N.Net->SegmentIdAt(Index);
		TestEqual(FString::Printf(TEXT("segment %d: same network, same name"), Index), G.N.NameOf(Id), Again.N.NameOf(Again.N.Net->SegmentIdAt(Index)));
	}
	return true;
}

namespace TaxiwayNamesTest
{
	/** Two 3 km parallels, A (y 0) and B below it, and a LINK from A's node at x 1 km down to B round a 90 degree
	 *  corner: First m at 45 degrees below +x, then Second m at 90 degrees to that (so 200 m in all - a connector's
	 *  length - but TWO chains, the corner being past BendDegrees). Laid A, B, first half, second half, each half from
	 *  the A end, so segment indices match between a backfill and a click-by-click drawing. bClick normalises per lay. */
	struct FCorneredLink
	{
		FTaxiwayNamesNet N;
		TArray<FRoadSegmentId> ASegs, BSegs, Link;

		FCorneredLink(double First, double Second, bool bClick)
		{
			const FVector2D Down1 = FVector2D(1.0, -1.0).GetSafeNormal();
			const FVector2D Down2 = FVector2D(-1.0, -1.0).GetSafeNormal();
			const FVector2D Start(100000.0, 0.0);
			const FVector2D Corner = Start + Down1 * First;
			const FVector2D End = Corner + Down2 * Second;
			const auto Lay = [this, bClick](FRoadNodeId A, FRoadNodeId B) { return bClick ? N.Click(A, B) : N.Lay(A, B); };
			const FRoadNodeId AMid = N.Node(Start.X, Start.Y);
			ASegs = { Lay(N.Node(0.0, 0.0), AMid), Lay(AMid, N.Node(300000.0, 0.0)) };
			const FRoadNodeId BMid = N.Node(End.X, End.Y);
			BSegs = { Lay(N.Node(0.0, End.Y), BMid), Lay(BMid, N.Node(300000.0, End.Y)) };
			const FRoadNodeId Bend = N.Node(Corner.X, Corner.Y);
			Link = { Lay(AMid, Bend), Lay(Bend, BMid) };
		}
	};

	/** The backfill of a cornered link against the same shape drawn click by click: both halves connectors, no third
	 *  letter, and the same name on every segment. */
	void CheckCorneredLink(FAutomationTestBase& Test, const TCHAR* Label, double First, double Second)
	{
		FCorneredLink Drawn(First, Second, true);
		FCorneredLink Backfilled(First, Second, false);
		Backfilled.N.Net->EnsureTaxiwayNames(Backfilled.N.Rules);
		const URoadNetwork& Net = *Backfilled.N.Net;
		for (const FRoadSegmentId& S : Backfilled.ASegs) { Test.TestEqual(FString::Printf(TEXT("%s: the first parallel is A"), Label), Backfilled.N.NameOf(S), FString(TEXT("A"))); }
		for (const FRoadSegmentId& S : Backfilled.BSegs) { Test.TestEqual(FString::Printf(TEXT("%s: the second parallel is B"), Label), Backfilled.N.NameOf(S), FString(TEXT("B"))); }
		int32 Letters = 0;
		for (const FTaxiway& T : Net.GetTaxiways()) { Letters += T.bAlive && !T.IsConnector() ? 1 : 0; }
		Test.TestEqual(FString::Printf(TEXT("%s: two letters - the link mints no third"), Label), Letters, 2);
		for (const FRoadSegmentId& S : Backfilled.Link)
		{
			const FTaxiway* T = Net.GetTaxiway(Net.TaxiwayOf(S));
			Test.TestTrue(FString::Printf(TEXT("%s: each half of the link is a connector (%s)"), Label, *Backfilled.N.NameOf(S)),
				T != nullptr && T->IsConnector());
		}
		Test.TestEqual(FString::Printf(TEXT("%s: control - drawn click by click, the link is A1 then A2"), Label),
			TArray<FString>{ Drawn.N.NameOf(Drawn.Link[0]), Drawn.N.NameOf(Drawn.Link[1]) }, TArray<FString>{ TEXT("A1"), TEXT("A2") });
		for (int32 Index = 0; Index < Net.GetSegments().Num(); ++Index)
		{
			Test.TestEqual(FString::Printf(TEXT("%s: segment %d backfills to the name drawing gives it"), Label, Index),
				Backfilled.N.NameOf(Net.SegmentIdAt(Index)), Drawn.N.NameOf(Drawn.N.Net->SegmentIdAt(Index)));
		}
	}
}

/** REVIEW FINDING (PR #524): backfill judged a link's first half against its still-unnamed second half - "not anchored",
 *  so a letter - and the second half became that letter's connector. A link of two halves must name as it draws. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesBackfillCorneredTest, "Airside.Model.TaxiwayNames.BackfillCorneredLinkIsConnector",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesBackfillCorneredTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	// Two equal 100 m halves: longest-first sorts them by index, the A half first.
	CheckCorneredLink(*this, TEXT("equal halves"), 10000.0, 10000.0);
	// A 50 m stub off A, then 150 m to B: longest-first meets the B half FIRST, whose first end is still unnamed.
	CheckCorneredLink(*this, TEXT("starts at a short stub"), 5000.0, 15000.0);
	return true;
}

/** Normalise is a FIXED POINT: once it has run, running it again writes nothing and announces no split - after every
 *  step of the invariant test's seeded edits. Backfill that judged against unnamed neighbours could leave a rejudge for
 *  the next pass; this is what would see it. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesIdempotentTest, "Airside.Model.TaxiwayNames.NormaliseIsIdempotent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesIdempotentTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	{
		// CONTROL: the snapshot sees a write - a normalise of an unnamed taxiway changes it.
		FTaxiwayNamesNet N;
		N.Lay(N.Node(0.0, 0.0), N.Node(50000.0, 0.0));
		const FString Before = NamingSnapshot(*N.Net);
		N.Normalise();
		TestNotEqual(TEXT("control: the snapshot sees a naming write"), NamingSnapshot(*N.Net), Before);
	}
	for (int32 Seed = 1; Seed <= 6; ++Seed)
	{
		TArray<FString> Names;
		const FString Breach = RandomEditRun(Seed, Names, true);
		if (!TestTrue(FString::Printf(TEXT("a second normalise changes nothing (%s)"), *Breach), Breach.IsEmpty())) { return false; }
	}
	return true;
}

/** Where an agent is, in names (spec "First consumers"): a taxiing aircraft on FTestAirport's one taxiway is on A - and
 *  before the names exist it is nowhere, which is what proves the answer is read from them (plan D13). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesWhereIsTest, "Airside.Model.TaxiwayNames.WhereIsAnAgent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesWhereIsTest::RunTest(const FString&)
{
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	FTestAirport Airport = FTestAirport::Build(Airframe);
	const FGuidelineNodeId Exit = RouteSearch::FindNearestNode(*Airport.Net, Airport.ExitAt, ETraversalClass::Aircraft, 200.0);
	const FRoutePlan Plan = TestGraph::Probe(*Airport.Net, Exit, Airport.Pose(Airport.Stands[0]), ETraversalClass::Aircraft);
	if (!TestTrue(TEXT("setup: a taxi route"), Plan.IsValid())) { return false; }
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Plane = Traffic->DispatchAgent(Airport.Net, Plan, Airframe, ETraversalClass::Aircraft, 0.0);
	if (!TestNotNull(TEXT("setup: dispatched"), Traffic->FindAgent(Plane))) { return false; }
	// OFF THE RUNWAY FIRST: the route starts on the runway's centreline node (FTestAirport), where the answer is the pair.
	const auto OnRunway = [&]()
	{
		const FRoadAgent* Moving = Traffic->FindAgent(Plane);
		const FRoutePlan& Route = Moving->PlanInProgress();
		const FGuidelineEdge* Edge = Airport.Net->GetGuidelineEdge(
			Route.Steps[UGroundTraffic::CurrentStep(Route, Moving->DistanceAlongPlan())].Edge);
		return Edge != nullptr && Airport.Net->IsRunwaySegment(Edge->DerivedFrom);
	};
	for (int32 Tick = 0; Tick < 4000 && OnRunway(); ++Tick) { Traffic->Advance(0.05, Airport.Net); }
	const FRoadAgent* Agent = Traffic->FindAgent(Plane);
	if (!TestTrue(TEXT("setup: off the runway"), Agent != nullptr && !OnRunway())) { return false; }
	TestEqual(TEXT("control: before any name exists it is nowhere"), InspectFacts::WhereIs(*Agent, *Airport.Net), FString());
	Airport.Net->NormaliseTaxiways(FTaxiwayNamingRules());
	const FString Where = InspectFacts::WhereIs(*Agent, *Airport.Net);
	TestTrue(FString::Printf(TEXT("on the fixture's one taxiway, A, or its junction ('%s')"), *Where), Where == TEXT("A"));
	FAgentFacts Facts;
	if (TestTrue(TEXT("described"), InspectFacts::DescribeAgent(*Traffic, Airport.Net, Plane, Facts)))
	{
		TestEqual(TEXT("the card's On is the same answer"), Facts.On, Where);
	}
	return true;
}

/** Labels (spec "UI"): one per taxiway at the middle of its longest segment, repeated every RepeatEvery along it;
 *  none for an empty parent kept only for its connectors. Meanings - a name and a road-plane point - never a colour. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesLabelAnchorsTest, "Airside.Model.TaxiwayNames.LabelAnchors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesLabelAnchorsTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	TArray<FRoadNodeId> A;
	for (int32 Index = 0; Index <= 4; ++Index) { A.Add(N.Node(30000.0 * Index, 0.0)); }
	for (int32 Index = 0; Index < 4; ++Index) { N.Click(A[Index], A[Index + 1]); }
	const FRoadSegmentId Stub = N.Click(A[2], N.Node(60000.0, -20000.0));
	const TArray<FTaxiwayLabel> Labels = TaxiwayLabels::Anchors(*N.Net, 50000.0);
	TArray<double> AxAt;
	int32 Connectors = 0;
	for (const FTaxiwayLabel& Label : Labels)
	{
		if (Label.Name == TEXT("A")) { AxAt.Add(Label.At.X); }
		if (Label.Name == TEXT("A1")) { ++Connectors; TestTrue(FString::Printf(TEXT("A1 at its middle, not %s"), *Label.At.ToString()), Label.At.Equals(FVector2D(60000.0, -10000.0), 1.0)); }
	}
	AxAt.Sort();
	// 1200 m in four equal 300 m segments: the tie goes to the first in chain order, middle at 150 m; then every 500 m.
	// ONE ASSERTION PER ANCHOR: TestEqual has no TArray<double> overload (plan correction), and a count alone would pass misplaced tags.
	const TArray<double> Expected{ 15000.0, 65000.0, 115000.0 };
	if (TestEqual(TEXT("A repeated every 500 m: three anchors"), AxAt.Num(), Expected.Num()))
	{
		for (int32 Index = 0; Index < Expected.Num(); ++Index)
		{
			TestEqual(FString::Printf(TEXT("A's anchor %d from the middle of its longest segment"), Index), AxAt[Index], Expected[Index], 1.0);
		}
	}
	TestEqual(TEXT("a short connector once"), Connectors, 1);
	N.Net->RemoveSegment(N.Net->SegmentIdAt(0));
	for (int32 Index = 1; Index < 4; ++Index) { N.Net->RemoveSegment(N.Net->SegmentIdAt(Index)); }
	N.Normalise();
	bool bParentLabel = false;
	for (const FTaxiwayLabel& Label : TaxiwayLabels::Anchors(*N.Net, 50000.0)) { bParentLabel |= Label.Name == TEXT("A"); }
	TestFalse(TEXT("an empty parent kept for A1 draws no label"), bParentLabel);
	TestEqual(TEXT("A1 is still drawn"), N.NameOf(Stub), FString(TEXT("A1")));
	return true;
}

/** Rename propagation (spec): A -> K makes its connectors K1..; a connector's own override survives its parent's rename;
 *  a renamed taxiway is never re-judged (plan D3); the old letter is free again (plan D15). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesRenameTest, "Airside.Model.TaxiwayNames.RenamePropagatesToConnectors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesRenameTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	TArray<FRoadNodeId> A;
	for (int32 Index = 0; Index <= 3; ++Index) { A.Add(N.Node(50000.0 * Index, 0.0)); }
	FRoadSegmentId Main;
	for (int32 Index = 0; Index < 3; ++Index) { Main = N.Click(A[Index], A[Index + 1]); }
	const FRoadSegmentId One = N.Click(A[1], N.Node(50000.0, -20000.0));
	const FRoadSegmentId Two = N.Click(A[2], N.Node(100000.0, -20000.0));
	const int32 AId = N.Net->TaxiwayOf(Main);
	const uint32 Revision = N.Net->GetGuidelineRevision();
	TestTrue(TEXT("A renamed k"), N.Net->RenameTaxiway(AId, TEXT(" k ")));
	TestTrue(TEXT("a rename moves the revision the card and caches key on (plan D11)"), N.Net->GetGuidelineRevision() != Revision);
	TestEqual(TEXT("trimmed and upper-cased"), N.NameOf(Main), FString(TEXT("K")));
	TestEqual(TEXT("A1 reads K1"), N.NameOf(One), FString(TEXT("K1")));
	TestEqual(TEXT("A2 reads K2"), N.NameOf(Two), FString(TEXT("K2")));
	TestTrue(TEXT("K2 overridden Q7"), N.Net->RenameTaxiway(N.Net->TaxiwayOf(Two), TEXT("Q7")));
	TestTrue(TEXT("K renamed M"), N.Net->RenameTaxiway(AId, TEXT("M")));
	TestEqual(TEXT("the derived one follows"), N.NameOf(One), FString(TEXT("M1")));
	TestEqual(TEXT("the override stays"), N.NameOf(Two), FString(TEXT("Q7")));
	const FRoadSegmentId Fresh = N.Click(N.Node(0.0, 200000.0), N.Node(60000.0, 200000.0));
	TestEqual(TEXT("A is free again: nothing displays it"), N.NameOf(Fresh), FString(TEXT("A")));
	// A PLAYER'S NAME IS NEVER RE-JUDGED: rename a stub, then grow it past 300 m.
	const FRoadNodeId Free = N.Node(150000.0, -20000.0);
	const FRoadSegmentId Stub = N.Click(A[3], Free);
	TestTrue(TEXT("the stub renamed Z9"), N.Net->RenameTaxiway(N.Net->TaxiwayOf(Stub), TEXT("Z9")));
	N.Click(Free, N.Node(150000.0, -60000.0));
	TestEqual(TEXT("grown past 300 m it keeps the player's name"), N.NameOf(Stub), FString(TEXT("Z9")));
	return true;
}

/** REVIEW FOCUS 5: what a player types. Each refusal says why, in the spec's words; a refused rename changes nothing. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesRenameRefusalsTest, "Airside.Model.TaxiwayNames.RenameRefusals",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesRenameRefusalsTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	const FRoadNodeId A1 = N.Node(50000.0, 0.0);
	const FRoadSegmentId A = N.Click(N.Node(0.0, 0.0), A1);
	N.Click(A1, N.Node(100000.0, 0.0));
	const FRoadSegmentId B = N.Click(N.Node(0.0, 100000.0), N.Node(60000.0, 100000.0));
	const FRoadSegmentId Stub = N.Click(A1, N.Node(50000.0, -20000.0));
	const int32 AId = N.Net->TaxiwayOf(A);
	TestTrue(TEXT("setup: the stub overridden K2"), N.Net->RenameTaxiway(N.Net->TaxiwayOf(Stub), TEXT("K2")));
	TestEqual(TEXT("taken"), N.Net->WhyTaxiwayNameRefused(AId, TEXT("b")), FString(TEXT("B is taken")));
	TestEqual(TEXT("I"), N.Net->WhyTaxiwayNameRefused(AId, TEXT("I")), FString(TEXT("I, O and X are avoided: they read as 1, 0 and closed")));
	TestEqual(TEXT("O inside"), N.Net->WhyTaxiwayNameRefused(AId, TEXT("GO")), FString(TEXT("I, O and X are avoided: they read as 1, 0 and closed")));
	TestEqual(TEXT("four characters"), N.Net->WhyTaxiwayNameRefused(AId, TEXT("ABCD")), FString(TEXT("A taxiway name is 1 to 3 letters or digits")));
	TestEqual(TEXT("empty"), N.Net->WhyTaxiwayNameRefused(AId, TEXT("  ")), FString(TEXT("A taxiway name is 1 to 3 letters or digits")));
	TestEqual(TEXT("punctuation"), N.Net->WhyTaxiwayNameRefused(AId, TEXT("A-1")), FString(TEXT("A taxiway name is 1 to 3 letters or digits")));
	// A COLLISION ONLY THROUGH A DERIVED NAME: the stub took A1 and now shows K2, so A's next connector is A2 - and
	// renaming A to K would make it read K2 as well.
	const FRoadSegmentId Second = N.Click(N.Net->GetSegment(A)->A, N.Node(0.0, -20000.0));
	TestEqual(TEXT("setup: A's next connector is A2"), N.NameOf(Second), FString(TEXT("A2")));
	TestEqual(TEXT("K would make A2 read K2, which the stub shows"), N.Net->WhyTaxiwayNameRefused(AId, TEXT("K")), FString(TEXT("K2 is taken")));
	TestFalse(TEXT("a refused rename changes nothing"), N.Net->RenameTaxiway(AId, TEXT("K")));
	TestEqual(TEXT("A is still A"), N.NameOf(A), FString(TEXT("A")));
	TestEqual(TEXT("renaming to its own name is no refusal"), N.Net->WhyTaxiwayNameRefused(AId, TEXT("a")), FString());
	TestEqual(TEXT("digits are fine"), N.Net->WhyTaxiwayNameRefused(AId, TEXT("10")), FString());
	TestEqual(TEXT("a dead taxiway"), N.Net->WhyTaxiwayNameRefused(999, TEXT("Q")), FString(TEXT("That taxiway is gone")));
	TestEqual(TEXT("B untouched"), N.NameOf(B), FString(TEXT("B")));
	return true;
}


#endif // WITH_DEV_AUTOMATION_TESTS
