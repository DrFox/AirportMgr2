#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Model/BuildPurse.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "Solve/IcaoCode.h"
#include "Solve/LetterEnvelope.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/BuildSession.h"
#include "Tool/PlotGesture.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/StandPlotTool.h"
#include "Tool/ToolReadout.h"

#if WITH_DEV_AUTOMATION_TESTS

// NAMED, NOT ANONYMOUS - the game module's tests are one unity build too, and a common name
// (a purse fake, a context builder) compiles alone and collides with another file's copy.
namespace ReadoutCacheTestFixture
{
	/** A purse whose balance the test moves BETWEEN ticks. Declared before the world in the test
	 *  that uses it, so it outlives the facade holding a raw pointer to it. */
	struct FMovableFundsPurse : IBuildPurse
	{
		double Funds = 0.0;

		virtual bool CanAfford(const FBuildQuote& Quote) const override { return Quote.BaseAmount() <= Funds; }
		virtual double Balance() const override { return Funds; }
		/** DOES NOT DEBIT: the test sets Funds by hand, and a placement that moved the balance would
		 *  make the edit-epoch case below pass on the PURSE key and prove nothing about the epoch. */
		virtual int32 Charge(const FBuildQuote&) override { return 1; }
		virtual void Reverse(int32) override {}
		virtual void Credit(const FBuildQuote& Quote) override { Funds += Quote.BaseAmount(); }
		virtual FText Describe(const FBuildQuote& Quote) const override { return FText::AsNumber(Quote.BaseAmount()); }
	};

	/** A click's context at Where, Free-snapped, 150uu - the shape the Airside tool tests use
	 *  (TestTool::ContextAt), rebuilt here because that fixture lives in a test module this one
	 *  may not depend on. */
	FToolContext ClickAt(IRoadEditTarget& Target, const FVector2D& Where)
	{
		FToolContext Context;
		Context.Target = &Target;
		Context.SnapRadius = 150.0;
		Context.Envelopes = FLetterEnvelopeTable::Floor();
		FRoadSnapResult Snap;
		Snap.Kind = ERoadSnapKind::Free;
		Snap.Position = Where;
		Context.SetCursor(Where, Snap);
		return Context;
	}
}

/**
 * ONE READOUT REBUILD FOR A STILL CURSOR, issue #190. Before this,
 * ARoadBuildController::CollectToolReadout ran Tool->BuildReadout every PlayerTick regardless
 * of whether anything the tool could answer differently about had changed - a
 * TArray<TPair<FString,FString>> plus a Printf per fact, paid every frame a gesture sat idle
 * under a motionless mouse.
 *
 * COUNTED THROUGH GetToolReadoutRevision, bumped only when CollectToolReadout actually calls
 * BuildReadout - see FToolReadoutKey's own comment for what the cache compares and why a
 * still cursor is exactly the case it exists for.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReadoutRebuildsOnceForStillCursorTest,
	"AirportMgr.Actions.ReadoutRebuildsOnceForStillCursor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FReadoutRebuildsOnceForStillCursorTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Target = TestWorld.Actor;
	if (!TestNotNull(TEXT("a target actor"), Target)) { return false; }

	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }

	// Same bypass and same reason as FPlayerTickBuildsOneContextTest: no level here for
	// BeginPlay's TActorIterator to find Target in, and no PlayerInput without this call.
	C->SetTargetForTest(Target);
	C->InitInputSystem();

	// Taxiway: a tool whose BuildReadout does real work over the pending gesture, not
	// Select's near-empty idle path - see FPlayerTickBuildsOneContextTest's own comment.
	C->SelectTool(1);

	const int32 Before = C->GetToolReadoutRevision();

	// K TICKS, NO MOUSE MOVED BETWEEN THEM. This test drives no viewport, so
	// MakeToolContext's CursorOnRoadPlane fails every call and PlayerTick's FrameContext
	// falls back to Session.LastPlaneHit() - the same position each time, with no click or
	// drag between ticks to advance the tool's own stage.
	constexpr int32 Ticks = 5;
	for (int32 Index = 0; Index < Ticks; ++Index)
	{
		C->PlayerTickForTest(1.0f / 60.0f);
	}

	const int32 After = C->GetToolReadoutRevision();

	// EXACTLY ONE: the first tick has no cached key yet and must build the readout; the
	// remaining Ticks - 1 see an unchanged FToolReadoutKey and must not rebuild it. This goes
	// to Ticks if the cache is ever bypassed, and to 0 if BuildReadout stops running at all.
	TestEqual(TEXT("a still cursor rebuilds the readout once across several ticks, not once "
					"per tick"),
		After - Before, 1);

	return true;
}

/**
 * THE READOUT CACHE SEES THE PURSE (issue #439). FToolReadoutKey named the cursor, the snap, the
 * guide and the grid - everything the PLAYER moves - and not the balance, which moves on its own
 * (landing fees, upkeep, a load). A stand in Confirm with the cursor held still therefore kept
 * the "cannot afford" readout it was greyed with, however much money arrived, until the mouse
 * moved: the tool's own answer had been made honest, and the controller kept serving last
 * frame's copy of it.
 *
 * DRIVEN THROUGH THE CONTROLLER'S OWN COLLECTION with a real stand tool pinned to Confirm, so
 * this fails on the cache and not on the tool: same cursor, same tool, only the purse moves.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReadoutCacheSeesThePurseTest,
	"AirportMgr.Actions.ReadoutCacheSeesThePurse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FReadoutCacheSeesThePurseTest::RunTest(const FString& Parameters)
{
	using namespace ReadoutCacheTestFixture;

	ReadoutCacheTestFixture::FMovableFundsPurse Purse; // before the world - the facade holds a raw pointer to it

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a target actor"), Actor)) { return false; }
	Actor->ClearNetwork();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();
	IRoadEditTarget* Edit = Actor;
	Edit->ConnectNodes(Edit->PlaceNode(FVector2D(-10000.0, 0.0)), Edit->PlaceNode(FVector2D(10000.0, 0.0)),
		ERoadKind::Taxiway, INDEX_NONE);
	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestNotNull(TEXT("an edit facade"), Facade)) { return false; }
	Facade->SetPurse(&Purse);
	ON_SCOPE_EXIT { Facade->SetPurse(nullptr); };

	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(Actor);
	C->InitInputSystem();

	// THE STAND TOOL, looked up by its registry name rather than an index that a re-ordered
	// registry would move under this test.
	int32 StandIndex = INDEX_NONE;
	for (int32 Index = 0; Index < ToolRegistry().Num(); ++Index)
	{
		if (ToolRegistry()[Index].Id == FName(TEXT("Stand"))) { StandIndex = Index; }
	}
	if (!TestTrue(TEXT("the registry names a Stand tool"), StandIndex != INDEX_NONE)) { return false; }
	C->SelectTool(StandIndex);
	FStandPlotTool* Tool = static_cast<FStandPlotTool*>(C->GetActiveTool());
	if (!TestNotNull(TEXT("the stand tool is active"), Tool)) { return false; }

	// THREE CLICKS TO CONFIRM, straight into the tool: the controller's own click path needs a
	// viewport this test does not have. The readout below still goes through the controller.
	const FVector2D AnchorCursor(0.0, 1000.0);
	Tool->OnClick(ClickAt(*Actor, AnchorCursor));
	TArray<FVector2D> Shown;
	Tool->Rect(ClickAt(*Actor, AnchorCursor), Shown);
	if (!TestTrue(TEXT("the first click anchored on the taxiway"), Shown.Num() == 4)) { return false; }
	const FVector2D Anchor = Shown[0];
	const double FloorWidth = IcaoCode::StandWidthForLetter(EIcaoCode::C);
	const double Width = FloorWidth <= PlotGesture::MinFrontageUu ? PlotGesture::MinFrontageUu
		: PlotGesture::MinFrontageUu
			+ FMath::CeilToDouble((FloorWidth - PlotGesture::MinFrontageUu) / PlotGesture::FrontageStepUu)
			* PlotGesture::FrontageStepUu;
	Tool->OnClick(ClickAt(*Actor, Anchor + FVector2D(Width, 0.0)));
	Tool->OnClick(ClickAt(*Actor, Anchor + FVector2D(Width, IcaoCode::StandDepthForLetter(EIcaoCode::C))));
	if (!TestTrue(TEXT("a Code C stand reaches Confirm"), Tool->GetStage() == EStandStage::Confirm)) { return false; }

	// SHORT, drawn ahead of the money. The first collection has no cached key, so it builds.
	Purse.Funds = 0.0;
	C->CollectToolReadoutForTest();
	const int32 Built = C->GetToolReadoutRevision();
	TestFalse(TEXT("purse short: Build is not committable"), C->GetToolReadout().bCommittable);

	// THE CONTROL: nothing moved, so the cache answers - or a rebuild every frame would pass below
	// for the wrong reason.
	C->CollectToolReadoutForTest();
	TestEqual(TEXT("the same cursor and the same purse rebuild nothing"), C->GetToolReadoutRevision(), Built);

	// CREDITED, THE CURSOR STILL: only the balance changed.
	Purse.Funds = 1.0e12;
	C->CollectToolReadoutForTest();
	TestEqual(TEXT("a credited purse rebuilds the readout though the cursor did not move"),
		C->GetToolReadoutRevision(), Built + 1);
	TestTrue(TEXT("and the rebuilt readout is committable"), C->GetToolReadout().bCommittable);

	// AND DRAINED AGAIN, the other direction.
	Purse.Funds = 0.0;
	C->CollectToolReadoutForTest();
	TestEqual(TEXT("a drained purse rebuilds it again"), C->GetToolReadoutRevision(), Built + 2);
	TestFalse(TEXT("and Build greys"), C->GetToolReadout().bCommittable);

	// AN EDIT UNDER AN UNMOVED CURSOR, THE PURSE UNTOUCHED (the edit epoch's key). Funded again, the
	// readout lights; then a stand lands on the pinned ground through the target - not through this
	// controller, so nothing here invalidates the cache by hand - and the very next collection must
	// re-ask the tool, which now refuses. The fixture's Charge does not debit, so the balance is
	// exactly what it was: the epoch is the only thing in the key that moved.
	Purse.Funds = 1.0e12;
	C->CollectToolReadoutForTest();
	const int32 Lit = C->GetToolReadoutRevision();
	if (!TestTrue(TEXT("funded again: Build is committable"), C->GetToolReadout().bCommittable)) { return false; }
	C->CollectToolReadoutForTest();
	TestEqual(TEXT("the control: the same cursor, purse and model rebuild nothing"),
		C->GetToolReadoutRevision(), Lit);

	TArray<FVector2D> Outline;
	Tool->Rect(ClickAt(*Actor, AnchorCursor), Outline);
	if (!TestTrue(TEXT("a stand is placed onto the pinned outline"),
		Edit->PlaceStandInPlot(Outline, Outline[0], Outline[1], EPavement::Grass) != INDEX_NONE)) { return false; }
	TestEqual(TEXT("premise: the balance is exactly what it was, so only the epoch can have moved"),
		Purse.Funds, 1.0e12);
	C->CollectToolReadoutForTest();
	TestEqual(TEXT("a stand placed under a still cursor rebuilds the readout"), C->GetToolReadoutRevision(), Lit + 1);
	TestFalse(TEXT("and Build greys over the overlap"), C->GetToolReadout().bCommittable);
	TestTrue(TEXT("naming it"), C->GetToolReadout().Warnings.ContainsByPredicate(
		[](const FString& W) { return W.Contains(TEXT("overlaps")); }));
	return true;
}

#endif
