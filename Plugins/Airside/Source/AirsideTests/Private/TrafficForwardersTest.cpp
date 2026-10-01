#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Build/AnchorLink.h"
#include "Content/AirsideSettings.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadAgentActor.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficForwardersTest,
	"Airside.Present.TrafficForwarders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficForwardersTest::RunTest(const FString& Parameters)
{
	// THE SEAM TEST for the Mediator split: every name on UAirsideTraffic must reach
	// UGroundTraffic, the view must appear and vanish on the model's own phase events, and
	// the phase relay must re-broadcast - because AirportOps binds to UAirsideTraffic's for
	// the phase, and a relay that was never wired would leave the flight board deaf without
	// any compile error to say so. The refusal is the MODEL's own delegate since #445 item 6
	// (the presenter's pure relay of it was cut), so it is heard there, through GetModel().
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world to spawn into"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	URoadNetwork& Net = *Actor->Network;

	UAirsideTraffic* Traffic = Actor->GetTraffic();
	if (!TestNotNull(TEXT("the actor has a traffic object"), Traffic)) { return false; }
	UGroundTraffic* Model = Traffic->GetModel();
	if (!TestNotNull(TEXT("and it owns a model"), Model)) { return false; }
	TestEqual(TEXT("the model's outer is THIS traffic object, not the CDO's (duplication rule)"), Model->GetOuter(), static_cast<UObject*>(Traffic));

	const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 0.0), false);
	const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(30000.0, 0.0), false);
	// AUTHORED (bDerived false) for the reason AgentRedirectTest names: a surface rebuild
	// sweeps DERIVED guidelines, and a fixture that vanished mid-test would look like a
	// broken forwarder.
	TestGraph::Join(Net, A, B, { EGuidelineDir::Bidirectional, nullptr, false });
	// #312: was a hand-built FRouteQuery that skipped AvoidRunways.
	const FRoutePlan Plan = TestGraph::Probe(Net, A, B, ETraversalClass::GroundVehicle);
	if (!TestTrue(TEXT("route found"), Plan.IsValid())) { return false; }

	TArray<TPair<EAgentPhase, EAgentPhase>> Relayed;
	Traffic->OnAgentPhaseChanged.AddLambda([&Relayed](const FAgentTransition& T) { Relayed.Emplace(T.From, T.To); });

	// A REAL VEHICLE since 2026-09-23: it used to be the default airframe with Climb cleared
	// so no departure could arm; an FVehicle has no climb, so none can.
	const FVehicle Van = UAirsideSettings::ResolveDefaultVehicle();
	if (!TestTrue(TEXT("dispatch through the actor is accepted"), Actor->DispatchAgent(Plan, Van, ETraversalClass::GroundVehicle))) { return false; }

	const int32 Id = Traffic->GetNewestAgentId();
	TestTrue(TEXT("the id is assigned by the model"), Id >= 1 && Model->GetNewestAgentId() == Id);
	TestEqual(TEXT("the model has the agent"), Model->GetAgentCount(), 1);
	TestEqual(TEXT("GetAgentCount forwards"), Traffic->GetAgentCount(), 1);
	const FRoadAgent* Agent = Model->FindAgent(Id);
	if (!TestNotNull(TEXT("FindAgent"), Agent)) { return false; }
	TestEqual(TEXT("the class the tool asked for reached the model"), Agent->Class, ETraversalClass::GroundVehicle);
	TestEqual(TEXT("the goal node is recorded for replans"), Agent->GoalNode, B);
	TestEqual(TEXT("the spawn was relayed as Gone -> Taxiing"), Relayed.Num(), 1);
	if (Relayed.Num() == 1) { TestTrue(TEXT("with the right phases"), Relayed[0].Key == EAgentPhase::Gone && Relayed[0].Value == EAgentPhase::Taxiing); }

	ARoadAgentActor* View = Traffic->GetNewestAgent();
	if (!TestNotNull(TEXT("a view was spawned on the phase event"), View)) { return false; }
	const FVector Before = View->GetActorLocation();

	for (int32 Tick = 0; Tick < 40; ++Tick) { Traffic->Advance(0.05f, 0.0, &Net, Actor->TrafficRules); }
	TestTrue(TEXT("the view moved: Advance forwards and pushes LastMotion to the view"),
		FVector::Dist(View->GetActorLocation(), Before) > 10.0);
	TestTrue(TEXT("and the model's own position agrees with the view"),
		FVector2D::Distance(Traffic->LastAgentPositionForTest(), FVector2D(View->GetActorLocation().X, View->GetActorLocation().Y)) < 1.0);

	// THE RULES SEAM. FTrafficRules is a level-authored UPROPERTY on the ACTOR - the only
	// object here the .umap saves - and the model that arbitrates on it is Transient and
	// re-pointed on a PIE duplication, so the figures have to travel down the tick. Measured
	// through ARoadNetworkActor::Tick rather than by calling Advance directly, because the
	// forwarder under test is the actor's own line: with it deleted the model keeps its
	// constructor default and every number a designer typed is silently ignored.
	Actor->TrafficRules.VehicleGap = 777.0;
	Actor->Tick(0.05f);
	TestEqual(TEXT("a figure set on the ACTOR reaches the model's rules within one tick"),
		Model->Rules.VehicleGap, 777.0, 1e-9);

	// MaxSubstepSeconds/MaxSubsteps THE SAME WAY (#107 item 6): both used to be
	// UPROPERTY(EditAnywhere) on UGroundTraffic itself, which is Transient and never exposed
	// EditAnywhere one layer up - so a designer could type into a field the Details panel
	// would never show, and it would do nothing. Living on FTrafficRules instead means this
	// same seam - already proven above for VehicleGap - covers them for free; asserted
	// explicitly anyway because "for free" is exactly the kind of claim CLAUDE.md's "check
	// where a list is CONSUMED" says to measure rather than assume.
	Actor->TrafficRules.MaxSubstepSeconds = 0.01;
	Actor->TrafficRules.MaxSubsteps = 3;
	Actor->Tick(0.05f);
	TestEqual(TEXT("MaxSubstepSeconds set on the ACTOR reaches the model's rules within one tick"),
		Model->Rules.MaxSubstepSeconds, 0.01, 1e-9);
	TestEqual(TEXT("MaxSubsteps too"), Model->Rules.MaxSubsteps, 3);

	// THE REBUILD SEAM, at the level of the composition: ARoadNetworkActor::RebuildMesh must
	// call UGroundTraffic::OnGraphRebuilt. Airside.Model.Traffic.GraphRebuild pins what that
	// function DOES, but it calls it by hand, so deleting the actor's three lines left every
	// agent holding freed guideline handles with all 118 tests green. A road edit through the
	// actor - PlaceNode, ConnectNodes, then the RebuildMesh every build tool calls after one
	// (see RoadDrawTool.cpp) - is the real path, and the summary is the evidence it ran.
	//
	// FAR FROM THE VAN'S ROUTE, so what is measured is the CALL and not a re-route: the new
	// pavement derives its own guidelines 100 km away, the van's authored A-B line survives
	// the sweep by handle, and its plan comes through Intact.
	const int32 FarA = Actor->PlaceNode(FVector2D(-100000.0, -80000.0));
	const int32 FarB = Actor->PlaceNode(FVector2D(-100000.0, -60000.0));
	if (TestTrue(TEXT("two road nodes placed through the actor"), FarA != INDEX_NONE && FarB != INDEX_NONE))
	{
		TestTrue(TEXT("and connected"), Actor->ConnectNodes(FarA, FarB));
		Actor->RebuildMesh();
		TestTrue(TEXT("the rebuild reached the traffic model: at least one agent re-resolved"),
			Model->GetLastRebuildSummaryForTest().ReResolved >= 1);
		TestEqual(TEXT("and the van was not stranded by pavement 100 km away"),
			Model->GetLastRebuildSummaryForTest().Stranded, 0);
	}

	TestTrue(TEXT("RetireAgent forwards"), Traffic->RetireAgent(Id));
	TestEqual(TEXT("the model dropped it"), Model->GetAgentCount(), 0);
	TestNull(TEXT("and the view is gone"), Traffic->GetNewestAgent());
	TestEqual(TEXT("removal was relayed as Taxiing -> Gone"), Relayed.Num(), 2);

	// THE ARRIVAL REFUSAL IS NOT RE-ASSERTED HERE: the actor's DispatchArrival refused on a runway-less
	// network and the model's OnArrivalRefused announced it - Airside.Present.ArrivalRefusedEvent makes the
	// same call and also measures the reason and the plan's sentence, so the four lines that stood here
	// (#462 M29) could not fail without it failing first.
	// ENFORCED BY: Airside.Present.ArrivalRefusedEvent

	// AND THE ROUTING SIDE OF THE SAME SEAM. Spec §4: "vehicles always route with the table,
	// aircraft never do" - the aircraft's route is fixed at clearance. That rule lives in
	// URoadEditFacade::FindRoute, which is the only production caller with a traffic object to
	// hand, so it can only be measured HERE and only through the actor. It is measured with a
	// phantom claim rather than a second agent because what is under test is whether the QUERY
	// carries the table at all: a real queue would take a whole convoy to build, and the answer
	// would then depend on the arbiter's timing as well as on this one line.
	//
	// A DIAMOND, well away from A-B above so neither route can borrow the other's edges: the
	// direct line RA->RC is 40000 uu and the detour through RD is 41231, so shortest-path takes
	// the direct one by 1231 uu. The phantom holds 20000 uu of the direct edge, which at
	// CongestionWeight 2.0 adds 40000 to its cost - far more than the detour is longer by.
	const FGuidelineNodeId RA = Net.AddGuidelineNode(FVector2D(0.0, 60000.0), false);
	const FGuidelineNodeId RC = Net.AddGuidelineNode(FVector2D(40000.0, 60000.0), false);
	const FGuidelineNodeId RD = Net.AddGuidelineNode(FVector2D(20000.0, 65000.0), false);
	const FGuidelineEdgeId Direct = TestGraph::Join(Net, RA, RC, { EGuidelineDir::Bidirectional, nullptr, false });
	TestGraph::Join(Net, RA, RD, { EGuidelineDir::Bidirectional, nullptr, false });
	TestGraph::Join(Net, RD, RC, { EGuidelineDir::Bidirectional, nullptr, false });

	FTrafficClaim Phantom;
	Phantom.AgentId = 9999;                                  // nobody: no agent has this id
	Phantom.Resource = FTrafficResource::OfEdge(Direct);
	Phantom.From = 0.0;
	Phantom.To = 20000.0;
	Phantom.bOccupied = true;
	FTrafficClaim Blocker;
	Model->OccupancyForTest().TryClaim(Phantom, Blocker);

	// THE ERRAND, NOT THE CLASS, since 2026-09-21. This pair has always asserted that a van
	// sent to a job is costed by the occupancy table and an aeroplane's route is not; the
	// rule now lives in FRoutePolicy rather than in an ETraversalClass branch inside
	// FindRoute, and the assertion below is unchanged.
	const FRoutePlan VanRoute = Actor->FindRoute(RA, RC, ETraversalClass::GroundVehicle, 0.0, ERouteErrand::VehicleToJob);
	const FRoutePlan PlaneRoute = Actor->FindRoute(RA, RC, ETraversalClass::Aircraft, 0.0, ERouteErrand::PlayerIssued);
	if (TestTrue(TEXT("both classes find a route across the diamond"), VanRoute.IsValid() && PlaneRoute.IsValid()))
	{
		// THE TWO ANSWERS DIFFER, and that difference IS the forwarder: one query carried the
		// table and the other did not. Asserted on the step count and on the edge, not on the
		// length, so a failure names which line the router took.
		TestEqual(TEXT("the van routes round the queue: two steps, via RD"), VanRoute.Steps.Num(), 2);
		TestTrue(TEXT("and so does not touch the held edge"),
			VanRoute.Steps.Num() == 2 && VanRoute.Steps[0].Edge != Direct && VanRoute.Steps[1].Edge != Direct);
		TestEqual(TEXT("the aircraft takes the direct edge regardless: its route is fixed at clearance"),
			PlaneRoute.Steps.Num(), 1);
		if (PlaneRoute.Steps.Num() == 1)
		{
			TestEqual(TEXT("which is the held one"), PlaneRoute.Steps[0].Edge, Direct);
		}
	}
	Model->OccupancyForTest().Clear();

	// AND THE SAME UNDER DUPLICATION, which is how play-in-editor makes its copy of the
	// level. Model is a Transient non-instanced pointer, so a duplicate arrives holding the
	// CDO's subobject unless PostInitProperties re-points it - the 2026-09-06 PIE bug, one
	// layer further down. The RELAY is the half a pointer check alone would miss: bound in
	// the constructor it would be bound to the CDO's model, and the duplicate's own agents
	// would spawn no view and tell AirportOps nothing, with no compile error to say so.
	ARoadNetworkActor* Dup = DuplicateObject<ARoadNetworkActor>(Actor, TestWorld.World->PersistentLevel);
	if (!TestNotNull(TEXT("the actor duplicates"), Dup)) { return false; }
	UAirsideTraffic* DupTraffic = Dup->GetTraffic();
	if (!TestNotNull(TEXT("the duplicate has a traffic object"), DupTraffic)) { return false; }
	if (!TestNotNull(TEXT("and a model"), DupTraffic->GetModel())) { return false; }
	TestEqual(TEXT("whose outer is the DUPLICATE's traffic, not the CDO's"),
		DupTraffic->GetModel()->GetOuter(), static_cast<UObject*>(DupTraffic));
	TestNotEqual(TEXT("and which is a different object from the source's model"),
		static_cast<UObject*>(DupTraffic->GetModel()), static_cast<UObject*>(Model));

	int32 DupRelayed = 0;
	DupTraffic->OnAgentPhaseChanged.AddLambda([&DupRelayed](const FAgentTransition&) { ++DupRelayed; });
	const FRoutePlan DupPlan = TestGraph::Probe(*Dup->Network, A, B, ETraversalClass::GroundVehicle);
	if (TestTrue(TEXT("the duplicate's own graph still routes"), DupPlan.IsValid()))
	{
		TestTrue(TEXT("dispatch on the duplicate is accepted"), Dup->DispatchAgent(DupPlan, Van, ETraversalClass::GroundVehicle));
		TestEqual(TEXT("the duplicate's relay is bound to the duplicate's model"), DupRelayed, 1);
		TestNotNull(TEXT("and its view was spawned"), DupTraffic->GetNewestAgent());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPhaseRelayShowsTheViewFirstTest,
	"Airside.Present.PhaseRelayShowsTheViewFirst",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPhaseRelayShowsTheViewFirstTest::RunTest(const FString& Parameters)
{
	// THE ONE RELAY LEFT ON UAirsideTraffic, AND WHAT IT ADDS (#445 item 6). The other four were pure forwarders and were cut: a
	// listener binds the model's delegate through GetModel(). This one stays because it is not pure - it spawns the agent's cube
	// before re-broadcasting the birth and destroys it before re-broadcasting the death - and this measures exactly that, from
	// INSIDE the listener: on Gone -> X the view the event describes is already standing, and on X -> Gone it is already gone. A
	// relay reduced to a forwarder (or one that broadcast first) would hand the listener a phase with no view, or a dead one.
	// Mutation-checked 2026-10-01: SpawnView skipped in OnModelPhaseChanged, this went red ("no view at birth").
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world to spawn into"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	URoadNetwork& Net = *Actor->Network;
	UAirsideTraffic* Traffic = Actor->GetTraffic();
	if (!TestNotNull(TEXT("the actor has a traffic object"), Traffic)) { return false; }

	const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 0.0), false);
	const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(30000.0, 0.0), false);
	// AUTHORED (bDerived false), as TrafficForwarders' own fixture is: a surface rebuild sweeps derived guidelines.
	TestGraph::Join(Net, A, B, { EGuidelineDir::Bidirectional, nullptr, false });
	const FRoutePlan Plan = TestGraph::Probe(Net, A, B, ETraversalClass::GroundVehicle);
	if (!TestTrue(TEXT("route found"), Plan.IsValid())) { return false; }

	int32 Births = 0;
	int32 Deaths = 0;
	bool bViewAtBirth = false;
	bool bViewAtDeath = true;
	Traffic->OnAgentPhaseChanged.AddLambda([&, Traffic](const FAgentTransition& T)
	{
		if (T.From == EAgentPhase::Gone) { ++Births; bViewAtBirth = Traffic->GetAgentView(T.AgentId) != nullptr; }
		if (T.To == EAgentPhase::Gone) { ++Deaths; bViewAtDeath = Traffic->GetAgentView(T.AgentId) != nullptr; }
	});

	const FVehicle Van = UAirsideSettings::ResolveDefaultVehicle();
	if (!TestTrue(TEXT("dispatch is accepted"), Actor->DispatchAgent(Plan, Van, ETraversalClass::GroundVehicle))) { return false; }
	if (!TestEqual(TEXT("the birth was heard once, through the relay"), Births, 1)) { return false; }
	TestTrue(TEXT("no view at birth would mean the relay broadcast before it spawned - the view stands when the listener hears Gone -> X"), bViewAtBirth);

	TestTrue(TEXT("the agent retires"), Traffic->RetireAgent(Traffic->GetNewestAgentId()));
	if (!TestEqual(TEXT("the death was heard once, through the relay"), Deaths, 1)) { return false; }
	TestFalse(TEXT("the view is gone by the time the listener hears X -> Gone"), bViewAtDeath);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * DeltaSeconds CROSSES THE Present/Model SEAM AS A double, NOT A float (#107 item 5).
 *
 * CONFIRMED, 2026-09-12 review. UAirsideTraffic::Advance used to take DeltaSeconds as a
 * float, and ARoadNetworkActor::Tick fed it static_cast<float>(Evened * SimTimeScale) - both
 * unnecessary narrowings of a value UGroundTraffic::Advance (and the substep split inside
 * it) has always used as a double. float(1.0/30.0) is very slightly LARGER than the double
 * it should equal, so UGroundTraffic::Advance's `CeilToInt(DeltaSeconds / MaxSubstepSeconds)`
 * rounds UP at every exact multiple: a plain 30 Hz frame at x1 took 2 substeps instead of 1,
 * and 60 Hz at x4 took 3 instead of 2 - work multiplied for no visible reason, at the most
 * ordinary settings in the game.
 *
 * PINNED THROUGH UGroundTraffic::GetLastStepsForTest(), added for exactly this: the split is
 * not otherwise observable, and the follower's own physics do not show a one-step difference
 * reliably (FTrafficSubstepTest's own near-exact SplitGap when the step count DOES match).
 * Calls UAirsideTraffic::Advance directly - the actual seam under test - with a double
 * literal, which narrows to float at the call site under the old signature and stays double
 * under the fixed one; that narrowing IS the defect, so this is the only way to reproduce it
 * rather than re-deriving the arithmetic by hand.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficAdvanceDeltaStaysDoubleTest,
	"Airside.Present.TrafficAdvanceDeltaStaysDouble",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficAdvanceDeltaStaysDoubleTest::RunTest(const FString& Parameters)
{
	UAirsideTraffic* Traffic = NewObject<UAirsideTraffic>(GetTransientPackage());
	if (!TestNotNull(TEXT("traffic constructed"), Traffic)) { return false; }
	UGroundTraffic* Model = Traffic->GetModel();
	if (!TestNotNull(TEXT("and it owns a model"), Model)) { return false; }

	// 30 Hz, x1 - the plainest setting in the game, not a hitch or a high speed multiplier.
	// A true double 1/30 s needs exactly one substep at the default MaxSubstepSeconds.
	Traffic->Advance(1.0 / 30.0, 0.0, nullptr, FTrafficRules());
	TestEqual(TEXT("30 Hz x1 takes exactly one substep, not a spurious second one"),
		Model->GetLastStepsForTest(), 1);

	// 60 Hz, x4 - the issue's second reported example.
	Traffic->Advance(4.0 / 60.0, 0.0, nullptr, FTrafficRules());
	TestEqual(TEXT("60 Hz x4 takes exactly two substeps, not three"),
		Model->GetLastStepsForTest(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceLinkRadiusIsLevelAuthoredTest,
	"Airside.Present.ServiceLinkRadiusIsLevelAuthored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceLinkRadiusIsLevelAuthoredTest::RunTest(const FString& Parameters)
{
	// THE SEAM, and the only test that fails if the property is left unwired. A figure the
	// designer sets on the level that never reaches FAnchorLink is exactly the failure
	// CLAUDE.md's "check where a list is CONSUMED" is about - a knob with nothing on the
	// other end of it, which this codebase has shipped three times.
	//
	// Read off FSurfaceSettings rather than off FAnchorLink, because that struct IS the only
	// route from the actor to the build - see URoadSurfacePresenter::FSurfaceSettings.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the actor"), Actor)) { return false; }

	TestEqual(TEXT("the default is FAnchorLink's own, not a second literal"),
		Actor->ServiceLinkRadius, FAnchorLink::DefaultServiceLinkRadius);

	Actor->ServiceLinkRadius = 777.0;
	TestEqual(TEXT("and a level-authored figure reaches the presenter's settings unchanged"),
		Actor->MakeSurfaceSettingsForTest().ServiceLinkRadius, 777.0);
	return true;
}


// ---------------------------------------------------------------------------------------
/**
 * A CLASS'S GAP IS NEVER LESS THAN HALF ITS FOOTPRINT, WHATEVER THE KNOBS SAY (issue #455, item 6).
 *
 * FTrafficRules::GapFor is what the claim pass stops a refused vehicle short of a node by, and a gap under half the
 * footprint stops it inside the zone where its own claim turns occupied. VehicleGap's default (300) is under half
 * of VehicleFootprint's (669.5), and the footprint is authored from the mesh and has moved four times, so the floor is
 * asserted for the defaults AND for a tuned pair - and a gap above the floor is used as it stands. HERE, IN THE FORWARDERS
 * TEST FILE, because it reads the raw fields to set up and check the premise, and this is the one test file the raw-read
 * lint row (Check-Architecture.ps1 rule 4, 'FTrafficRules Vehicle/AircraftGap') allows to.
 * The premise is asserted too: at the defaults the raw field IS under the floor, or this would pass without the fix.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficRulesGapClearsHalfTheFootprintTest,
	"Airside.Model.Traffic.RulesGapClearsHalfTheFootprint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficRulesGapClearsHalfTheFootprintTest::RunTest(const FString& Parameters)
{
	FTrafficRules Rules;
	TestTrue(TEXT("premise: the authored vehicle gap is under half the vehicle footprint at the defaults"),
		Rules.VehicleGap < Rules.VehicleFootprint * 0.5);
	for (const ETraversalClass Class : { ETraversalClass::Aircraft, ETraversalClass::GroundVehicle })
	{
		TestTrue(*FString::Printf(TEXT("the default gap for class %d is over half its footprint"), static_cast<int32>(Class)),
			Rules.GapFor(Class) > Rules.FootprintFor(Class) * 0.5);
	}
	TestEqual(TEXT("an aircraft's gap is its authored 1500: already over the floor"), Rules.GapFor(ETraversalClass::Aircraft), 1500.0);

	// TUNED: a tiny gap and a big footprint. The floor moves with the footprint, not with a typed number.
	Rules.VehicleGap = 10.0;
	Rules.VehicleFootprint = 850.0;
	TestTrue(TEXT("a level that tuned the gap down and the footprint up still gets a gap over half the footprint"),
		Rules.GapFor(ETraversalClass::GroundVehicle) > 425.0);

	// ABOVE THE FLOOR IT IS USED AS IT STANDS.
	Rules.VehicleGap = 900.0;
	TestEqual(TEXT("a gap above the floor is the gap"), Rules.GapFor(ETraversalClass::GroundVehicle), 900.0);
	return true;
}

// ---------------------------------------------------------------------------------------
// #446's touchdown item: FLandingRun::bTouchedDown is true for the ONE substep that put the wheels down, and
// UAirsideTraffic::Advance used to read it once per FRAME, after a model Advance that runs several substeps at x4 and
// up - so the smoke appeared only when the touchdown happened to fall on a frame's last substep (about a quarter of
// landings at x8). The pin: frames of FOUR substeps each, and four runs whose lead-in of single substeps puts the
// touchdown on each of the four positions in turn. Every run shows exactly one touchdown; the poll showed one of four.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTouchdownShownOncePerLandingTest,
	"Airside.Present.TouchdownShownOncePerLanding",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTouchdownShownOncePerLandingTest::RunTest(const FString& Parameters)
{
	const FAirframe Piper = TestAirframes::Piper();
	FTrafficRules Rules;
	Rules.MaxSubstepSeconds = 1.0 / 60.0;
	Rules.MaxSubsteps = 32;
	constexpr int32 SubstepsPerFrame = 4;
	const double Frame = SubstepsPerFrame * Rules.MaxSubstepSeconds;
	for (int32 LeadIn = 0; LeadIn < SubstepsPerFrame; ++LeadIn)
	{
		// WORLD-FREE, a bare presenter: no smoke component, which is why the count is of touchdowns SHOWN and not of
		// puffs - see TouchdownsShownForTest. SpawnView says "model only" and the model simulates regardless.
		UAirsideTraffic* Traffic = NewObject<UAirsideTraffic>(GetTransientPackage());
		const FTestAirport Airport = FTestAirport::Build(Piper);
		if (!TestTrue(TEXT("the arrival is admitted"),
			Traffic->DispatchArrival(*Airport.Net, Airport.Threshold - FVector2D(1000.0, 0.0), Piper, 0.0, 1.0)))
		{
			return false;
		}
		for (int32 K = 0; K < LeadIn; ++K)
		{
			Traffic->Advance(Rules.MaxSubstepSeconds, 0.0, Airport.Net, Rules);
		}
		int32 Frames = 0;
		bool bPausedChecked = false;
		for (; Frames < 20000 && Traffic->LastAgentPhaseForTest() == EAgentPhase::Arriving; ++Frames)
		{
			Traffic->Advance(Frame, 0.0, Airport.Net, Rules);
			// PAUSED FRAMES RIGHT AFTER THE TOUCHDOWN: the model's moments are emptied at the top of every Advance,
			// the paused early return included - kept past it, the presenter would read this frame's touchdown again on
			// every paused frame and puff each time.
			if (!bPausedChecked && Traffic->TouchdownsShownForTest() == 1)
			{
				bPausedChecked = true;
				Traffic->Advance(0.0, 0.0, Airport.Net, Rules);
				Traffic->Advance(0.0, 0.0, Airport.Net, Rules);
				TestEqual(*FString::Printf(TEXT("lead-in %d: two paused frames after it show it again"), LeadIn),
					Traffic->TouchdownsShownForTest(), 1);
			}
		}
		TestEqual(*FString::Printf(TEXT("lead-in %d: the landing rolled out and vacated"), LeadIn),
			Traffic->LastAgentPhaseForTest(), EAgentPhase::Taxiing);
		TestEqual(*FString::Printf(TEXT("lead-in %d: frames of %d substeps show its touchdown exactly once"),
			LeadIn, SubstepsPerFrame), Traffic->TouchdownsShownForTest(), 1);
	}
	return true;
}

#endif
