#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "AirsideTestFixtures.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Model/BuildPurse.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/RoadDrawTool.h"
#include "Tool/RoadEditTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * A purse that records rather than banks, so a test can see exactly what the facade did.
	 *
	 * A FAKE AND NOT ULedger, deliberately: these tests are about the SEAM - that the facade
	 * charges, reverses and credits at the right moments - not about what a ledger does with
	 * the numbers. ULedger has its own tests, world-free, in AirportOps.
	 */
	class FRecordingPurse : public IBuildPurse
	{
	public:
		double Funds = 1000000.0;
		TArray<double> Charges;
		TArray<int32> Reversed;
		TArray<double> Credits;
		int32 NextId = 1;

		virtual bool CanAfford(const FBuildQuote& Quote) const override
		{
			return Quote.BaseAmount <= Funds;
		}

		virtual int32 Charge(const FBuildQuote& Quote) override
		{
			Funds -= Quote.BaseAmount;
			Charges.Add(Quote.BaseAmount);
			return NextId++;
		}

		virtual void Reverse(int32 ChargeId) override { Reversed.Add(ChargeId); }

		virtual void Credit(const FBuildQuote& Quote) override { Credits.Add(Quote.BaseAmount); }

		virtual FText Describe(const FBuildQuote& Quote) const override
		{
			return FText::AsNumber(Quote.BaseAmount);
		}
	};

	/** Gives the actor's default taxiway profile a price, so a connection costs something. */
	void PriceTheTaxiway(ARoadNetworkActor& Actor)
	{
		if (URoadProfile* Profile = Actor.ResolveProfile())
		{
			Profile->CostPerMetre = 300.0;
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPurseChargesTest,
	"Airside.Present.BuildPurseCharges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildPurseChargesTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld World;
	PriceTheTaxiway(*World.Actor);

	FRecordingPurse Purse;
	World.Actor->GetEditFacade()->SetPurse(&Purse);

	const int32 A = World.Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = World.Actor->PlaceNode(FVector2D(10000.0, 0.0));
	TestTrue(TEXT("the taxiway is built"), World.Actor->ConnectNodes(A, B, ERoadKind::Taxiway, INDEX_NONE));

	TestEqual(TEXT("connecting two nodes charges exactly once"), Purse.Charges.Num(), 1);
	TestEqual(TEXT("100 m of a 300-per-metre taxiway costs 30,000"),
		Purse.Charges.IsValidIndex(0) ? Purse.Charges[0] : 0.0, 30000.0, 1e-6);

	// PLACING A BARE NODE IS FREE, and that is a decision rather than an accident: a node holds
	// no pavement, so there is nothing to pay for until it is joined to something.
	TestEqual(TEXT("the two bare nodes cost nothing"), Purse.Charges.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPurseRefusesTest,
	"Airside.Present.BuildPurseRefuses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildPurseRefusesTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld World;
	PriceTheTaxiway(*World.Actor);

	FRecordingPurse Purse;
	Purse.Funds = 0.0;
	World.Actor->GetEditFacade()->SetPurse(&Purse);

	const int32 A = World.Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = World.Actor->PlaceNode(FVector2D(10000.0, 0.0));

	TestFalse(TEXT("a build nobody can pay for is refused"),
		World.Actor->ConnectNodes(A, B, ERoadKind::Taxiway, INDEX_NONE));
	TestEqual(TEXT("and nothing was charged for it"), Purse.Charges.Num(), 0);

	// THE WHOLE REASON THE REFUSAL HAPPENS BEFORE THE EDIT. FRoadEditScope's abandon discards
	// the undo snapshot; it does NOT roll the network back. A refusal at commit time would
	// leave the segment standing, unpaid for, with no undo step for it - and nothing on screen
	// would distinguish that from a build that worked.
	TestEqual(TEXT("no segment was left behind by the refusal"),
		World.Actor->GetNetwork()->GetSegments().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPurseNullBuildsFreeTest,
	"Airside.Present.BuildPurseNullBuildsFree",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildPurseNullBuildsFreeTest::RunTest(const FString& Parameters)
{
	// THE EDITOR MODE'S TEST, and it is not incidental. URoadBuildEdMode and every existing
	// tool test build with no purse at all, and a default that silently began charging would
	// break design-time building with nothing anywhere to say why.
	FAirsideTestWorld World;
	PriceTheTaxiway(*World.Actor);

	const int32 A = World.Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = World.Actor->PlaceNode(FVector2D(10000.0, 0.0));

	TestTrue(TEXT("with no purse, building is free and always allowed"),
		World.Actor->ConnectNodes(A, B, ERoadKind::Taxiway, INDEX_NONE));
	TestEqual(TEXT("and the segment really is there"),
		World.Actor->GetNetwork()->GetSegments().Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPurseUndoReversesTest,
	"Airside.Present.BuildPurseUndoReverses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildPurseUndoReversesTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld World;
	PriceTheTaxiway(*World.Actor);

	FRecordingPurse Purse;
	World.Actor->GetEditFacade()->SetPurse(&Purse);

	const int32 A = World.Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = World.Actor->PlaceNode(FVector2D(10000.0, 0.0));
	World.Actor->ConnectNodes(A, B, ERoadKind::Taxiway, INDEX_NONE);
	if (!TestEqual(TEXT("the build charged"), Purse.Charges.Num(), 1)) { return false; }

	TestTrue(TEXT("undo steps back"), World.Actor->GetEditFacade()->Undo());

	TestEqual(TEXT("undo reverses the charge the build actually made - BY ID, so it puts back "
		"exactly what was taken rather than a figure recomputed from geometry undo has just "
		"removed"), Purse.Reversed.Num(), 1);
	TestEqual(TEXT("and it is that build's own id"),
		Purse.Reversed.IsValidIndex(0) ? Purse.Reversed[0] : 0, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPurseRedoRechargesTest,
	"Airside.Present.BuildPurseRedoRecharges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildPurseRedoRechargesTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld World;
	PriceTheTaxiway(*World.Actor);

	FRecordingPurse Purse;
	World.Actor->GetEditFacade()->SetPurse(&Purse);

	const int32 A = World.Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = World.Actor->PlaceNode(FVector2D(10000.0, 0.0));
	World.Actor->ConnectNodes(A, B, ERoadKind::Taxiway, INDEX_NONE);
	World.Actor->GetEditFacade()->Undo();

	TestTrue(TEXT("redo steps forward"), World.Actor->GetEditFacade()->Redo());

	// WITHOUT THIS, UNDO IS A MONEY PRINTER: undo refunds the build, and a redo that re-applied
	// the edit for nothing would leave the player holding both the taxiway and its refund, over
	// and over.
	TestEqual(TEXT("redo charges for the rebuild rather than handing it back free"),
		Purse.Charges.Num(), 2);
	TestEqual(TEXT("and charges the same amount the original build did"),
		Purse.Charges.IsValidIndex(1) ? Purse.Charges[1] : 0.0, 30000.0, 1e-6);

	// And the SECOND undo must reverse the SECOND charge, not the first - the first names a
	// ledger entry that has already been reversed once.
	World.Actor->GetEditFacade()->Undo();
	TestEqual(TEXT("undoing again reverses the charge the redo made"),
		Purse.Reversed.Num(), 2);
	TestEqual(TEXT("which is the new id, not the already-reversed one"),
		Purse.Reversed.IsValidIndex(1) ? Purse.Reversed[1] : 0, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPurseDemolishCreditsTest,
	"Airside.Present.BuildPurseDemolishCredits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildPurseDemolishCreditsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld World;
	PriceTheTaxiway(*World.Actor);

	FRecordingPurse Purse;
	World.Actor->GetEditFacade()->SetPurse(&Purse);

	const int32 A = World.Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = World.Actor->PlaceNode(FVector2D(10000.0, 0.0));
	World.Actor->ConnectNodes(A, B, ERoadKind::Taxiway, INDEX_NONE);

	TestTrue(TEXT("the segment is deleted"), World.Actor->DeleteSegment(0));

	// CREDIT, NOT REVERSE. Demolition is a new transaction valuing the geometry at today's
	// price; it is not the undoing of the build. That asymmetry is what keeps a BuiltFor field
	// out of FRoadSegment and out of every save.
	TestEqual(TEXT("demolishing credits scrap value"), Purse.Credits.Num(), 1);
	TestEqual(TEXT("and does NOT reverse the original charge, which would be undo's job"),
		Purse.Reversed.Num(), 0);
	TestEqual(TEXT("the credit is quoted at today's price for what was torn out"),
		Purse.Credits.IsValidIndex(0) ? Purse.Credits[0] : 0.0, 30000.0, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPurseDragChargesTheDeltaTest,
	"Airside.Present.BuildPurseDragChargesTheDelta",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildPurseDragChargesTheDeltaTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld World;
	PriceTheTaxiway(*World.Actor);

	FRecordingPurse Purse;
	World.Actor->GetEditFacade()->SetPurse(&Purse);

	const int32 A = World.Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = World.Actor->PlaceNode(FVector2D(1000.0, 0.0));   // 10 m
	World.Actor->ConnectNodes(A, B, ERoadKind::Taxiway, INDEX_NONE);
	const int32 ChargesAfterBuild = Purse.Charges.Num();

	// THE HOLE THIS CLOSES: build ten metres, drag the end two hundred, and the extra pavement
	// is free - MoveNode creates no segment, so nothing else in the facade charges for it.
	URoadEditFacade* Facade = World.Actor->GetEditFacade();
	Facade->BeginInteractiveEdit(TEXT("drag node"));
	World.Actor->MoveNode(B, FVector2D(20000.0, 0.0));               // 200 m
	Facade->EndInteractiveEdit(true);

	if (!TestEqual(TEXT("the drag charged once for what it added"),
		Purse.Charges.Num(), ChargesAfterBuild + 1)) { return false; }
	TestEqual(TEXT("and charged for the 190 m of NEW pavement, not for the whole 200"),
		Purse.Charges.Last(), 190.0 * 300.0, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPurseDragRevertsWhenBrokeTest,
	"Airside.Present.BuildPurseDragRevertsWhenBroke",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildPurseDragRevertsWhenBrokeTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld World;
	PriceTheTaxiway(*World.Actor);

	FRecordingPurse Purse;
	World.Actor->GetEditFacade()->SetPurse(&Purse);

	const int32 A = World.Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = World.Actor->PlaceNode(FVector2D(1000.0, 0.0));
	World.Actor->ConnectNodes(A, B, ERoadKind::Taxiway, INDEX_NONE);

	// Nothing left for the drag to spend.
	Purse.Funds = 0.0;
	const int32 ChargesBefore = Purse.Charges.Num();

	URoadEditFacade* Facade = World.Actor->GetEditFacade();
	Facade->BeginInteractiveEdit(TEXT("drag node"));
	World.Actor->MoveNode(B, FVector2D(20000.0, 0.0));
	Facade->EndInteractiveEdit(true);

	TestEqual(TEXT("a drag nobody can pay for charges nothing"), Purse.Charges.Num(), ChargesBefore);

	// REVERTED, NOT MERELY REFUSED, and this is the assertion that matters. The node moved on
	// every frame of the drag, so dropping the undo snapshot would have left the longer taxiway
	// standing and unpaid for - which is exactly the free pavement the charge exists to stop.
	const URoadNetwork* Network = World.Actor->GetNetwork();
	if (!TestTrue(TEXT("the node still exists"), Network->GetNodes().IsValidIndex(B)))
	{
		return false;
	}
	TestEqual(TEXT("and it is back where the drag started, not left out at 200 m"),
		Network->GetNodes()[B].Position.X, 1000.0, 1e-6);
	return true;
}

namespace
{
	/** Records the labels a tool asked to have drawn, and with which style. */
	struct FPricingSink : public IToolPreviewSink
	{
		TArray<FString> Labels;
		TArray<EPreviewStyle> Styles;

		virtual void Marker(const FVector2D&, EPreviewStyle) override {}
		virtual void Line(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void Label(const FVector2D&, const FString& Text, EPreviewStyle Style) override
		{
			Labels.Add(Text);
			Styles.Add(Style);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGhostPricesTheRoadTest,
	"Airside.Tool.GhostPricesTheRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGhostPricesTheRoadTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld World;
	PriceTheTaxiway(*World.Actor);

	FRecordingPurse Purse;
	World.Actor->GetEditFacade()->SetPurse(&Purse);

	const int32 A = World.Actor->PlaceNode(FVector2D(0.0, 0.0));

	// A Node snap needs its HANDLE filled in as well as its kind - see TestTool::ContextAt,
	// which sets only what every snap kind has in common. Without the handle the first click
	// starts no chain and the tool stays idle, which draws no preview at all.
	FToolContext Start = TestTool::ContextAt(*World.Actor, FVector2D(0.0, 0.0), ERoadSnapKind::Node);
	Start.Snap.Node.Index = A;
	Start.Snap.Node.Generation = World.Actor->GetNetwork()->GetNodes()[A].Generation;
	Start.Limits.MinSegmentLength = 250.0;
	Start.Limits.MinTurnDegrees = 25.0;

	FRoadDrawTool Tool(ERoadKind::Taxiway);
	Tool.OnClick(Start);

	FToolContext Context = TestTool::ContextAt(*World.Actor, FVector2D(10000.0, 0.0));
	Context.Limits.MinSegmentLength = 250.0;
	Context.Limits.MinTurnDegrees = 25.0;

	FPricingSink Affordable;
	Tool.BuildPreview(Context, Affordable);

	// THE PRICE BEFORE THE CLICK, and through Label rather than a new sink message: Label
	// already exists, so the sink's contract - intent in road-plane coordinates naming a
	// MEANING - is untouched.
	TestTrue(TEXT("the ghost prices what the click would lay"), Affordable.Labels.Num() > 0);
	TestTrue(TEXT("and while it is affordable the label is Pending, not Refused"),
		Affordable.Styles.Contains(EPreviewStyle::Pending));

	// EPreviewStyle::Refused is already defined as "something the gesture cannot do, with the
	// reason", which is exactly what a road the player cannot pay for is.
	Purse.Funds = 0.0;
	FPricingSink Broke;
	Tool.BuildPreview(Context, Broke);
	TestTrue(TEXT("a road nobody can pay for is drawn Refused, so the player sees the refusal "
		"before the click rather than after it"),
		Broke.Styles.Contains(EPreviewStyle::Refused));
	return true;
}

namespace
{
	/** A 12 m x 12 m plot, wound clockwise, with the road-facing edge stated in that same
	 *  winding order - the exact fixture PlotPlacementTest's
	 *  FPlotOutlineIsAlwaysCounterClockwiseTest already proves places a depot successfully. */
	struct FPricedDepotPlot
	{
		TArray<FVector2D> Outline = {
			FVector2D(0.0, 0.0), FVector2D(0.0, 1200.0),
			FVector2D(1200.0, 1200.0), FVector2D(1200.0, 0.0) };
		FVector2D FrontageA = FVector2D(1200.0, 0.0);
		FVector2D FrontageB = FVector2D(0.0, 0.0);
	};
}

/**
 * Issue #193: PlaceEntityInPlot ended in CommitAndNotify - no quote, no CanAfford, no charge -
 * while PlaceEntity charges BuildCost::ForEntity for the identical UEntityDefinition. A plotted
 * depot was free; a plopped stand was not. This is FBuildPurseChargesTest's own shape, aimed at
 * the plot path instead of ConnectNodes.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPurseChargesForPlottedDepotTest,
	"Airside.Present.BuildPurseChargesForPlottedDepot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildPurseChargesForPlottedDepotTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld World;
	ARoadNetworkActor* Actor = World.Actor;
	Actor->ClearNetwork();

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }
	// A NON-ZERO PLACEMENT COST - MakeFuelDepotTransient's own default is 0.0, which would
	// make "the entity is priced" indistinguishable from "the entity is free".
	Depot->PlacementCost = 5000.0;
	Actor->FuelDepotDefinition = Depot;

	FRecordingPurse Purse;
	Actor->GetEditFacade()->SetPurse(&Purse);

	const FPricedDepotPlot Plot;
	IRoadEditTarget* Target = Actor;
	const int32 Placed = Target->PlaceEntityInPlot(Plot.Outline, Plot.FrontageA, Plot.FrontageB,
		{ EDepotModule::Shed }, EPlaceableEntity::FuelDepot);
	if (!TestTrue(TEXT("the depot is placed"), Placed != INDEX_NONE)) { return false; }

	if (!TestEqual(TEXT("placing a plotted depot charges exactly once"), Purse.Charges.Num(), 1))
	{
		return false;
	}

	// 5000 FOR THE ENTITY (Depot->PlacementCost) PLUS THE PAD: a 12 m x 12 m plot, 144 m2 at
	// UAirsideSettings::ApronCostPerSquareMetre's default of 15/m2 - the same rate AddApron
	// charges, summed rather than invented, exactly as the issue asked.
	TestEqual(TEXT("charged the entity's placement cost plus the pad's apron rate x area - not "
		"free, and not the entity alone"),
		Purse.Charges[0], 5000.0 + 144.0 * 15.0, 1e-6);

	return true;
}

/**
 * Issue #193, the other half: a plot nobody can afford must place nothing, exactly as
 * PlaceEntity already refuses - not build the depot and then discover it should not have.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPurseRefusesPlottedDepotTest,
	"Airside.Present.BuildPurseRefusesPlottedDepot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildPurseRefusesPlottedDepotTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld World;
	ARoadNetworkActor* Actor = World.Actor;
	Actor->ClearNetwork();

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }
	Depot->PlacementCost = 5000.0;
	Actor->FuelDepotDefinition = Depot;

	FRecordingPurse Purse;
	Purse.Funds = 0.0;
	Actor->GetEditFacade()->SetPurse(&Purse);

	const FPricedDepotPlot Plot;
	IRoadEditTarget* Target = Actor;
	const int32 Placed = Target->PlaceEntityInPlot(Plot.Outline, Plot.FrontageA, Plot.FrontageB,
		{ EDepotModule::Shed }, EPlaceableEntity::FuelDepot);

	TestEqual(TEXT("a plot nobody can pay for is refused"), Placed, INDEX_NONE);
	TestEqual(TEXT("and nothing was charged for it"), Purse.Charges.Num(), 0);
	TestEqual(TEXT("no entity was left behind by the refusal"),
		Actor->GetNetwork()->GetEntities().Num(), 0);
	return true;
}

/**
 * Issue #193, the refund half: undo on a plotted depot must reverse the SAME charge id
 * CommitPurchase recorded, through the identical pending-charge path PlaceEntity's own undo
 * uses - not silently leave the money spent, which CommitAndNotify's plain path would have.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPurseUndoReversesPlottedDepotTest,
	"Airside.Present.BuildPurseUndoReversesPlottedDepot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildPurseUndoReversesPlottedDepotTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld World;
	ARoadNetworkActor* Actor = World.Actor;
	Actor->ClearNetwork();

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }
	Depot->PlacementCost = 5000.0;
	Actor->FuelDepotDefinition = Depot;

	FRecordingPurse Purse;
	Actor->GetEditFacade()->SetPurse(&Purse);

	const FPricedDepotPlot Plot;
	IRoadEditTarget* Target = Actor;
	const int32 Placed = Target->PlaceEntityInPlot(Plot.Outline, Plot.FrontageA, Plot.FrontageB,
		{ EDepotModule::Shed }, EPlaceableEntity::FuelDepot);
	if (!TestTrue(TEXT("the depot is placed"), Placed != INDEX_NONE)) { return false; }
	if (!TestEqual(TEXT("the plot charged"), Purse.Charges.Num(), 1)) { return false; }

	TestTrue(TEXT("undo steps back"), Actor->GetEditFacade()->Undo());

	TestEqual(TEXT("undo reverses the plot's own charge id - by id, the same pending-charge "
		"path PlaceEntity's own undo uses"), Purse.Reversed.Num(), 1);
	TestEqual(TEXT("and it is that build's own id"),
		Purse.Reversed.IsValidIndex(0) ? Purse.Reversed[0] : 0, 1);
	TestEqual(TEXT("and the depot itself is gone again"),
		Actor->GetNetwork()->GetEntities().ContainsByPredicate(
			[](const FEntityInstance& E) { return E.bAlive; }), false);
	return true;
}

#endif
