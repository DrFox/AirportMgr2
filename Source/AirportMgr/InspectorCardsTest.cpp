#include "CoreMinimal.h"
#include "ArrivalViewModels.h"
#include "BuildActions.h"
#include "Components/TextBlock.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "InspectorCards.h"
#include "InspectorFacilityRows.h"
#include "InspectorWidget.h"
#include "RoadBuildController.h"
#include "Solve/GuidelineGeom.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/InspectFacts.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/OpsDefinition.h"
#include "Model/OpsAlerts.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/TaxiwayStrip.h"
#include "Model/TrafficOccupancy.h"
#include "Present/AirsideTraffic.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestGraph.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/Selection.h"
#include "UI/UiButton.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE INSPECTOR'S CARDS (issue #441), at the composition: one test per card - a real card fed real facts, producing the expected
 * view - the table that holds them, the two properties that make the gate safe (a memo never says what a fresh describe would not),
 * and the seams the split introduced (the table the widget asks, the purchase rows the widget builds).
 */
namespace InspectorCardsTest
{
	/** A world, the actor, FTestAirport laid onto the actor's own network (one runway, one taxiway, one stand). */
	struct FCardRig
	{
		FAirsideTestWorld TestWorld;
		ARoadNetworkActor* Actor = nullptr;
		URoadNetwork* Net = nullptr;
		FTestAirport Field;

		bool Ok() const { return Actor != nullptr && Net != nullptr && Field.Stands.Num() > 0; }

		FCardRig()
		{
			Actor = TestWorld.Actor;
			if (Actor == nullptr)
			{
				return;
			}
			Actor->PlaceNode(FVector2D(-300000.0, -300000.0));   // a network, for the fixture to build onto
			Net = Actor->Network;
			Field = FTestAirport::Build(UAirsideSettings::ResolveDefaultAirframe(), FTestAirportOptions(), Net);
		}

		int32 SegmentWhere(TFunctionRef<bool(FRoadSegmentId)> Pred) const
		{
			for (int32 Index = 0; Index < Net->GetSegments().Num(); ++Index)
			{
				const FRoadSegmentId Id = Net->SegmentIdAt(Index);
				if (Id.IsSet() && Pred(Id))
				{
					return Index;
				}
			}
			return INDEX_NONE;
		}
		int32 RunwaySegment() const { return SegmentWhere([this](FRoadSegmentId Id) { return Net->IsRunwaySegment(Id); }); }
		int32 TaxiwaySegment() const { return SegmentWhere([this](FRoadSegmentId Id) { return TaxiwayStrip::HasStrip(*Net, Id); }); }

		static FSelection Select(ESelectionKind Kind, int32 Id) { FSelection S; S.Kind = Kind; S.Id = Id; return S; }
		FInspectorCardInput Input(const FSelection& Selection) const
		{
			FInspectorCardInput In;
			In.Target = Actor;
			In.Selection = Selection;
			return In;
		}
		FGuidelineNodeId StandPose() const { return Net->GetEntity(Field.Stands[0])->PoseNode; }
		UGroundTraffic& Traffic() const { return *Actor->GetTraffic()->GetModel(); }
	};

	/** The depot rig the widget's own depot tests use (an attached runtime, the player's plotted depot with a shed and room for a
	 *  second), without the controller - a card is asked with the runtime in its input. */
	struct FCardDepotRig
	{
		FAirsideTestWorld World;
		UOpsRuntime* Runtime = nullptr;
		FEntityInstanceId Depot;
		int32 Index = INDEX_NONE;

		bool Build()
		{
			ARoadNetworkActor* Actor = World.Actor;
			if (Actor == nullptr) { return false; }
			Actor->PlaceNode(FVector2D(0.0, 40000.0));
			Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
			Runtime = NewObject<UOpsRuntime>();
			Runtime->Attach(Actor);
			const TArray<FVector2D> Plot = { FVector2D(0.0, 0.0), FVector2D(5000.0, 0.0), FVector2D(5000.0, 2400.0), FVector2D(0.0, 2400.0) };
			Index = Actor->PlaceEntityInPlot(Plot, Plot[0], Plot[1],
				{ EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump }, EPlaceableEntity::FuelDepot);
			if (Index == INDEX_NONE) { return false; }
			Depot = Actor->Network->EntityIdAt(Index);
			return true;
		}

		FInspectorCardInput Input() const
		{
			FInspectorCardInput In;
			In.Runtime = Runtime;
			In.Target = World.Actor;
			In.Selection.Kind = ESelectionKind::Stand;
			In.Selection.Id = Index;
			return In;
		}

		/** Game seconds forward, in real steps the clock turns into game time. */
		void AdvanceGame(double Seconds)
		{
			USimClock& Clock = *Runtime->GetClock();
			const double Until = Clock.Now() + Seconds;
			for (int32 Step = 0; Step < 100000 && Clock.Now() < Until; ++Step) { Clock.Advance(0.5 / FMath::Max(Clock.TimeScale(), 1e-6)); }
		}
	};

	/** Whether two views say the same thing - every field a card fills, the quote at the resolution its rows render. Why names the
	 *  first difference. */
	bool ViewsMatch(const FInspectorCardView& A, const FInspectorCardView& B, FString& Why)
	{
		auto Differ = [&Why](const TCHAR* What, const FString& Kept, const FString& Fresh)
		{
			Why = FString::Printf(TEXT("%s: kept '%s', fresh '%s'"), What, *Kept, *Fresh);
			return false;
		};
		if (A.Card != B.Card) { return Differ(TEXT("card"), FString::FromInt(static_cast<int32>(A.Card)), FString::FromInt(static_cast<int32>(B.Card))); }
		if (A.Title != B.Title) { return Differ(TEXT("title"), A.Title, B.Title); }
		if (A.Facts != B.Facts) { return Differ(TEXT("facts"), A.Facts, B.Facts); }
		if (A.Status != B.Status) { return Differ(TEXT("status"), A.Status, B.Status); }
		if (A.Deadlock != B.Deadlock) { return Differ(TEXT("deadlock"), A.Deadlock, B.Deadlock); }
		if (A.Verbs != B.Verbs) { return Differ(TEXT("verbs"), FString::FromInt(static_cast<int32>(A.Verbs)), FString::FromInt(static_cast<int32>(B.Verbs))); }
		if (A.bCanDepart != B.bCanDepart) { return Differ(TEXT("can depart"), A.bCanDepart ? TEXT("yes") : TEXT("no"), B.bCanDepart ? TEXT("yes") : TEXT("no")); }
		if (A.WaitedForId != B.WaitedForId) { return Differ(TEXT("waited for"), FString::FromInt(A.WaitedForId), FString::FromInt(B.WaitedForId)); }
		if (!A.WaitingForCaption.EqualTo(B.WaitingForCaption)) { return Differ(TEXT("show caption"), A.WaitingForCaption.ToString(), B.WaitingForCaption.ToString()); }
		if (!A.RunwayCaption.EqualTo(B.RunwayCaption)) { return Differ(TEXT("runway caption"), A.RunwayCaption.ToString(), B.RunwayCaption.ToString()); }
		if (!A.RunwayUseCaption.EqualTo(B.RunwayUseCaption)) { return Differ(TEXT("runway mode caption"), A.RunwayUseCaption.ToString(), B.RunwayUseCaption.ToString()); }
		const FFacilityQuote& QA = A.Quote;
		const FFacilityQuote& QB = B.Quote;
		if (QA.IsFacility() != QB.IsFacility() || QA.Vehicles != QB.Vehicles || QA.Bays != QB.Bays
			|| QA.Modules.Num() != QB.Modules.Num() || QA.VehicleOffers.Num() != QB.VehicleOffers.Num() || QA.Fleet.Num() != QB.Fleet.Num())
		{
			return Differ(TEXT("quote shape"), FString::Printf(TEXT("%d/%d bays, %d modules, %d offers, %d fleet"), QA.Vehicles, QA.Bays, QA.Modules.Num(), QA.VehicleOffers.Num(), QA.Fleet.Num()),
				FString::Printf(TEXT("%d/%d bays, %d modules, %d offers, %d fleet"), QB.Vehicles, QB.Bays, QB.Modules.Num(), QB.VehicleOffers.Num(), QB.Fleet.Num()));
		}
		for (int32 Each = 0; Each < QA.Modules.Num(); ++Each)
		{
			if (QA.Modules[Each].Refusal != QB.Modules[Each].Refusal || QA.Modules[Each].Owned != QB.Modules[Each].Owned
				|| QA.Modules[Each].Reserved != QB.Modules[Each].Reserved || !QA.Modules[Each].Label.EqualTo(QB.Modules[Each].Label))
			{
				return Differ(TEXT("shed row"), QA.Modules[Each].Label.ToString(), QB.Modules[Each].Label.ToString());
			}
		}
		for (int32 Each = 0; Each < QA.VehicleOffers.Num(); ++Each)
		{
			if (QA.VehicleOffers[Each].Refusal != QB.VehicleOffers[Each].Refusal || !QA.VehicleOffers[Each].Label.EqualTo(QB.VehicleOffers[Each].Label))
			{
				return Differ(TEXT("vehicle offer"), QA.VehicleOffers[Each].Label.ToString(), QB.VehicleOffers[Each].Label.ToString());
			}
		}
		for (int32 Each = 0; Each < QA.Fleet.Num(); ++Each)
		{
			if (QA.Fleet[Each].Refusal != QB.Fleet[Each].Refusal || QA.Fleet[Each].Line != QB.Fleet[Each].Line || !QA.Fleet[Each].SellLabel.EqualTo(QB.Fleet[Each].SellLabel))
			{
				return Differ(TEXT("fleet row"), QA.Fleet[Each].Line, QB.Fleet[Each].Line);
			}
		}
		return true;
	}

	bool Shown(const UWidget* Widget) { return Widget != nullptr && Widget->GetVisibility() == ESlateVisibility::Visible; }
}

// --- One test per card --------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorAircraftCardTest, "AirportMgr.Inspector.Card.Aircraft",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorAircraftCardTest::RunTest(const FString&)
{
	// FACTS IN, VIEW OUT: the composition of the aircraft card with no widget, no traffic and no runtime - what it says of a
	// taxiing aircraft, of one held behind another in a ring, and of one a flight owns.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	FAircraftCard Card;
	FAgentFacts F;
	F.Id = 7;
	F.TypeName = TEXT("SR22");
	F.HeadingDegrees = 90.0;
	F.GroundSpeed = 515.0;      // 5.15 m/s, 10.01 kt
	F.Altitude = 1250.0;        // 12.5 m: the half that printf and FMath::RoundToInt once read differently
	F.Destination = TEXT("Stand 3");
	F.Status = TEXT("Taxiing");
	F.bEngineRunning = true;
	FInspectorCardInput In;
	In.Target = TestWorld.Actor;
	In.Selection.Kind = ESelectionKind::Aircraft;
	In.Selection.Id = 7;
	In.PrecomputedAgentFacts = &F;

	const FInspectorCardView* View = Card.Describe(In);
	if (!TestNotNull(TEXT("a view"), View)) { return false; }
	TestEqual(TEXT("it is the aircraft card's"), View->Card, EInspectorCard::Aircraft);
	TestEqual(TEXT("no registration: the type and the id title it"), View->Title, FString(TEXT("SR22  #7")));
	TestEqual(TEXT("the figures as printed"), View->Facts,
		FString(TEXT("Heading 090\nSpeed 5.2 m/s (10 kt)\nVertical speed 0.0 m/s (0 ft/min)\nAltitude 13 m\nTo Stand 3\nEngine running")));
	// DESCENDING: the sign survives, and the tenths are not "-3.-2" (speed's "%d.%d" applied to a negative).
	F.VerticalSpeed = -320.0;   // -3.2 m/s, -629.9 ft/min
	View = Card.Describe(In);
	if (!TestNotNull(TEXT("a view, descending"), View)) { return false; }
	TestTrue(TEXT("a descent prints signed in m/s and ft/min"), View->Facts.Contains(TEXT("\nVertical speed -3.2 m/s (-630 ft/min)\n")));
	F.VerticalSpeed = 0.0;
	TestEqual(TEXT("the status as the model said it"), View->Status, FString(TEXT("Taxiing")));
	TestEqual(TEXT("no ring: no deadlock line"), View->Deadlock, FString());
	TestTrue(TEXT("the agent verbs, and only those"), View->Verbs == (EInspectorVerbs::Depart | EInspectorVerbs::Follow | EInspectorVerbs::Unstick));
	TestFalse(TEXT("Depart is not lit while it taxis"), View->bCanDepart);
	TestEqual(TEXT("it waits for nobody"), View->WaitedForId, 0);

	// HELD BEHIND ANOTHER, in a ring with it: the hold line names the blocker, the ring names the partner, Show follows.
	F.bCanDepart = true;
	F.Status = TEXT("Waiting behind aircraft 9");
	F.bStatusIsHold = true;
	F.Hold.WaitingOn = 9;
	F.Hold.At = EHoldAt::Behind;
	F.DeadlockedWith = { 9 };
	View = Card.Describe(In);
	if (!TestNotNull(TEXT("a view, held"), View)) { return false; }
	TestEqual(TEXT("the hold line, with the blocker named as the card names an agent no flight owns"),
		View->Status, FString(TEXT("Waiting behind aircraft 9")));
	TestTrue(TEXT("the ring, in the alert's words for the fix"), View->Deadlock.StartsWith(TEXT("Deadlocked with aircraft 9"))
		&& View->Deadlock.Contains(UOpsAlerts::DeadlockRemedy().ToString()));
	TestTrue(TEXT("Show is offered"), EnumHasAllFlags(View->Verbs, EInspectorVerbs::WaitingFor));
	TestEqual(TEXT("for the one it waits on"), View->WaitedForId, 9);
	TestEqual(TEXT("captioned with its name"), View->WaitingForCaption.ToString(), FString(TEXT("Show aircraft 9")));
	TestTrue(TEXT("Depart lights with the facts that say it may"), View->bCanDepart);

	// A FLIGHT OWNS IT: the registration leads the title, and the blocker is named by its own flight.
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	UFlightBoard* Board = NewObject<UFlightBoard>(GetTransientPackage());
	for (const TPair<int32, const TCHAR*>& Owned : { TPair<int32, const TCHAR*>(7, TEXT("G-SVBT")), TPair<int32, const TCHAR*>(9, TEXT("G-HDVK")) })
	{
		UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
		Flight->AgentId = Owned.Key;
		Flight->Callsign = Owned.Value;
		Flight->AirlineName = FText::FromString(TEXT("Flying Club"));
		Flight->SetPhaseForTest(EFlightPhase::TaxiIn);
		Board->AddOffer(*Clock, Flight);
	}
	In.Flights = Board;
	View = Card.Describe(In);
	if (!TestNotNull(TEXT("a view, with flights"), View)) { return false; }
	TestEqual(TEXT("the registration first, then type and airline"), View->Title, FString(TEXT("G-SVBT · SR22 · Flying Club")));
	TestEqual(TEXT("the blocker by registration"), View->WaitingForCaption.ToString(), FString(TEXT("Show G-HDVK")));
	TestTrue(TEXT("and the hold line says so"), View->Status.Contains(TEXT("G-HDVK")));

	// THE STALL'S CONVERSION is the clock's (#447: it was a public static on the widget, then on the card, and the job board's bid wrote it again).
	Clock->SetUniformDay(1200.0);   // 72 game seconds a real second, the day HoldAndDeadlockLines reads 80 s as 1 h 36 min in
	TestEqual(TEXT("80 movement seconds at a 1200 s day are 96 game minutes"), Clock->GameSecondsOfMovement(80.0), 5760.0);

	// A GONE AGENT has no card: nothing precomputed, and none in the model.
	In.PrecomputedAgentFacts = nullptr;
	In.Selection.Id = 12345;
	TestNull(TEXT("an agent that is not there shows nothing"), Card.Describe(In));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorVehicleCardNamesTheVehicleTest, "AirportMgr.Inspector.Card.ServiceVehicleTitleNamesTheVehicle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorVehicleCardNamesTheVehicleTest::RunTest(const FString&)
{
	// #478 ITEM 1: a selected bowser read "FUEL  #37" - its type CODE and its AGENT id - while its fuel line said "Bowser" and the
	// depot card showed the VEHICLE id. The title, and the name of any agent a hold or a ring names, go through
	// FServiceFleet::NameOf with the vehicle's own id: "Bowser #2".
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>(GetTransientPackage());
	UJobBoard& Jobs = *Runtime->GetJobBoard();
	// THE CATALOGUE, as Attach resolves it (#430): the runtime here is never attached, and the kind's name is the catalogue row's.
	UOpsRuntime::ResolveVehicleCatalogue(Jobs, *GetDefault<UScenario>());
	// A SECOND VEHICLE FIRST, so the bowser's id is 2 and its agent's is 37: a title that printed the agent id, or the first id, cannot pass.
	Jobs.AddVehicleForTest(TEXT("UTILITY"), FEntityInstanceId(), EServiceVehicleState::Idle, 0.0);
	FServiceVehicle& Bowser = Jobs.AddVehicleForTest(TEXT("FUEL"), FEntityInstanceId(), EServiceVehicleState::Idle, 9700.0);
	constexpr int32 BowserAgent = 37;
	Bowser.AgentId = BowserAgent;
	const FString Kind = FServiceFleet::NameOf(Jobs, TEXT("FUEL")).ToString();
	if (!TestNotEqual(TEXT("setup: the kind is named, not coded - or the title below could pass printing the code"), Kind, FString(TEXT("FUEL")))) { return false; }
	if (!TestNotEqual(TEXT("setup: the vehicle's id is not its agent's"), Bowser.Id, BowserAgent)) { return false; }
	const FString Expected = FString::Printf(TEXT("%s #%d"), *Kind, Bowser.Id);

	// THE BOWSER ITSELF, selected: its agent is the selection (an agent is an aircraft-kind selection, whatever it is).
	FAircraftCard Card;
	FAgentFacts F;
	F.Id = BowserAgent;
	F.TypeName = TEXT("FUEL");
	F.Status = TEXT("Taxiing");
	FInspectorCardInput In;
	In.Runtime = Runtime;
	In.Target = TestWorld.Actor;
	In.Selection.Kind = ESelectionKind::Aircraft;
	In.Selection.Id = BowserAgent;
	In.PrecomputedAgentFacts = &F;
	const FInspectorCardView* View = Card.Describe(In);
	if (!TestNotNull(TEXT("a view for the bowser"), View)) { return false; }
	TestEqual(TEXT("the bowser's title is its kind's name and its VEHICLE id"), View->Title, Expected);

	// AN AGENT THE JOB BOARD KNOWS NO VEHICLE FOR keeps the id title - the fallback is for what has no vehicle, not for a bowser.
	F.Id = 12;
	In.Selection.Id = 12;
	View = Card.Describe(In);
	if (!TestNotNull(TEXT("a view for an agent with no vehicle"), View)) { return false; }
	TestEqual(TEXT("no vehicle: the code and the agent id, as before"), View->Title, FString(TEXT("FUEL  #12")));

	// A HOLD BEHIND THE BOWSER, and a ring with it: both name it as the card titles it, never by agent id.
	F.Id = 7;
	F.TypeName = TEXT("SR22");
	F.Status = TEXT("Waiting behind aircraft 37");
	F.bStatusIsHold = true;
	F.Hold.WaitingOn = BowserAgent;
	F.Hold.At = EHoldAt::Behind;
	F.DeadlockedWith = { BowserAgent };
	In.Selection.Id = 7;
	View = Card.Describe(In);
	if (!TestNotNull(TEXT("a view, held behind the bowser"), View)) { return false; }
	TestTrue(FString::Printf(TEXT("the hold line names the vehicle ('%s')"), *View->Status), View->Status.Contains(Expected));
	TestTrue(FString::Printf(TEXT("the ring names it too ('%s')"), *View->Deadlock), View->Deadlock.StartsWith(FString::Printf(TEXT("Deadlocked with %s"), *Expected)));
	TestEqual(TEXT("and Show is captioned with it"), View->WaitingForCaption.ToString(), FString::Printf(TEXT("Show %s"), *Expected));
	TestEqual(TEXT("Show still selects the AGENT - the card selects agents, the name is for reading"), View->WaitedForId, BowserAgent);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorRunwayCardTest, "AirportMgr.Inspector.Card.Runway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorRunwayCardTest::RunTest(const FString&)
{
	using namespace InspectorCardsTest;
	FCardRig Rig;
	if (!TestTrue(TEXT("the rig"), Rig.Ok())) { return false; }
	const int32 Segment = Rig.RunwaySegment();
	FRunwayCard Card;
	const FInspectorCardInput In = Rig.Input(FCardRig::Select(ESelectionKind::Runway, Segment));
	const FInspectorCardView* View = Card.Describe(In);
	if (!TestNotNull(TEXT("a view"), View)) { return false; }
	TestEqual(TEXT("it is the runway card's"), View->Card, EInspectorCard::Runway);
	TestTrue(FString::Printf(TEXT("titled by its pair ('%s')"), *View->Title), View->Title.StartsWith(TEXT("Runway ")) && View->Title.Contains(TEXT("/")));
	TestTrue(FString::Printf(TEXT("what it takes, and its length ('%s')"), *View->Facts),
		View->Facts.Contains(TEXT("In use: ")) && View->Facts.Contains(TEXT("Takes: mixed")) && View->Facts.Contains(TEXT(" m long")));
	TestTrue(FString::Printf(TEXT("the status says which way ('%s')"), *View->Status), View->Status.StartsWith(TEXT("Landing and taking off ")));
	TestTrue(TEXT("the two runway verbs, and only those"), View->Verbs == (EInspectorVerbs::Runway | EInspectorVerbs::RunwayUse));
	TestTrue(FString::Printf(TEXT("the flip names the other end ('%s')"), *View->RunwayCaption.ToString()),
		View->RunwayCaption.ToString().StartsWith(TEXT("Use ")) && View->RunwayCaption.ToString().Len() == 6);
	TestEqual(TEXT("the mode caption is the CURRENT mode"), View->RunwayUseCaption.ToString(), FString(TEXT("Mixed ops")));
	TestEqual(TEXT("a runway has no deadlock line"), View->Deadlock, FString());
	TestFalse(TEXT("and no facility to quote"), View->Quote.IsFacility());

	// THROUGH THE FACADE, as the player sets it: the card says the new mode - the gate saw the guideline rebuild.
	FRunwayFacts Facts = Rig.Net->RunwayFactsFor(Rig.Net->SegmentIdAt(Segment));
	Facts.Use = ERunwayUse::ArrivalsOnly;
	if (!TestTrue(TEXT("the facade takes the facts"), Rig.Actor->SetRunwayFacts(Segment, Facts))) { return false; }
	View = Card.Describe(In);
	if (!TestNotNull(TEXT("a view, after"), View)) { return false; }
	TestEqual(TEXT("the mode caption follows"), View->RunwayUseCaption.ToString(), FString(TEXT("Arrivals only")));
	TestTrue(TEXT("and the facts line"), View->Facts.Contains(TEXT("Takes: arrivals only")));

	TestNull(TEXT("a segment that is no runway shows nothing"),
		Card.Describe(Rig.Input(FCardRig::Select(ESelectionKind::Runway, Rig.TaxiwaySegment()))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorTaxiwayCardTest2, "AirportMgr.Inspector.Card.Taxiway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorTaxiwayCardTest2::RunTest(const FString&)
{
	using namespace InspectorCardsTest;
	FCardRig Rig;
	if (!TestTrue(TEXT("the rig"), Rig.Ok())) { return false; }
	const int32 Segment = Rig.TaxiwaySegment();
	FTaxiwayCard Card;
	const FInspectorCardView* View = Card.Describe(Rig.Input(FCardRig::Select(ESelectionKind::Taxiway, Segment)));
	if (!TestNotNull(TEXT("a view"), View)) { return false; }
	TestEqual(TEXT("it is the taxiway card's"), View->Card, EInspectorCard::Taxiway);
	TestEqual(TEXT("titled by its segment"), View->Title, FString::Printf(TEXT("Taxiway %d"), Segment));
	TestTrue(FString::Printf(TEXT("its letter, strip and span ('%s')"), *View->Facts),
		View->Facts.StartsWith(TEXT("Code ")) && View->Facts.Contains(TEXT("Strip ")) && View->Facts.Contains(TEXT("Max span ")));
	TestEqual(TEXT("open to its letter"), View->Status, FString(TEXT("Open to its letter")));
	TestTrue(TEXT("no verbs: a taxiway is read, not driven"), View->Verbs == EInspectorVerbs::None);
	TestFalse(TEXT("and no facility to quote"), View->Quote.IsFacility());
	TestNull(TEXT("a segment that is no taxiway shows nothing"),
		Card.Describe(Rig.Input(FCardRig::Select(ESelectionKind::Taxiway, Rig.RunwaySegment()))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorStandCardTest, "AirportMgr.Inspector.Card.Stand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorStandCardTest::RunTest(const FString&)
{
	using namespace InspectorCardsTest;
	FCardRig Rig;
	if (!TestTrue(TEXT("the rig"), Rig.Ok())) { return false; }
	const FInspectorCardInput In = Rig.Input(FCardRig::Select(ESelectionKind::Stand, Rig.Field.Stands[0].Index));
	FStandCard Card;
	const FInspectorCardView* View = Card.Describe(In);
	if (!TestNotNull(TEXT("a view"), View)) { return false; }
	TestEqual(TEXT("it is the stand card's"), View->Card, EInspectorCard::Stand);
	TestTrue(FString::Printf(TEXT("titled by its number ('%s')"), *View->Title), View->Title.StartsWith(TEXT("Stand ")));
	TestTrue(FString::Printf(TEXT("its code, anchors and reachability ('%s')"), *View->Facts),
		View->Facts.StartsWith(TEXT("Code ")) && View->Facts.Contains(TEXT("service anchors")));
	TestEqual(TEXT("empty"), View->Status, FString(TEXT("Empty")));
	TestTrue(TEXT("no verbs"), View->Verbs == EInspectorVerbs::None);
	TestFalse(TEXT("and no facility to quote"), View->Quote.IsFacility());

	Rig.Traffic().HoldStand(-7, Rig.StandPose());
	View = Card.Describe(In);
	if (!TestNotNull(TEXT("a view, held"), View)) { return false; }
	TestTrue(FString::Printf(TEXT("a hold is a reservation ('%s')"), *View->Status), View->Status.StartsWith(TEXT("Reserved for aircraft #")));

	TestNull(TEXT("an entity that is not there shows nothing"),
		Card.Describe(Rig.Input(FCardRig::Select(ESelectionKind::Stand, 9999))));
	return true;
}

/**
 * THE DEPOT CARD NAMES THE DEPOT BY ITS NUMBER, NOT ITS ENTITY INDEX (#490).
 *
 * The title printed the INDEX, which RoadSlot recycles into a different depot after a bulldoze - the card of a depot built after one was
 * bulldozed said "Fuel depot 1" beside a player's memory of the depot that was gone. The issue's own pin: depots 1 and 2, bulldoze 1, build another;
 * the third takes the first's slot, and its card says depot 3 (the same number the job board's vehicle lines say - AirportOps.Model.DepotLabel.*).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorDepotCardNamesTheDepotByItsNumberTest, "AirportMgr.Inspector.DepotCardNamesTheDepotByItsNumber",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorDepotCardNamesTheDepotByItsNumberTest::RunTest(const FString&)
{
	using namespace InspectorCardsTest;
	FCardDepotRig Rig;
	if (!TestTrue(TEXT("setup: an attached runtime and the first plotted depot"), Rig.Build())) { return false; }
	ARoadNetworkActor* Actor = Rig.World.Actor;
	const auto PlaceDepotAt = [Actor](double X)
	{
		const TArray<FVector2D> Plot = { FVector2D(X, 0.0), FVector2D(X + 5000.0, 0.0), FVector2D(X + 5000.0, 2400.0), FVector2D(X, 2400.0) };
		return Actor->PlaceEntityInPlot(Plot, Plot[0], Plot[1], { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump }, EPlaceableEntity::FuelDepot);
	};
	const int32 First = Rig.Index;
	const int32 Second = PlaceDepotAt(20000.0);
	if (!TestTrue(TEXT("a second depot is placed"), Second != INDEX_NONE)) { return false; }
	if (!TestTrue(TEXT("the first is bulldozed"), Actor->DeleteEntity(First))) { return false; }
	const int32 Third = PlaceDepotAt(40000.0);
	if (!TestTrue(TEXT("a third depot is placed"), Third != INDEX_NONE)) { return false; }
	TestEqual(TEXT("it took the bulldozed depot's slot - the recycling that makes an index the wrong name for a depot"), Third, First);

	const auto TitleOf = [&Rig](int32 EntityIndex)
	{
		Rig.Index = EntityIndex;
		FDepotCard Card;
		const FInspectorCardView* View = Card.Describe(Rig.Input());
		return View != nullptr ? View->Title : FString(TEXT("<no card>"));
	};
	TestEqual(TEXT("the third depot's card says depot 3, not the retired 1 its index would give"), TitleOf(Third), FString(TEXT("Fuel depot 3")));
	TestEqual(TEXT("the survivor's card still says depot 2"), TitleOf(Second), FString(TEXT("Fuel depot 2")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorDepotCardTest, "AirportMgr.Inspector.Card.Depot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorDepotCardTest::RunTest(const FString&)
{
	using namespace InspectorCardsTest;
	FCardDepotRig Rig;
	if (!TestTrue(TEXT("setup: an attached runtime and a plotted depot"), Rig.Build())) { return false; }
	FDepotCard Card;
	const FInspectorCardInput In = Rig.Input();
	TestEqual(TEXT("the table tells a depot from a stand by its pose role"),
		FInspectorCards::CardFor(In.Selection, In.Target->GetNetwork()), EInspectorCard::Depot);
	const FInspectorCardView* View = Card.Describe(In);
	if (!TestNotNull(TEXT("a view"), View)) { return false; }
	TestEqual(TEXT("it is the depot card's"), View->Card, EInspectorCard::Depot);
	TestTrue(FString::Printf(TEXT("titled as a depot ('%s')"), *View->Title), View->Title.StartsWith(TEXT("Fuel depot ")));
	TestTrue(TEXT("no verbs: the rows under it are the depot's"), View->Verbs == EInspectorVerbs::None);
	if (!TestTrue(TEXT("the quote is IN the view, for the rows to render"), View->Quote.IsFacility())) { return false; }
	TestEqual(TEXT("an empty depot"), View->Quote.Vehicles, 0);
	TestTrue(TEXT("with room and a shed to buy"), View->Quote.Bays > 0 && View->Quote.Modules.Num() > 0);
	TestEqual(TEXT("asked once"), Card.QuoteCount(), 1);

	// A PURCHASE MOVES THE LEDGER AND THE FLEET - both revisions in the card's key - so the next Describe shows it.
	const FPurchaseResult Bought = Rig.Runtime->BuyVehicle(Rig.Depot, View->Quote.VehicleOffers[0].TypeCode);
	if (!TestTrue(TEXT("setup: a vehicle bought"), Bought.Succeeded())) { return false; }
	View = Card.Describe(In);
	if (!TestNotNull(TEXT("a view, after"), View)) { return false; }
	TestEqual(TEXT("the quote shows the vehicle at once"), View->Quote.Vehicles, 1);
	TestEqual(TEXT("and lists it as a fleet row"), View->Quote.Fleet.Num(), 1);
	TestEqual(TEXT("asked again, once"), Card.QuoteCount(), 2);
	return true;
}

// --- The table, and the widget's use of it -------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorEveryKindHasACardTest, "AirportMgr.Inspector.EveryKindHasACard",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorEveryKindHasACardTest::RunTest(const FString&)
{
	// THE TABLE AND ITS CONSUMER. The static_asserts stop a kind or a card appended without a row from COMPILING; this is what
	// goes red when the rows agree by count and not by name, or when the widget stops asking the table: every card is found by
	// its own Id, every selection kind reaches one, and a real panel shown each of them paints the verbs ITS card names and
	// collapses the rest - so a caption from the card before can never stay on screen.
	using namespace InspectorCardsTest;
	FCardRig Rig;
	if (!TestTrue(TEXT("the rig"), Rig.Ok())) { return false; }
	FInspectorCards Cards;
	for (int32 Each = static_cast<int32>(EInspectorCard::None) + 1; Each < static_cast<int32>(EInspectorCard::Count); ++Each)
	{
		const EInspectorCard Card = static_cast<EInspectorCard>(Each);
		IInspectorCard* Found = Cards.Find(Card);
		if (TestNotNull(FString::Printf(TEXT("a card for EInspectorCard %d"), Each), Found))
		{
			TestEqual(FString::Printf(TEXT("found by its own Id (%d)"), Each), Found->Id(), Card);
		}
	}
	TestNull(TEXT("None has no card"), Cards.Find(EInspectorCard::None));
	for (int32 Each = static_cast<int32>(ESelectionKind::None) + 1; Each < static_cast<int32>(ESelectionKind::Count); ++Each)
	{
		const FSelection Selection = FCardRig::Select(static_cast<ESelectionKind>(Each), 0);
		const EInspectorCard Card = FInspectorCards::CardFor(Selection, Rig.Net);
		TestNotEqual(FString::Printf(TEXT("selection kind %d reaches a card"), Each), Card, EInspectorCard::None);
		TestNotNull(FString::Printf(TEXT("and the table has it (%d)"), Each), Cards.Find(Card));
	}
	TestEqual(TEXT("no selection, no card"), FInspectorCards::CardFor(FSelection(), Rig.Net), EInspectorCard::None);
	TestEqual(TEXT("a kind outside the enum, no card"),
		FInspectorCards::CardFor(FCardRig::Select(static_cast<ESelectionKind>(200), 0), Rig.Net), EInspectorCard::None);
	TestEqual(TEXT("a stand's entity is the stand card's"),
		FInspectorCards::CardFor(FCardRig::Select(ESelectionKind::Stand, Rig.Field.Stands[0].Index), Rig.Net), EInspectorCard::Stand);

	// THE WIDGET PAINTS WHAT THE TABLE'S CARD SAYS: each kind in turn through a real panel, the verbs checked on every step.
	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(Rig.TestWorld.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel"), Panel)) { return false; }
	FAgentFacts F;
	F.Id = 1;
	F.TypeName = TEXT("TestType");
	F.Status = TEXT("Waiting behind aircraft 2");
	F.bStatusIsHold = true;
	F.Hold.WaitingOn = 2;
	F.Hold.At = EHoldAt::Behind;
	struct FWant { ESelectionKind Kind; int32 Id; const TCHAR* TitlePrefix; bool bAgentVerbs; bool bRunwayVerbs; bool bShow; };
	const FWant Order[] = {
		{ ESelectionKind::Aircraft, 1, TEXT("TestType"), true, false, true },
		{ ESelectionKind::Runway, Rig.RunwaySegment(), TEXT("Runway"), false, true, false },
		{ ESelectionKind::Taxiway, Rig.TaxiwaySegment(), TEXT("Taxiway"), false, false, false },
		{ ESelectionKind::Stand, Rig.Field.Stands[0].Index, TEXT("Stand"), false, false, false },
		{ ESelectionKind::Aircraft, 1, TEXT("TestType"), true, false, true },
		{ ESelectionKind::Runway, Rig.RunwaySegment(), TEXT("Runway"), false, true, false } };
	for (const FWant& Want : Order)
	{
		Panel->Refresh(Rig.Actor, FCardRig::Select(Want.Kind, Want.Id), Want.Kind == ESelectionKind::Aircraft ? &F : nullptr);
		const FString Step = FString::Printf(TEXT("kind %d"), static_cast<int32>(Want.Kind));
		TestTrue(Step + TEXT(": the panel shows"), Panel->IsShownForTest());
		TestTrue(FString::Printf(TEXT("%s: titled by its card ('%s')"), *Step, *Panel->TitleForTest()), Panel->TitleForTest().StartsWith(Want.TitlePrefix));
		TestEqual(Step + TEXT(": Depart"), Shown(Panel->DepartButton), Want.bAgentVerbs);
		TestEqual(Step + TEXT(": Follow"), Shown(Panel->FollowButton), Want.bAgentVerbs);
		TestEqual(Step + TEXT(": Unstick"), Shown(Panel->UnstickMenu), Want.bAgentVerbs);
		TestEqual(Step + TEXT(": Show (only while it waits)"), Shown(Panel->WaitingForButton), Want.bShow);
		TestEqual(Step + TEXT(": the Show caption is the card's, never the last one's"),
			Panel->WaitingForCaptionForTest().IsEmpty(), !Want.bShow);
		TestEqual(Step + TEXT(": the flip"), Shown(Panel->RunwayButton), Want.bRunwayVerbs);
		TestEqual(Step + TEXT(": the runway mode"), Shown(Panel->RunwayUseButton), Want.bRunwayVerbs);
		TestEqual(Step + TEXT(": no deadlock line off an aircraft's card"), Panel->DeadlockForTest().IsEmpty(), true);
		TestTrue(Step + TEXT(": Locate, on every card"), Panel->IsLocateShownForTest());
		// COLLAPSED, not merely not Visible: the rows' root is SelfHitTestInvisible when shown, so a Visible test could never fail.
		// A depot's card is AirportMgr.Inspector.PurchaseRowsCollapseOffTheDepotCard's - this walk has no runtime to quote from.
		TestTrue(Step + TEXT(": no purchase rows off a depot's card"), Panel->FacilityRows->GetVisibility() == ESlateVisibility::Collapsed);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorFacilityRowsAreWiredTest, "AirportMgr.Inspector.FacilityRowsAreWired",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorFacilityRowsAreWiredTest::RunTest(const FString&)
{
	// THE PURCHASE ROWS ARE A SUB-WIDGET THE INSPECTOR BUILDS, AND ITS THREE ACTION ROWS ARE CONSUMED: an inspector that stopped
	// building it, left it unparented, or forgot to hand it a door to run a row through would draw its depot card
	// with no rows, or rows whose clicks went nowhere, and every other test of the card would notice only by the missing buttons.
	FAirsideTestWorld Bare(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), Bare.World)) { return false; }
	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(Bare.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel"), Panel)) { return false; }
	UInspectorFacilityRows* Rows = Panel->FacilityRows;
	if (!TestNotNull(TEXT("the panel built its purchase rows"), Rows)) { return false; }
	TestTrue(TEXT("built, through Build"), Rows->IsBuilt());
	TestNotNull(TEXT("and parented in the card's column, or it is never drawn"), Rows->GetParent());
	TestTrue(TEXT("handed a door to run a BuildActions row through - WITH an argument, the only thing the rows act by (#448)"), static_cast<bool>(Rows->RunActionSource));

	const FBuildAction* BuyModule = FindAction(FName(TEXT("selection.buy_module")));
	const FBuildAction* BuyVehicle = FindAction(FName(TEXT("selection.buy_vehicle")));
	if (!TestNotNull(TEXT("the registry has a buy_module row"), BuyModule) || !TestNotNull(TEXT("and a buy_vehicle row"), BuyVehicle)) { return false; }
	if (!TestNotNull(TEXT("the rows built a buy-module button"), Rows->BuyModuleButton.Get())
		|| !TestNotNull(TEXT("and a buy-vehicle menu"), Rows->BuyVehicleMenu.Get())) { return false; }
	TestEqual(TEXT("Buy's caption comes from its row"), Rows->BuyModuleCaptionForTest(), BuyModule->Label.ToString());
	TestEqual(TEXT("the menu's caption comes from its row"), Rows->BuyVehicleMenu->GetButton()->GetLabel()->GetText().ToString(), BuyVehicle->Label.ToString());

	// THE ONE LIST: the rows own exactly their three, and the inspector's positional scan skips exactly those.
	for (const TCHAR* Own : { TEXT("selection.buy_module"), TEXT("selection.buy_vehicle"), TEXT("selection.sell_vehicle") })
	{
		TestTrue(FString::Printf(TEXT("the rows own %s"), Own), UInspectorFacilityRows::OwnsAction(FName(Own)));
	}
	for (const TCHAR* NotOwn : { TEXT("selection.depart"), TEXT("selection.follow"), TEXT("selection.runway_in_use"), TEXT("selection.unstick") })
	{
		TestFalse(FString::Printf(TEXT("and not %s"), NotOwn), UInspectorFacilityRows::OwnsAction(FName(NotOwn)));
	}
	// AND THE ROWS FOUND EACH BY ID, from the same table: each action's index is the registry row carrying that id.
	const TConstArrayView<FBuildAction> Registry = BuildActions();
	const TPair<UInspectorFacilityRows::EAction, const TCHAR*> Found[] = {
		{ UInspectorFacilityRows::EAction::BuyModule, TEXT("selection.buy_module") },
		{ UInspectorFacilityRows::EAction::BuyVehicle, TEXT("selection.buy_vehicle") },
		{ UInspectorFacilityRows::EAction::SellVehicle, TEXT("selection.sell_vehicle") } };
	for (const TPair<UInspectorFacilityRows::EAction, const TCHAR*>& Each : Found)
	{
		const int32 Index = Rows->ActionIndexForTest(Each.Key);
		TestTrue(FString::Printf(TEXT("the rows found %s in BuildActions() ..."), Each.Value), Registry.IsValidIndex(Index));
		TestTrue(FString::Printf(TEXT("... and it is that row (index %d)"), Index), Registry.IsValidIndex(Index) && Registry[Index].Id == FName(Each.Value));
	}
	const FBuildAction* Depart = FindAction(FName(TEXT("selection.depart")));
	if (TestNotNull(TEXT("Depart is still in the registry"), Depart) && TestNotNull(TEXT("and on the panel"), Panel->DepartButton.Get()))
	{
		TestEqual(TEXT("and the purchase rows, skipped by the positional scan, did not shift it"),
			Panel->DepartButton->GetLabel()->GetText().ToString(), Depart->Label.ToString());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorPurchaseRowsCollapseTest, "AirportMgr.Inspector.PurchaseRowsCollapseOffTheDepotCard",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorPurchaseRowsCollapseTest::RunTest(const FString&)
{
	// THE ROWS GO WHEN THE DEPOT DOES. The widget hands EVERY card's quote to the rows, and a non-depot's default quote is what collapses
	// them - so a widget that showed the rows only for a depot's card would leave the last depot's buttons drawn under a stand's, with
	// every other test green (the aircraft/runway walk has no depot to come from, and its rows are collapsed from the build).
	// The rows' root is SelfHitTestInvisible while shown, so "gone" is asked as Collapsed, never as "not Visible".
	using namespace InspectorCardsTest;
	FCardDepotRig Rig;
	if (!TestTrue(TEXT("setup: an attached runtime and a plotted depot"), Rig.Build())) { return false; }
	ARoadNetworkActor* Actor = Rig.World.Actor;
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();
	IRoadEditTarget* Target = Actor;
	const int32 Stand = Target->PlaceStand(FVector2D(60000.0, 0.0), 0.0);
	if (!TestTrue(TEXT("setup: a stand to select after the depot"), Stand != INDEX_NONE)) { return false; }
	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(Rig.World.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel"), Panel) || !TestNotNull(TEXT("with its purchase rows"), Panel->FacilityRows.Get())) { return false; }
	UInspectorFacilityRows* Rows = Panel->FacilityRows;
	TestTrue(TEXT("collapsed before any card"), Rows->GetVisibility() == ESlateVisibility::Collapsed);

	const FSelection Depot = FCardRig::Select(ESelectionKind::Stand, Rig.Index);
	Panel->RefreshWith(Rig.Runtime, Actor, Depot);
	TestTrue(FString::Printf(TEXT("the depot's card ('%s')"), *Panel->TitleForTest()), Panel->TitleForTest().StartsWith(TEXT("Fuel depot")));
	TestTrue(TEXT("shows its purchase rows"), Rows->AreFacilityRowsShownForTest());
	TestTrue(TEXT("and the rows' root is laid out"), Rows->GetVisibility() != ESlateVisibility::Collapsed);

	Panel->RefreshWith(Rig.Runtime, Actor, FCardRig::Select(ESelectionKind::Stand, Stand));
	TestTrue(FString::Printf(TEXT("a stand's card next ('%s')"), *Panel->TitleForTest()), Panel->TitleForTest().StartsWith(TEXT("Stand")));
	TestTrue(TEXT("collapses the rows' root"), Rows->GetVisibility() == ESlateVisibility::Collapsed);
	TestFalse(TEXT("and every row in it"), Rows->AreFacilityRowsShownForTest());

	Panel->RefreshWith(Rig.Runtime, Actor, Depot);
	TestTrue(TEXT("the depot again: the rows return"), Rows->AreFacilityRowsShownForTest() && Rows->GetVisibility() != ESlateVisibility::Collapsed);

	FAgentFacts F;
	F.Id = 1;
	F.TypeName = TEXT("TestType");
	Panel->RefreshWith(Rig.Runtime, Actor, FCardRig::Select(ESelectionKind::Aircraft, 1), &F);
	TestTrue(TEXT("and an aircraft's card collapses them as well"),
		Rows->GetVisibility() == ESlateVisibility::Collapsed && !Rows->AreFacilityRowsShownForTest());
	return true;
}

// --- The two properties that make the gates safe --------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorCardMemoMatchesFreshTest, "AirportMgr.Inspector.CardMemoMatchesFreshDescribe",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorCardMemoMatchesFreshTest::RunTest(const FString&)
{
	// A NETWORK CARD'S KEY NEVER HIDES A CHANGE. Long-lived cards follow a fixed-seed walk of every input the runway, taxiway and
	// stand cards read - a threshold dragged, the runway's mode and approach set through the facade, a stand held and released,
	// claims churning, the taxiway re-laid wider or in another surface - and after EVERY step each one's view must equal a fresh
	// card's. The gate may skip a describe only when nothing moved; if a key misses an input, a step shows a stale view and this
	// names it. The same property AirportMgr.Inspector.KeyEqualMeansTextEqual holds for the aircraft card, at the model.
	using namespace InspectorCardsTest;
	FCardRig Rig;
	if (!TestTrue(TEXT("the rig"), Rig.Ok())) { return false; }
	FInspectorCards Kept;
	FRandomStream Stream(441);
	const FRoadSegment* Piece = Rig.Net->GetSegment(Rig.Net->SegmentIdAt(Rig.RunwaySegment()));
	if (!TestNotNull(TEXT("the runway segment"), Piece)) { return false; }
	const FRoadNodeId Threshold = Piece->A;
	int32 Mismatches = 0;
	int32 Compared = 0;
	FString First;
	constexpr int32 Steps = 240;
	for (int32 Step = 0; Step < Steps; ++Step)
	{
		const int32 Runway = Rig.RunwaySegment();
		switch (Stream.RandRange(0, 9))
		{
		case 0: // a quiet frame: nothing moves
			break;
		case 8:
		{
			// THE TAXIWAY RE-LAID: another width from the actor's own set and another surface, through the facade.
			const int32 Widths = Rig.Actor->GetWidthCount(ERoadKind::Taxiway);
			if (Widths > 0)
			{
				Rig.Actor->UpgradeSegment(Rig.TaxiwaySegment(), ERoadKind::Taxiway, Stream.RandRange(0, Widths - 1),
					Stream.RandRange(0, 1) == 0 ? EPavement::Tarmac : EPavement::Concrete);
			}
			break;
		}
		case 1:
			Rig.Net->SetNodePosition(Threshold, Rig.Net->GetNode(Threshold)->Position + FVector2D(Stream.FRandRange(-3000.0f, 3000.0f), 0.0));
			break;
		case 2:
		case 3:
		{
			FRunwayFacts Facts = Rig.Net->RunwayFactsFor(Rig.Net->SegmentIdAt(Runway));
			Facts.Use = static_cast<ERunwayUse>(Stream.RandRange(static_cast<int32>(ERunwayUse::Mixed), static_cast<int32>(ERunwayUse::DeparturesOnly)));
			Facts.Approach = static_cast<ERunwayApproach>(Stream.RandRange(0, static_cast<int32>(ERunwayApproach::Count) - 1));
			Rig.Actor->SetRunwayFacts(Runway, Facts);
			break;
		}
		case 4:
			Rig.Traffic().HoldStand(-7, Rig.StandPose());
			break;
		case 5:
			Rig.Traffic().ReleaseHold(-7);
			break;
		case 6:
			Rig.Traffic().Advance(0.05, Rig.Net);
			break;
		case 7:
			// A BODY ON THE POSE - only where nothing else holds it. The arbiter never lets two holders have one pose node
			// (HoldStand refuses the second), and Assert is a test door that would: a walk through that state measures a
			// world the game cannot reach, and the walk's first run did (a hold by -7 and a body by 77, the card naming either).
			if (!Rig.Traffic().IsStandHeld(Rig.StandPose(), 77))
			{
				Rig.Traffic().OccupancyForTest().Assert(FTrafficClaim::Make(77, FTrafficResource::OfNode(Rig.StandPose()), /*bOccupied*/ true, 2));
				Rig.Traffic().Advance(0.05, Rig.Net);
			}
			break;
		default:
			Rig.Traffic().OccupancyForTest().ReleaseAll(77);
			Rig.Traffic().Advance(0.05, Rig.Net);
			break;
		}
		const FSelection Selections[] = {
			FCardRig::Select(ESelectionKind::Runway, Rig.RunwaySegment()),
			FCardRig::Select(ESelectionKind::Taxiway, Rig.TaxiwaySegment()),
			FCardRig::Select(ESelectionKind::Stand, Rig.Field.Stands[0].Index) };
		for (const FSelection& Selection : Selections)
		{
			const FInspectorCardInput In = Rig.Input(Selection);
			const EInspectorCard Which = FInspectorCards::CardFor(Selection, Rig.Net);
			const FInspectorCardView* KeptView = Kept.Find(Which)->Describe(In);
			FInspectorCards Fresh;
			const FInspectorCardView* FreshView = Fresh.Find(Which)->Describe(In);
			++Compared;
			FString Why;
			if ((KeptView == nullptr) != (FreshView == nullptr))
			{
				Why = KeptView == nullptr ? TEXT("the kept card shows nothing, a fresh one shows a card") : TEXT("the kept card shows a card, a fresh one shows nothing");
			}
			else if (KeptView != nullptr && !ViewsMatch(*KeptView, *FreshView, Why))
			{
				// Why names the first difference.
			}
			else
			{
				continue;
			}
			if (Mismatches++ == 0)
			{
				First = FString::Printf(TEXT("step %d, card %d - %s"), Step, static_cast<int32>(Which), *Why);
			}
		}
	}
	TestEqual(FString::Printf(TEXT("%d of %d kept views differed from a fresh describe - first: %s"), Mismatches, Compared, *First), Mismatches, 0);
	// THE WALK MEASURES SOMETHING: the gate held on quiet steps (fewer describes than frames) and opened on moves (more than one).
	TestTrue(FString::Printf(TEXT("the runway card was described again when its inputs moved (%d times in %d steps)"), Kept.Runway().DescribeCount(), Steps),
		Kept.Runway().DescribeCount() > 10 && Kept.Runway().DescribeCount() < Steps);
	TestTrue(FString::Printf(TEXT("and the stand card (%d)"), Kept.Stand().DescribeCount()), Kept.Stand().DescribeCount() > 10 && Kept.Stand().DescribeCount() < Steps);
	TestTrue(FString::Printf(TEXT("and the taxiway card (%d)"), Kept.Taxiway().DescribeCount()), Kept.Taxiway().DescribeCount() > 2 && Kept.Taxiway().DescribeCount() < Steps);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorDepotMemoMatchesFreshTest, "AirportMgr.Inspector.DepotMemoMatchesFreshDescribe",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorDepotMemoMatchesFreshTest::RunTest(const FString&)
{
	// THE DEPOT CARD'S KEY, the same property over what ITS Describe and its quote read: the vehicles bought and sold, the money
	// posted, the clock running through minutes, a shed bought (the airport rebuilds), a job opened. The quote is in the view, so
	// the rows it feeds are checked with the text.
	using namespace InspectorCardsTest;
	FCardDepotRig Rig;
	if (!TestTrue(TEXT("setup: an attached runtime and a plotted depot"), Rig.Build())) { return false; }
	FDepotCard Kept;
	FRandomStream Stream(441);
	int32 Mismatches = 0;
	FString First;
	constexpr int32 Steps = 160;
	for (int32 Step = 0; Step < Steps; ++Step)
	{
		const FFacilityQuote Quote = Rig.Runtime->QuoteFacility(Rig.Depot);
		switch (Stream.RandRange(0, 7))
		{
		case 0: // quiet
			break;
		case 1:
			if (Quote.VehicleOffers.Num() > 0) { Rig.Runtime->BuyVehicle(Rig.Depot, Quote.VehicleOffers[0].TypeCode); }
			break;
		case 2:
			if (Quote.Fleet.Num() > 0) { Rig.Runtime->SellVehicle(Quote.Fleet[0].VehicleId); }
			break;
		case 3:
			Rig.Runtime->GetLedger()->Post(Rig.Runtime->GetClock()->Now(), ELedgerCategory::Fleet, Stream.FRandRange(-90000.0f, 30000.0f), FText::FromString(TEXT("test: money moves")));
			break;
		case 4:
			Rig.AdvanceGame(Stream.FRandRange(1.0f, 130.0f));
			break;
		case 5:
			Rig.Runtime->BuyModule(Rig.Depot, EDepotModule::Shed);
			break;
		case 6:
			Rig.Runtime->GetJobBoard()->AddJobForTest(Stream.RandRange(1, 9), EServiceJobState::Open, EServiceRefusal::None, 0);
			break;
		default:
			Rig.AdvanceGame(Stream.FRandRange(1.0f, 20.0f));
			break;
		}
		const FInspectorCardInput In = Rig.Input();
		const FInspectorCardView* KeptView = Kept.Describe(In);
		FDepotCard Fresh;
		const FInspectorCardView* FreshView = Fresh.Describe(In);
		FString Why;
		if ((KeptView == nullptr) != (FreshView == nullptr))
		{
			Why = TEXT("one card shows nothing and the other does not");
		}
		else if (KeptView != nullptr && !ViewsMatch(*KeptView, *FreshView, Why))
		{
			// Why names the first difference.
		}
		else
		{
			continue;
		}
		if (Mismatches++ == 0)
		{
			First = FString::Printf(TEXT("step %d - %s"), Step, *Why);
		}
	}
	TestEqual(FString::Printf(TEXT("%d of %d kept depot views differed from a fresh describe - first: %s"), Mismatches, Steps, *First), Mismatches, 0);
	TestTrue(FString::Printf(TEXT("the walk measured something: described again on moves, held on quiet steps (%d describes in %d steps)"), Kept.DescribeCount(), Steps),
		Kept.DescribeCount() > 10 && Kept.DescribeCount() < Steps);
	TestTrue(FString::Printf(TEXT("and the quote was asked once per describe, not once per step (%d quotes)"), Kept.QuoteCount()), Kept.QuoteCount() == Kept.DescribeCount());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorTurnaroundKeyMatchesTest, "AirportMgr.Inspector.TurnaroundKeyMatchesItsSentence",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorTurnaroundKeyMatchesTest::RunTest(const FString&)
{
	// THE TURNAROUND'S KEY IS A COUPLING ACROSS TWO FILES: it recomputes RoundToInt(Abs(Left) / 60) because GameTimeText::Duration
	// (GameTimeText.cpp) rounds that way. Walk Now across a whole contract and past it into lateness, a few seconds at a time
	// and on every half minute (where two roundings part), and the kept line must be the sentence a fresh DescribeTurnaround gives
	// - so a rounding changed on either side goes red here, and not stale on the card.
	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	FRandomStream Stream(441);
	FInspectorTurnaround Kept;
	int32 Mismatches = 0;
	int32 Checked = 0;
	int32 Recomposed = 0;
	FString First;
	auto Check = [&](double Now)
	{
		Recomposed += Kept.Refresh(*Flight, Now) ? 1 : 0;
		const FString Fresh = UArrivalRowViewModel::DescribeTurnaround(*Flight, Now).ToString();
		++Checked;
		if (Kept.Line != Fresh && Mismatches++ == 0)
		{
			First = FString::Printf(TEXT("contract %.0f s accepted %.0f s, now %.1f s: kept '%s' but a fresh sentence is '%s'"),
				Flight->ContractSeconds, Flight->AcceptedAt, Now, *Kept.Line, *Fresh);
		}
	};
	for (int32 Contract = 0; Contract < 8; ++Contract)
	{
		Flight->AcceptedAt = Stream.FRandRange(0.0f, 5000.0f);
		Flight->ContractSeconds = Contract == 0 ? 0.0 : Stream.RandRange(1, 40) * 300.0;
		for (int32 Half = -240; Half <= 240; ++Half)
		{
			Check(Flight->AirborneBy() - 30.0 * Half);
		}
		double Now = Flight->AcceptedAt;
		for (int32 Step = 0; Step < 800; ++Step)
		{
			Now += Stream.FRandRange(0.0f, 40.0f);
			Check(Now);
		}
	}
	TestEqual(FString::Printf(TEXT("%d of %d kept sentences differed from a fresh one - first: %s"), Mismatches, Checked, *First), Mismatches, 0);
	TestTrue(FString::Printf(TEXT("the walk moved the minutes it prints (%d composes for %d asks)"), Recomposed, Checked), Recomposed > 100 && Recomposed < Checked);

	// AND IT HOLDS WHERE NOTHING MOVES: half-second steps across ten minutes of one contract compose once a minute, not once an ask.
	Flight->AcceptedAt = 1000.0;
	Flight->ContractSeconds = 7200.0;
	const int32 Before = Recomposed;
	int32 Asks = 0;
	for (double Now = 1000.0; Now < 1600.0; Now += 0.5)
	{
		Check(Now);
		++Asks;
	}
	TestTrue(FString::Printf(TEXT("ten minutes at half-second steps: %d composes for %d asks"), Recomposed - Before, Asks), Recomposed - Before <= 12);
	TestEqual(TEXT("and still no sentence differed"), Mismatches, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorEveryCardLocatesTest, "AirportMgr.Inspector.EveryCardLocatesItsSubject",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorEveryCardLocatesTest::RunTest(const FString&)
{
	// EVERY CARD SAYS WHERE ITS SUBJECT IS (2026-10-02, "on all inspection tabs a button to locate the thing"): the five cards each fill
	// FInspectorCardView::Locate, by the kind SelectAndFocus can act on - an aircraft by agent id (found where it is at the click), a
	// stand and a depot by entity index, a runway and a taxiway as a Point on the selected segment's own curve. A card that forgot is a
	// Locate button that never shows, which nothing else here would notice.
	using namespace InspectorCardsTest;
	FCardRig Rig;
	if (!TestTrue(TEXT("the rig"), Rig.Ok())) { return false; }
	FInspectorCards Cards;
	auto ViewOf = [&](ESelectionKind Kind, int32 Id, const FAgentFacts* Facts) -> const FInspectorCardView*
	{
		FInspectorCardInput In = Rig.Input(FCardRig::Select(Kind, Id));
		In.PrecomputedAgentFacts = Facts;
		IInspectorCard* Card = Cards.Find(FInspectorCards::CardFor(In.Selection, Rig.Net));
		return Card != nullptr ? Card->Describe(In) : nullptr;
	};
	auto SegmentMid = [&](int32 Index)
	{
		const FRoadSegment& S = Rig.Net->GetSegments()[Index];
		return GuidelineGeom::Eval(Rig.Net->GetNode(S.A)->Position, S.Control, Rig.Net->GetNode(S.B)->Position, 0.5);
	};

	FAgentFacts F;
	F.Id = 7;
	F.TypeName = TEXT("TestType");
	if (const FInspectorCardView* V = ViewOf(ESelectionKind::Aircraft, 7, &F); TestNotNull(TEXT("an aircraft card"), V))
	{
		TestEqual(TEXT("an aircraft is located by its agent id, where it is at the click"), V->Locate.Kind, EAlertFocusKind::Agent);
		TestEqual(TEXT("that agent"), V->Locate.Id, 7);
	}
	for (const int32 Segment : { Rig.RunwaySegment(), Rig.TaxiwaySegment() })
	{
		const bool bRunway = Segment == Rig.RunwaySegment();
		const FInspectorCardView* V = ViewOf(bRunway ? ESelectionKind::Runway : ESelectionKind::Taxiway, Segment, nullptr);
		if (!TestNotNull(bRunway ? TEXT("a runway card") : TEXT("a taxiway card"), V)) { continue; }
		TestEqual(TEXT("a segment is located as a point - it is already the selection"), V->Locate.Kind, EAlertFocusKind::Point);
		TestEqual(TEXT("on the segment's own curve, at its middle"), V->Locate.Point, SegmentMid(Segment));
	}
	const int32 Stand = Rig.Field.Stands[0].Index;
	if (const FInspectorCardView* V = ViewOf(ESelectionKind::Stand, Stand, nullptr); TestNotNull(TEXT("a stand card"), V))
	{
		TestEqual(TEXT("a stand is located as its entity"), V->Locate.Kind, EAlertFocusKind::Entity);
		TestEqual(TEXT("that stand"), V->Locate.Id, Stand);
	}
	FCardDepotRig Depot;
	if (TestTrue(TEXT("setup: a plotted depot"), Depot.Build()))
	{
		FDepotCard Card;
		const FInspectorCardInput In = Depot.Input();
		if (const FInspectorCardView* V = Card.Describe(In); TestNotNull(TEXT("a depot card"), V))
		{
			TestEqual(TEXT("a depot is located as its entity"), V->Locate.Kind, EAlertFocusKind::Entity);
			TestEqual(TEXT("that depot"), V->Locate.Id, In.Selection.Id);
		}
	}

	// THE WIDGET SHOWS IT for each, and hides it with nothing selected - a Locate left from the last card would send the camera to it.
	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(Rig.TestWorld.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel"), Panel)) { return false; }
	Panel->Refresh(Rig.Actor, FCardRig::Select(ESelectionKind::Taxiway, Rig.TaxiwaySegment()), nullptr);
	TestTrue(TEXT("the panel shows Locate on a card that has a place"), Panel->IsLocateShownForTest());
	Panel->Refresh(Rig.Actor, FSelection(), nullptr);
	ARoadBuildController* C = Rig.TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(Rig.Actor);
	TestFalse(TEXT("nothing selected: Locate has nowhere to go"), Panel->Locate(*C));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorLocateMovesTheCameraTest, "AirportMgr.Inspector.LocateMovesTheCamera",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorLocateMovesTheCameraTest::RunTest(const FString&)
{
	// THE BUTTON'S ACTION, through a real controller: the camera goes to the card's subject and the card stays on it. A taxiway (a
	// Point - camera only) and a stand (an Entity - SelectAndFocus re-selects what is already selected).
	using namespace InspectorCardsTest;
	FCardRig Rig;
	if (!TestTrue(TEXT("the rig"), Rig.Ok())) { return false; }
	ARoadBuildController* C = Rig.TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(Rig.Actor);
	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(Rig.TestWorld.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel"), Panel)) { return false; }

	const int32 Segment = Rig.TaxiwaySegment();
	const FRoadSegment& S = Rig.Net->GetSegments()[Segment];
	const FVector2D Mid = GuidelineGeom::Eval(Rig.Net->GetNode(S.A)->Position, S.Control, Rig.Net->GetNode(S.B)->Position, 0.5);
	Panel->Refresh(Rig.Actor, FCardRig::Select(ESelectionKind::Taxiway, Segment), nullptr);
	TestTrue(TEXT("a taxiway is somewhere to go"), Panel->Locate(*C));
	TestEqual(TEXT("the camera goes to the taxiway's middle"), C->GetViewFocus(), Mid);

	const int32 Stand = Rig.Field.Stands[0].Index;
	Panel->Refresh(Rig.Actor, FCardRig::Select(ESelectionKind::Stand, Stand), nullptr);
	TestTrue(TEXT("a stand is somewhere to go"), Panel->Locate(*C));
	TestEqual(TEXT("the camera goes to the stand"), C->GetViewFocus(), Rig.Net->GetEntities()[Stand].Position);
	TestEqual(TEXT("and the stand is the selection"), C->GetSelection().Kind, ESelectionKind::Stand);
	TestEqual(TEXT("that stand"), C->GetSelection().Id, Stand);
	return true;
}

#endif
