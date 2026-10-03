#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/TaxiwayStrip.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * TAXIWAY NAMES AT THE COMPOSITION (spec "Seam tests"): through ARoadNetworkActor and its facade, so each test goes red
 * if its seam - the load repair, the facade's notify - stops calling the model.
 */

/**
 * REVIEW FOCUS 4: a network saved before names, with roads that lost their own profile to a save (Profile null -
 * URoadNetwork::DefaultProfile's own comment), is named on BOTH load paths: a level's and a save game's (Serialize alone).
 * Plan D6: only after RepairLoadedNetwork re-resolves the default can those roads be read as taxiways at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesLoadTest, "Airside.Present.TaxiwayNames.LoadBackfillsBothPaths",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesLoadTest::RunTest(const FString&)
{
	for (const ELoadedFrom From : { ELoadedFrom::Level, ELoadedFrom::SaveGame })
	{
		FAirsideTestWorld TestWorld;
		ARoadNetworkActor* Actor = TestWorld.Actor;
		if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
		// THE NETWORK IS MADE BY THE FIRST EDIT (Airside.Present.RepairReResolvesAMissingDefault's own setup) - a fresh
		// actor has none, and the repair is a no-op without one.
		Actor->PlaceNode(FVector2D(0.0, -100000.0));
		URoadNetwork* Net = Actor->Network;
		if (!TestNotNull(TEXT("the first edit made a network"), Net)) { return false; }
		const FRoadSegmentId Lost = Net->AddStraightSegment(Net->AddNode({ 0.0, 0.0 }), Net->AddNode({ 50000.0, 0.0 }), nullptr);
		Net->DefaultProfile = nullptr;
		TestFalse(TEXT("control: with no default it reads as no taxiway, so naming before the repair would skip it"),
			TaxiwayStrip::HasStrip(*Net, Lost));
		Actor->RepairLoadedNetwork(From);
		TestEqual(FString::Printf(TEXT("load path %d: the profile-less road is taxiway A"), static_cast<int32>(From)),
			Net->TaxiwayDisplayName(Net->TaxiwayOf(Lost)), FString(TEXT("A")));
	}
	return true;
}

/**
 * THE FACADE NOTIFY NAMES (plan D9) AND REVIEW FOCUS 3: a taxiway drawn click by click through the actor is ONE name because the
 * facade normalises on every Topology notify; deleting its middle announces "B split off from A" ONCE; undo brings A
 * back whole and says nothing; redo splits it again.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesFacadeTest, "Airside.Present.TaxiwayNames.SplitIsAnnouncedAndUndone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesFacadeTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestNotNull(TEXT("its facade"), Facade)) { return false; }
	TArray<FString> Heard;
	const FDelegateHandle Handle = Facade->OnTaxiwaySplit.AddLambda([&Heard](const FString& SplitOff, const FString& From)
		{ Heard.Add(SplitOff + TEXT("<") + From); });

	const int32 N0 = Actor->PlaceNode({ 0.0, 0.0 });
	const int32 N1 = Actor->PlaceNode({ 40000.0, 0.0 });
	const int32 N2 = Actor->PlaceNode({ 50000.0, 0.0 });
	const int32 N3 = Actor->PlaceNode({ 70000.0, 0.0 });
	TestTrue(TEXT("click 1"), Actor->ConnectNodes(N0, N1, ERoadKind::Taxiway, INDEX_NONE, EPavement::Tarmac));
	const int32 Long = Actor->Network->GetSegments().Num() - 1;
	TestTrue(TEXT("click 2"), Actor->ConnectNodes(N1, N2, ERoadKind::Taxiway, INDEX_NONE, EPavement::Tarmac));
	const int32 Middle = Actor->Network->GetSegments().Num() - 1;
	TestTrue(TEXT("click 3"), Actor->ConnectNodes(N2, N3, ERoadKind::Taxiway, INDEX_NONE, EPavement::Tarmac));
	const int32 Short = Actor->Network->GetSegments().Num() - 1;
	const auto NameAt = [Actor](int32 Index) { return Actor->Network->TaxiwayDisplayName(Actor->Network->TaxiwayOf(Actor->Network->SegmentIdAt(Index))); };
	TestEqual(TEXT("three clicks through the actor are one taxiway, named with no explicit call"), NameAt(Short), FString(TEXT("A")));

	TestTrue(TEXT("the middle is deleted"), Actor->DeleteSegment(Middle));
	TestEqual(TEXT("the split is announced once"), Heard, TArray<FString>{ TEXT("B<A") });
	TestEqual(TEXT("the long piece keeps A"), NameAt(Long), FString(TEXT("A")));
	TestEqual(TEXT("the short piece is B"), NameAt(Short), FString(TEXT("B")));

	TestTrue(TEXT("undo"), Facade->Undo());
	TestEqual(TEXT("undo brings A back whole"), NameAt(Short), FString(TEXT("A")));
	TestEqual(TEXT("and announces nothing - the Memento carried the names"), Heard.Num(), 1);
	int32 Alive = 0;
	for (const FTaxiway& T : Actor->Network->GetTaxiways()) { Alive += T.bAlive ? 1 : 0; }
	TestEqual(TEXT("one live taxiway after the undo"), Alive, 1);

	TestTrue(TEXT("redo"), Facade->Redo());
	TestEqual(TEXT("redo splits again"), NameAt(Short), FString(TEXT("B")));
	Facade->OnTaxiwaySplit.Remove(Handle);
	return true;
}

/**
 * OWNER RULING 2026-10-02 (PR #524) AT THE COMPOSITION: a connector left on the split-off piece is re-parented AND the
 * facade announces it on the split's own event ("C1<A2"); undo brings A2 back silently - the Memento carries ParentId and
 * ConnectorNumber with the names; redo re-parents it again.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesReparentFacadeTest, "Airside.Present.TaxiwayNames.ReparentIsAnnouncedAndUndone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesReparentFacadeTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestNotNull(TEXT("its facade"), Facade)) { return false; }
	TArray<FString> Heard;
	const FDelegateHandle Handle = Facade->OnTaxiwaySplit.AddLambda([&Heard](const FString& SplitOff, const FString& From)
		{ Heard.Add(SplitOff + TEXT("<") + From); });
	const auto Connect = [this, Actor](int32 From, int32 To)
	{
		TestTrue(TEXT("a click"), Actor->ConnectNodes(From, To, ERoadKind::Taxiway, INDEX_NONE, EPavement::Tarmac));
		return Actor->Network->GetSegments().Num() - 1;
	};
	const auto NameAt = [Actor](int32 Index) { return Actor->Network->TaxiwayDisplayName(Actor->Network->TaxiwayOf(Actor->Network->SegmentIdAt(Index))); };

	// A: 600 m, a 100 m middle, 300 m; B far away; stubs A1 (near) and A2 (far) - the model test's FStubbedA, by actor.
	TArray<int32> P;
	for (const double X : { 0.0, 30000.0, 60000.0, 70000.0, 85000.0, 100000.0 }) { P.Add(Actor->PlaceNode({ X, 0.0 })); }
	TArray<int32> ASegs;
	for (int32 I = 0; I + 1 < P.Num(); ++I) { ASegs.Add(Connect(P[I], P[I + 1])); }
	Connect(Actor->PlaceNode({ 0.0, 200000.0 }), Actor->PlaceNode({ 60000.0, 200000.0 }));
	const int32 Near = Connect(P[1], Actor->PlaceNode({ 30000.0, -20000.0 }));
	const int32 Far = Connect(P[4], Actor->PlaceNode({ 85000.0, -20000.0 }));
	TestEqual(TEXT("setup: near stub A1"), NameAt(Near), FString(TEXT("A1")));
	TestEqual(TEXT("setup: far stub A2"), NameAt(Far), FString(TEXT("A2")));
	TestEqual(TEXT("setup: nothing announced while drawing"), Heard.Num(), 0);

	TestTrue(TEXT("the middle is deleted"), Actor->DeleteSegment(ASegs[2]));
	TestEqual(TEXT("the split, then the rename"), Heard, TArray<FString>{ TEXT("C<A"), TEXT("C1<A2") });
	TestEqual(TEXT("the far stub is C1"), NameAt(Far), FString(TEXT("C1")));
	TestEqual(TEXT("the near stub is still A1"), NameAt(Near), FString(TEXT("A1")));

	TestTrue(TEXT("undo"), Facade->Undo());
	TestEqual(TEXT("undo restores A2"), NameAt(Far), FString(TEXT("A2")));
	TestEqual(TEXT("and A whole"), NameAt(ASegs[4]), FString(TEXT("A")));
	TestEqual(TEXT("and announces nothing"), Heard.Num(), 2);

	TestTrue(TEXT("redo"), Facade->Redo());
	TestEqual(TEXT("redo re-parents it again"), NameAt(Far), FString(TEXT("C1")));
	Facade->OnTaxiwaySplit.Remove(Handle);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
