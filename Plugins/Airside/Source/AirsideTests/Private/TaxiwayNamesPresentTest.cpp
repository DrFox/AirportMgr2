#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/TaxiwayStrip.h"
#include "Present/PreviewPalette.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/BuildSession.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/TaxiwayNameOverlay.h"

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

namespace TaxiwayNamesPresentTest
{
	/** Records Label calls only. Prefixed against the UNITY build. */
	struct FTaxiwayNamesLabelSink : IToolPreviewSink
	{
		TArray<FString> Names;
		virtual void Marker(const FVector2D&, EPreviewStyle) override {}
		virtual void Line(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void Label(const FVector2D&, const FString& Text, EPreviewStyle Style) override
		{
			if (Style == EPreviewStyle::TaxiwayName) { Names.AddUnique(Text); }
		}
	};
}

/** Spawn the actor, draw, and the labels the drivers are handed are exactly the names (spec "Seam tests"). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesLabelsTest, "Airside.Present.TaxiwayNames.LabelsMatchNames",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesLabelsTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesPresentTest;
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	TestTrue(TEXT("a taxiway"), Actor->ConnectNodes(Actor->PlaceNode({ 0.0, 0.0 }), Actor->PlaceNode({ 60000.0, 0.0 }),
		ERoadKind::Taxiway, INDEX_NONE, EPavement::Tarmac));
	TestTrue(TEXT("another"), Actor->ConnectNodes(Actor->PlaceNode({ 0.0, 100000.0 }), Actor->PlaceNode({ 60000.0, 100000.0 }),
		ERoadKind::Taxiway, INDEX_NONE, EPavement::Tarmac));
	if (!TestNotNull(TEXT("the edits made a network"), Actor->Network.Get())) { return false; }
	FTaxiwayNamesLabelSink Sink;
	const int32 Described = TaxiwayNameOverlay::Describe(*Actor->Network, Actor->TaxiwayNaming.LabelRepeatDistance, Sink);
	Sink.Names.Sort();
	TestEqual(TEXT("the labels are the names"), Sink.Names, TArray<FString>{ TEXT("A"), TEXT("B") });
	TestEqual(TEXT("the count the drivers log is the count described (one 600 m taxiway, one label each)"), Described, 2);
	TestTrue(TEXT("a name is drawn as a tag"), PreviewPalette::DefaultLook(EPreviewStyle::TaxiwayName).bTag);
	TestFalse(TEXT("and nothing else is"), PreviewPalette::DefaultLook(EPreviewStyle::Pending).bTag);

	// WHEN (spec): shown while a build tool is lit; while watching, the G toggle decides. The session answers for both drivers.
	FBuildSession Session;
	Session.SelectTool(FBuildSession::SelectToolIndex);
	TestTrue(TEXT("watching, G on: names shown"), Session.WantsTaxiwayNamesDrawn(true));
	TestFalse(TEXT("watching, G off: hidden"), Session.WantsTaxiwayNamesDrawn(false));
	Session.SelectTool(1);   // Taxiway - a build tool whose rings are lit (Airside.Tool.RoadNodesStandDownOutsideTheRoadTools)
	TestTrue(TEXT("control: the taxiway tool lights the rings"), Session.WantsRoadNodesDrawn());
	TestTrue(TEXT("a lit build tool shows them with G off"), Session.WantsTaxiwayNamesDrawn(false));
	return true;
}

/** A rename through the facade is ONE undo step (spec: "a test undoes a rename"). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesRenameUndoTest, "Airside.Present.TaxiwayNames.RenameIsOneUndoStep",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesRenameUndoTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	TestTrue(TEXT("a taxiway"), Actor->ConnectNodes(Actor->PlaceNode({ 0.0, 0.0 }), Actor->PlaceNode({ 60000.0, 0.0 }),
		ERoadKind::Taxiway, INDEX_NONE, EPavement::Tarmac));
	const FRoadSegmentId Seg = Actor->Network->SegmentIdAt(Actor->Network->GetSegments().Num() - 1);
	const int32 Id = Actor->Network->TaxiwayOf(Seg);
	URoadEditFacade* Facade = Actor->GetEditFacade();
	FString Why;
	TestFalse(TEXT("a refusal is said"), Facade->RenameTaxiway(Id, TEXT("I"), Why));
	TestEqual(TEXT("in the spec's words"), Why, FString(TEXT("I, O and X are avoided: they read as 1, 0 and closed")));
	TestTrue(TEXT("renamed"), Facade->RenameTaxiway(Id, TEXT("k"), Why));
	TestEqual(TEXT("K"), Actor->Network->TaxiwayDisplayName(Id), FString(TEXT("K")));
	TestTrue(TEXT("undo"), Facade->Undo());
	TestEqual(TEXT("one undo puts A back"), Actor->Network->TaxiwayDisplayName(Id), FString(TEXT("A")));
	return true;
}


#endif // WITH_DEV_AUTOMATION_TESTS
