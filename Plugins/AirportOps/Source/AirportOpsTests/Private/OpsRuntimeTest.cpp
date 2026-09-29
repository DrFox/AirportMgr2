#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/TaxiwayStrip.h"
#include "Model/OfferGenerator.h"
#include "Model/ArrivalSequencer.h"
#include "Model/BuildPurse.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/JobBoard.h"
#include "Model/InspectFacts.h"
#include "Model/OpsEvents.h"
#include "Model/OpsSave.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/SimClock.h"
#include "OpsEventsTestListener.h"
#include "Present/AirsideTraffic.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Solve/IcaoCode.h"
#include "Testing/AirsideTestGraph.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsRuntimeTest,
	"AirportOps.Present.Runtime",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsRuntimeTest::RunTest(const FString& Parameters)
{
	// THE COMPOSITION TEST FOR THE SEAM. Every piece below has its own unit test; this is
	// the one that fails if any of them is left unwired: Airside delegate -> runtime ->
	// bus, runtime speed -> actor scale, runtime save -> slot -> restore -> mesh rebuilt.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world to spawn into"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }

	// A real road, so the mesh has triangles the load can be measured by.
	const int32 RoadA = Actor->PlaceNode(FVector2D(0.0, 30000.0));
	const int32 RoadB = Actor->PlaceNode(FVector2D(20000.0, 30000.0));
	if (!TestTrue(TEXT("a road segment exists"), Actor->ConnectNodes(RoadA, RoadB))) { return false; }
	if (!TestNotNull(TEXT("the actor has a network"), Actor->Network.Get())) { return false; }

	// A RUNWAY, clear of the road above, so UFlightBoard::DefaultApproachFocus has a threshold
	// to find - issue #105's follow-up comment on this test asks for exactly that, plus the
	// offer schedule check below.
	URoadProfile* RunwayProfile = TestProfiles::Runway();
	Actor->MinimumRunwayLength = 100.0;   // short, deliberately - see MeshFreshnessTest's own comment
	if (!TestTrue(TEXT("a runway is placed"),
		Actor->PlaceRunway(FVector2D(0.0, -50000.0), FVector2D(6000.0, -50000.0), RunwayProfile)))
	{
		return false;
	}
	FVector2D ExpectedFocus;
	if (!TestTrue(TEXT("the runway gives DefaultApproachFocus a threshold to find"),
		UFlightBoard::DefaultApproachFocus(*Actor->Network, ExpectedFocus)))
	{
		return false;
	}

	// And one authored guideline for the agent, as in AgentRedirectTest.
	{
		URoadNetwork& Net = *Actor->Network;
		const FGuidelineNodeId Start = Net.AddGuidelineNode(FVector2D(0.0, 0.0), /*bDerived*/ false);
		const FGuidelineNodeId End = Net.AddGuidelineNode(FVector2D(20000.0, 0.0), /*bDerived*/ false);
		FGuidelineEdge Edge;
		Edge.A = Start;
		Edge.B = End;
		Edge.Control = FVector2D(10000.0, 0.0);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Net.AddGuidelineEdge(MoveTemp(Edge));

		// #312: was a hand-built FRouteQuery that skipped AvoidRunways.
		const FRoutePlan Outbound = TestGraph::Probe(Net, Start, End, ETraversalClass::GroundVehicle);
		if (!TestTrue(TEXT("the leg routes"), Outbound.IsValid())) { return false; }

		UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
		Runtime->Attach(Actor);

		// ATTACH'S OFFER CADENCE (issue #105, follow-up from #121 review). Content-independent:
		// it only asks whether SOME airline in the real catalog offers anything, not whether
		// this test's runway admits any particular one.
		if (TestTrue(TEXT("Attach armed the repeating offer schedule"), Runtime->HasOfferScheduledForTest()))
		{
			// ONE TICK, RIGHT THROUGH ONE GENERATOR MINUTE - not a guessed real-seconds delta
			// that might land short of it. TimeScale() is exactly the game-seconds-per-real-
			// second USimClock::Advance uses at this hour, and one minute cannot cross dawn or
			// dusk by enough to matter at the 1% margin.
			const double RealSeconds = UOfferGenerator::TickSeconds / Runtime->GetClock()->TimeScale() * 1.01;
			Runtime->Tick(RealSeconds);

			// THE FOCUS UPDATES REGARDLESS OF WHETHER THIS RUNWAY ADMITS ANY REAL FLEET
			// AIRCRAFT (UOpsRuntime::OfferTick sets it before the admissibility check) -
			// so this assertion, unlike an offer actually appearing, does not depend on this
			// test's short runway matching a real AircraftType's LandingFieldLength.
			TestEqual(TEXT("a runway-bearing network's board focus equals DefaultApproachFocus"),
				Runtime->GetFlightBoard()->ApproachFocus, ExpectedFocus);
		}

		UOpsEventsTestListener* L = NewObject<UOpsEventsTestListener>();
		Runtime->GetEvents()->OnAgentPhaseChanged.AddDynamic(L, &UOpsEventsTestListener::OnPhase);
		Runtime->GetEvents()->OnSpeedChanged.AddDynamic(L, &UOpsEventsTestListener::OnSpeed);

		Actor->DispatchAgent(Outbound, UAirsideSettings::ResolveDefaultAirframe());
		// THE BUS DELIVERS ON THE NEXT OPS STEP, not inside Airside's broadcast (spec 2026-09-29
		// ops-event-bus §1) - a zero-length step drains it without moving the clock.
		Runtime->Tick(0.0);

		// BUILT FROM THE ENUM, not spelled ":4->1". This literal broke the day
		// EAgentPhase::Manoeuvring was added between Parked and Gone and moved Gone from 4 to
		// 5 - a failure that said nothing whatever about whether a spawn reaches the ops bus,
		// which is the only thing this assertion is for. It is the exact maintenance cost the
		// speed-ladder comment forty lines below argues against, and this line had not taken
		// the lesson.
		//
		// NOT to be confused with OpsEventsTest's spelled-out ordinals, which are a
		// DELIBERATE canary: that test exists to fail when the wire format changes. This one
		// does not.
		const FString SpawnSuffix = FString::Printf(TEXT(":%d->%d"),
			static_cast<int32>(EAgentPhase::Gone), static_cast<int32>(EAgentPhase::Taxiing));

		TestTrue(TEXT("a spawn on the Airside traffic reaches the ops bus as Gone -> Taxiing"),
			L->Seen.ContainsByPredicate([&SpawnSuffix](const FString& S)
				{ return S.StartsWith(TEXT("phase:")) && S.EndsWith(SpawnSuffix); }));

		Runtime->StepSpeed(+1);
		Runtime->Tick(0.0);   // the bus delivers on the next ops step - see the spawn above
		if (TestTrue(TEXT("stepping speed announces something"), L->Seen.Num() > 0))
		{
			TestEqual(TEXT("stepping speed announces the new speed"), L->Seen.Last(), FString(TEXT("speed:2")));
		}
		TestEqual(TEXT("and pushes the multiplier into the actor"), Actor->GetSimTimeScale(), 2.0, 1e-12);

		Runtime->TogglePause();
		TestEqual(TEXT("pause zeroes the actor's scale"), Actor->GetSimTimeScale(), 0.0, 1e-12);
		Runtime->TogglePause();
		TestEqual(TEXT("unpause restores the previous speed"), Actor->GetSimTimeScale(), 2.0, 1e-12);

		// Asked of the LADDER rather than a literal, so adding a rung does not turn this
		// into a failing test that says nothing about the behaviour it guards. It used to
		// read 8.0 and duly failed the day x16 and x32 were added, which is a maintenance
		// cost with no diagnostic value.
		const TArrayView<const ESimSpeed> Ladder = USimClock::SpeedLadder();
		const double Fastest = USimClock::Multiplier(Ladder.Last());
		const double Slowest = USimClock::Multiplier(Ladder[0]);

		Runtime->StepSpeed(+Ladder.Num() + 2);
		TestEqual(TEXT("stepping past the top clamps at the fastest rung"),
			Actor->GetSimTimeScale(), Fastest, 1e-12);
		Runtime->StepSpeed(-Ladder.Num() - 4);
		TestEqual(TEXT("stepping past the bottom clamps at the slowest rung, never paused"),
			Actor->GetSimTimeScale(), Slowest, 1e-12);

		Runtime->Tick(1.0);
		TestTrue(TEXT("ticking the runtime advances the clock"), Runtime->GetClock()->Now() > 0.0);

		// Save, clear, load: the network comes back and the mesh is rebuilt, measured by
		// triangle count - the same probe MeshFreshnessTest uses. The facade defers its
		// rebuild, so the baseline and the cleared state are each rebuilt explicitly before
		// they are read; the LOAD path is the one under test and gets no such help.
		Actor->RebuildMesh();
		const int32 TrisBefore = Actor->GetPresenter()->SurfaceTriangleCountForTest();
		TestTrue(TEXT("the road produced a surface to measure"), TrisBefore > 0);
		const FString Slot = TEXT("AirportOpsTest_Runtime");
		if (!TestTrue(TEXT("save writes"), Runtime->SaveToSlot(Slot))) { return false; }

		Actor->ClearNetwork();
		Actor->RebuildMesh();
		TestEqual(TEXT("cleared network has no nodes"), Actor->Network->GetNodes().Num(), 0);
		TestEqual(TEXT("and no surface"), Actor->GetPresenter()->SurfaceTriangleCountForTest(), 0);

		if (!TestTrue(TEXT("load reads"), Runtime->LoadFromSlot(Slot))) { return false; }
		// 4, not 2: the road's own two PLUS the runway's two, added for issue #105's follow-up
		// comment above - see its own comment.
		TestEqual(TEXT("the nodes are back"), Actor->Network->GetNodes().Num(), 4);
		TestEqual(TEXT("and the surface mesh was rebuilt from them"), Actor->GetPresenter()->SurfaceTriangleCountForTest(), TrisBefore);
		TestEqual(TEXT("agents do not survive a load - they were never saved"), Actor->GetTraffic()->GetAgentCount(), 0);
		TestFalse(TEXT("a load is a new undo baseline"), Actor->CanUndo());
		TestFalse(TEXT("a missing slot is refused, not a crash"), Runtime->LoadFromSlot(TEXT("AirportOpsTest_NoSuchRuntimeSlot")));
	}
	return true;
}

namespace
{
	/**
	 * Records every call rather than answering one - so a test can assert NONE happened,
	 * which a purse that merely returns a fixed answer cannot prove: a stale pointer that
	 * silently keeps saying "yes" would pass a value-only assertion just as well as a purse
	 * that was never asked.
	 */
	class FSpyPurse : public IBuildPurse
	{
	public:
		int32 CanAffordCalls = 0;

		virtual bool CanAfford(const FBuildQuote& Quote) const override
		{
			++const_cast<FSpyPurse*>(this)->CanAffordCalls;
			return true;
		}
		virtual int32 Charge(const FBuildQuote& Quote) override { return INDEX_NONE; }
		virtual void Reverse(int32 ChargeId) override {}
		virtual void Credit(const FBuildQuote& Quote) override {}
		virtual FText Describe(const FBuildQuote& Quote) const override { return FText(); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsRuntimeDetachClearsPurseTest,
	"AirportOps.Present.RuntimeDetachClearsPurse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsRuntimeDetachClearsPurseTest::RunTest(const FString& Parameters)
{
	// ISSUE #193: Attach wires Facade->SetPurse(Ledger) but until this fix Detach never
	// called SetPurse(nullptr) to match, so the facade kept a raw IBuildPurse* pointing at
	// a purse this runtime no longer owns. A SPY rather than the real Ledger, so the
	// assertion is "the old purse is never asked again" rather than "CanAfford happens to
	// still return the right answer" - the second is true of a stale pointer purely by luck
	// until the ledger it points at is gone.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world to spawn into"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }

	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);

	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestNotNull(TEXT("Attach wired a facade"), Facade)) { return false; }

	// SUBSTITUTED AFTER Attach, standing in for the ledger it actually wired: the point
	// under test is Detach's own contract with whatever purse is currently set, not which
	// object that happens to be in production.
	FSpyPurse Spy;
	Facade->SetPurse(&Spy);

	// Detach IS PRIVATE - Attach's own first line is its only caller, as a re-entry guard
	// for whatever was wired before. Attach(nullptr) reaches it the same way a second real
	// Attach would: Detach() runs against the STILL-SET Target below, and only afterwards
	// does Target become null and the early-return fire (see Attach's own body) - so nothing
	// past this point rewires the purse and the facade is left exactly where Detach put it.
	Runtime->Attach(nullptr);

	TestNull(TEXT("Detach clears the purse"), Facade->GetPurse());

	FBuildQuote Quote;
	Quote.Lines.Add({ nullptr, EBuildUnit::Each, 100.0, 1.0, {} });
	TestTrue(TEXT("with no purse, CanAfford treats it as free"), Facade->CanAfford(Quote));
	TestEqual(TEXT("and never dereferences the detached purse to decide that"),
		Spy.CanAffordCalls, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsRuntimeLandNearTest,
	"AirportOps.Present.LandNear",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsRuntimeLandNearTest::RunTest(const FString& Parameters)
{
	// THE SEAM ISSUE #191 INTRODUCED: LandNear replaced ~90 lines split across
	// ARoadBuildController::LandAircraftNearViewFocus and LandThroughTheBoard, which no test
	// exercised directly because both lived on a PlayerController. This measures the two facts
	// that matter about the move - that LandNear actually reaches UFlightBoard::AcceptImmediate
	// rather than silently doing nothing, and that it resolves the RIGHT airframe (the content
	// default, or the caller's override) - without needing a runway, a stand or a route: those
	// are AcceptImmediate's and ArrivalPlanner's own concerns, already covered by
	// AirportOps.Model.FlightBoard.AcceptImmediate and Airside.Model.ArrivalPlanner.
	//
	// A network with NO runway is used ON PURPOSE, not as a shortcut: AcceptImmediate always
	// records the flight it built (UFlightBoard::AddOffer) before asking ArrivalPlanner
	// whether it can be accepted, so the refusal - NoRunway, deterministic and needing no
	// fixture - is exactly as good a vantage point as an accepted landing for watching what
	// airframe reached the board.
	{
		// UNATTACHED: Target is null, the same state the editor mode's runtime never reaches
		// (it has none) but a freshly-constructed one starts in. Must refuse rather than crash.
		UOpsRuntime* Unattached = NewObject<UOpsRuntime>();
		const EArrivalRefusal Why = Unattached->LandNear(FVector2D(100.0, 100.0), nullptr);
		TestEqual(TEXT("no attached network: refused as NoRunway rather than crashing"),
			Why, EArrivalRefusal::NoRunway);
		TestEqual(TEXT("and nothing was ever offered to a board with no target to check"),
			Unattached->GetFlightBoard()->Offers().Num(), 0);
	}

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world to spawn into"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }

	// A NODE, NOT NOTHING: Actor->Network is null until the first edit (URoadEditFacade::
	// EnsureNetwork is lazy), and LandNear's own null guard would otherwise refuse here for a
	// reason that has nothing to do with the one this test means to measure. No runway either
	// way - see the class comment on why that refusal is the deliberately chosen vantage point.
	Actor->PlaceNode(FVector2D::ZeroVector);
	if (!TestNotNull(TEXT("the actor has a network"), Actor->Network.Get())) { return false; }

	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);

	const EArrivalRefusal FirstWhy = Runtime->LandNear(FVector2D(500.0, 500.0), nullptr);
	TestEqual(TEXT("still NoRunway - this network has none - proving the refusal came from "
		"ArrivalPlanner and not from LandNear's own null guards"), FirstWhy, EArrivalRefusal::NoRunway);

	TArray<UFlight*> Offers = Runtime->GetFlightBoard()->Offers();
	if (!TestEqual(TEXT("one flight was made and offered despite the refusal - AcceptImmediate "
		"records before it asks, and LandNear must reach that far"), Offers.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("with no override, LandNear resolved the content default airframe"),
		Offers[0]->Airframe.TypeCode, UAirsideSettings::ResolveDefaultAirframe().TypeCode);

	// NOW WITH AN OVERRIDE - standing in for the type the driver's Land panel passes (it was
	// DefaultGame.ini's LandAircraftType until 2026-09-27). A DISTINCT TypeCode, so the assertion
	// below can only pass if LandNear actually used it rather than falling back to the default -
	// the exact regression a controller-side change to that fallback logic could reintroduce.
	FAirframe Override;
	Override.TypeCode = FName(TEXT("Airside_LandNearTest_Override"));
	Override.Wingspan = 4321.0;
	const EArrivalRefusal SecondWhy = Runtime->LandNear(FVector2D(-500.0, -500.0), &Override);
	TestEqual(TEXT("still NoRunway, same network"), SecondWhy, EArrivalRefusal::NoRunway);

	Offers = Runtime->GetFlightBoard()->Offers();
	if (!TestEqual(TEXT("a second flight was offered"), Offers.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("with an override, LandNear used IT rather than the content default - the "
		"one fact that proves the caller's FAirframe* actually reaches AcceptImmediate"),
		Offers[1]->Airframe.TypeCode, Override.TypeCode);

	return true;
}

// NAMED, NOT ANONYMOUS - the tests module is a UNITY build; see StandAllocatorTest.cpp.
namespace OpsRuntimeStandLoadTest
{
	/** The one live stand on Actor's network, by index, or INDEX_NONE. */
	int32 OnlyStand(const ARoadNetworkActor* Actor)
	{
		const TArray<FEntityInstance>& Entities = Actor->Network->GetEntities();
		for (int32 Index = 0; Index < Entities.Num(); ++Index)
		{
			if (Entities[Index].bAlive && Entities[Index].IsStand()) { return Index; }
		}
		return INDEX_NONE;
	}
}

/**
 * FINAL REVIEW C2: A DRAWN D STAND SURVIVES A SAVE AND A LOAD IN A NEW SESSION. OpsSave writes
 * FEntityInstance::Definition as a PATH, and a D/E/F definition is a transient object built by
 * ARoadNetworkActor::ResolveStandDefinitionFor - in a new session there is nothing at that path,
 * the restored stand's Definition came back null, FAnchorLink::Gather skipped it, and the
 * Inspector said "NOT reachable" over a stand the player could see. The load now rebinds every
 * stand's definition from its own outline's letter (ARoadNetworkActor::RebindStandDefinitions).
 *
 * THE NEW SESSION IS SIMULATED BY RENAMING the saved definition out of the path the save
 * recorded, which is exactly what a fresh process looks like to the archive's path lookup -
 * and the load goes into a SECOND actor in a second world, whose definition cache is empty.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsRuntimeDrawnStandSurvivesLoadTest,
	"AirportOps.Present.RuntimeLoad.DrawnStandSurvivesLoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsRuntimeDrawnStandSurvivesLoadTest::RunTest(const FString& Parameters)
{
	using namespace OpsRuntimeStandLoadTest;
	const FString Slot = TEXT("AirportOpsTest_DrawnStand");

	UEntityDefinition* SavedDefinition = nullptr;
	{
		FAirsideTestWorld TestWorld;
		ARoadNetworkActor* Actor = TestWorld.Actor;
		if (!TestNotNull(TEXT("an actor to draw on"), Actor)) { return false; }
		Actor->ClearNetwork();
		Actor->StandDefinition = UEntityDefinition::MakeStandTransient();

		IRoadEditTarget* Target = Actor;
		const int32 West = Target->PlaceNode(FVector2D(-10000.0, 0.0));
		const int32 East = Target->PlaceNode(FVector2D(20000.0, 0.0));
		Target->ConnectNodes(West, East, ERoadKind::Taxiway, INDEX_NONE);

		// A CODE D FLOOR, entrance behind the taxiway's clearance strip (2026-09-28; it was
		// 10 m off the centreline, over the pavement edge, and is refused now) - D because C
		// alone has an authored asset and would come back by path in any session.
		const double Width = IcaoCode::StandWidthForLetter(EIcaoCode::D);
		const double Depth = IcaoCode::StandDepthForLetter(EIcaoCode::D);
		const FRoadSegmentId Taxi = Actor->Network->SegmentIdAt(0);
		const double EntranceY = Actor->Network->GetSegment(Taxi)->Profile->GetHalfWidthLeft()
			+ TaxiwayStrip::StripWidthOf(*Actor->Network, Taxi);
		const FVector2D A(0.0, EntranceY), B(Width, EntranceY);
		const TArray<FVector2D> Rect = { A, B, FVector2D(Width, EntranceY + Depth), FVector2D(0.0, EntranceY + Depth) };
		const int32 Index = Target->PlaceStandInPlot(Rect, A, B, EPavement::Tarmac);
		if (!TestTrue(TEXT("a Code D stand is drawn"), Index != INDEX_NONE)) { return false; }
		Actor->RebuildMesh();

		FStandFacts Before;
		if (!TestTrue(TEXT("the stand describes"), InspectFacts::DescribeStand(nullptr, *Actor->Network, Index, Before))
			|| !TestTrue(TEXT("and is reachable before the save, or the load proves nothing"), Before.bReachable))
		{
			return false;
		}
		SavedDefinition = Actor->Network->GetEntities()[Index].Definition;

		UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
		Runtime->Attach(Actor);
		if (!TestTrue(TEXT("save writes"), Runtime->SaveToSlot(Slot))) { return false; }

		// RENAMED WHILE ITS WORLD IS STILL UP, so nothing between here and the load can see
		// the object in a half-torn-down outer.
		if (!TestNotNull(TEXT("the drawn stand had a definition to save"), SavedDefinition)) { return false; }
		SavedDefinition->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	}

	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a fresh actor to load into"), Actor)) { return false; }
	// A NETWORK TO LOAD INTO - a fresh actor makes one lazily, and LoadFromSlot refuses
	// without one rather than inventing it.
	Actor->ClearNetwork();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	if (!TestTrue(TEXT("load reads"), Runtime->LoadFromSlot(Slot))) { return false; }

	const int32 Index = OnlyStand(Actor);
	if (!TestTrue(TEXT("the stand came back"), Index != INDEX_NONE)) { return false; }
	const FEntityInstance& Stand = Actor->Network->GetEntities()[Index];
	TestNotNull(TEXT("with a definition, rebound from its own outline's letter"), Stand.Definition.Get());
	TestTrue(TEXT("and not the saved session's object, which a new session would not have"),
		Stand.Definition.Get() != SavedDefinition);

	FStandFacts After;
	TestTrue(TEXT("the loaded stand describes"), InspectFacts::DescribeStand(nullptr, *Actor->Network, Index, After));
	TestTrue(TEXT("and is REACHABLE - the Inspector's own query, which read NOT reachable"), After.bReachable);
	return true;
}

/**
 * FINAL REVIEW C2, second half: OpsSave::Restore runs Serialize, never PostLoad, so the
 * outline migration URoadNetwork::PostLoad runs for a LEVEL never ran for a SAVE GAME - a
 * stand saved before stands had outlines came back outline-less, and every "a stand has an
 * outline" rule downstream (overlap, paint, rebind) quietly skipped it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsRuntimeLegacyStandGetsOutlineOnLoadTest,
	"AirportOps.Present.RuntimeLoad.LegacyStandGetsOutline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsRuntimeLegacyStandGetsOutlineOnLoadTest::RunTest(const FString& Parameters)
{
	using namespace OpsRuntimeStandLoadTest;

	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->ClearNetwork();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();

	IRoadEditTarget* Target = Actor;
	const int32 Placed = Target->PlaceStand(FVector2D(1000.0, 2000.0), FMath::DegreesToRadians(90.0));
	if (!TestTrue(TEXT("a stand is placed"), Placed != INDEX_NONE)) { return false; }

	// BACK TO NO OUTLINE, the shape of a save from before stands had one - the same
	// construction Airside.Model.StandOutline.LegacyGetsCodeCBox uses, since placement itself
	// now always gives one.
	FRoadNetworkTestAccess Access(*Actor->Network);
	if (!TestTrue(TEXT("the outline is cleared"), Access.SetEntityOutlineForTest(Actor->Network->EntityIdAt(Placed), {})))
	{
		return false;
	}

	const FString Slot = TEXT("AirportOpsTest_LegacyStand");
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	if (!TestTrue(TEXT("save writes"), Runtime->SaveToSlot(Slot))) { return false; }
	if (!TestTrue(TEXT("load reads"), Runtime->LoadFromSlot(Slot))) { return false; }

	const int32 Index = OnlyStand(Actor);
	if (!TestTrue(TEXT("the stand came back"), Index != INDEX_NONE)) { return false; }
	TestEqual(TEXT("with the Code C box its pose implies - four corners"),
		Actor->Network->GetEntities()[Index].Outline.Num(), 4);
	return true;
}

/**
 * REVIEW I1 (2026-09-28): the clock's queue is not saved and holds ABSOLUTE due times, so the
 * repeating offer tick and upkeep booked at Attach still pointed at the PRE-load time after a
 * load. A save two days ahead fired the minute tick ~2,880 times in one frame (a burst, and
 * hundreds of DroppedOffers C would read as unmet demand); a save behind went silent until the
 * clock caught up. RearmRepeatingSchedules re-books both from the loaded Now.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsRuntimeRearmsRepeatersOnLoadTest,
	"AirportOps.Present.OffersRearmOnLoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsRuntimeRearmsRepeatersOnLoadTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world to spawn into"), TestWorld.World)) { return false; }
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(TestWorld.Actor);
	if (!TestTrue(TEXT("the minute tick is armed"), Runtime->HasOfferScheduledForTest())) { return false; }

	// A SAVE FROM TWO DAYS LATER, restored into the runtime's own clock the way OpsSave does:
	// GameSeconds jumps, the queue does not.
	USimClock* Later = NewObject<USimClock>();
	Later->SetUniformDay(1.0);
	Later->StartAtHour(9.0);
	Later->Advance(2.0);
	TArray<uint8> Bytes;
	OpsSave::SerializeObject(*Later, Bytes);
	OpsSave::DeserializeObject(*Runtime->GetClock(), Bytes);
	Runtime->GetClock()->SetUniformDay(USimClock::SecondsPerDay);   // 1 game s per real s from here

	Runtime->RearmRepeatingSchedules();
	const int32 Before = Runtime->OfferTicksForTest();
	Runtime->Tick(1.0);
	TestEqual(TEXT("one game second after the load fires no backlog of minute ticks"),
		Runtime->OfferTicksForTest() - Before, 0);
	Runtime->Tick(60.0);
	TestEqual(TEXT("and the next one comes a minute after the loaded time, not days away"),
		Runtime->OfferTicksForTest() - Before, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsRuntimeTicksTheQueueTest,
	"AirportOps.Present.RuntimeTicksTheQueue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsRuntimeTicksTheQueueTest::RunTest(const FString& Parameters)
{
	// THE SEAM, at the composition (review I2): the queue exists only if the runtime wires a
	// sequencer into its board and ticks the queue every frame.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world to spawn into"), TestWorld.World)) { return false; }
	// A NETWORK, which a fresh actor lacks until its first edit - and Tick reaches the model
	// only through one.
	TestWorld.Actor->PlaceNode(FVector2D(0.0, 0.0));
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(TestWorld.Actor);
	TestNotNull(TEXT("the board has a sequencer"), Runtime->GetFlightBoard()->Sequencer.Get());
	const int32 Before = Runtime->GetFlightBoard()->TickQueueCallsForTest();
	Runtime->Tick(0.1);
	Runtime->Tick(0.1);
	TestEqual(TEXT("every runtime tick ticks the queue"), Runtime->GetFlightBoard()->TickQueueCallsForTest() - Before, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsRuntimeWiresLitresTest,
	"AirportOps.Fuel.RuntimeWiresLitresOwed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsRuntimeWiresLitresTest::RunTest(const FString& Parameters)
{
	// THE SEAM (spec 2026-09-28-fuel-litres section 2): the litres a flight was offered at reach
	// the fuel demand only if the runtime points the fuel service at the board.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world to spawn into"), TestWorld.World)) { return false; }
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(TestWorld.Actor);
	if (!TestTrue(TEXT("the fuel service has a litres seam"), static_cast<bool>(Runtime->GetJobBoard()->LitresOwedFor)))
	{
		return false;
	}
	UFlight* Flight = NewObject<UFlight>();
	Flight->AgentId = 42;
	Flight->Phase = EFlightPhase::Landing;
	Flight->FuelLitres = 321.0;
	Runtime->GetFlightBoard()->AddOffer(*Runtime->GetClock(), Flight);
	FAirframe Airframe;
	Airframe.FuelCapacityLitres = 1000.0;
	TestEqual(TEXT("the flight's own litres reach the demand"),
		Runtime->GetJobBoard()->LitresOwedFor(42, Airframe), 321.0, 1e-9);
	TestEqual(TEXT("an agent no flight owns gets the stated fallback"),
		Runtime->GetJobBoard()->LitresOwedFor(7, Airframe), UJobBoard::DefaultLitres(Airframe), 1e-9);
	return true;
}

#endif
