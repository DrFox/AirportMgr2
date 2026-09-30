#include "UI/UiWindowHost.h"
#include "CoreMinimal.h"
#include "BuildActions.h"
#include "BuildBarWidget.h"
#include "BuildHudLayer.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Entities/EntityDefinition.h"
#include "Content/AirsideSettings.h"
#include "InspectorFacilityRows.h"
#include "InspectorWidget.h"
#include "Misc/AutomationTest.h"
#include "Model/FacilityPurchases.h"
#include "Model/InspectFacts.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideTraffic.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "Testing/AirsideTestGraph.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/Selection.h"
#include "Profiles/RoadProfile.h"
#include "Solve/IcaoCode.h"
#include "Model/GroundTraffic.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/OpsAlerts.h"
#include "Model/SimClock.h"
#include "Model/TrafficOccupancy.h"
#include "UI/UiButton.h"
#include "UI/UiMenuButton.h"
#include "UIStyle.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorWidgetTest,
	"AirportMgr.Inspector.ReadsFacts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorWidgetTest::RunTest(const FString& Parameters)
{
	// THE PANEL IS WIRED. A real widget, a real actor, a real agent: Depart greys while it
	// taxis and lights when it parks; the title names the aircraft; no selection hides it.
	// Refresh is called directly with the same arguments NativeTick passes, because a
	// headless test has no controller to poll through - the tick-to-Refresh seam is one
	// line and is read, not run, here.
	FAirsideTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	URoadNetwork& Net = *Actor->Network;
	const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 0.0), false);
	const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(20000.0, 0.0), false);
	{
		FGuidelineEdge Edge;
		Edge.A = A; Edge.B = B;
		Edge.Control = FVector2D(10000.0, 0.0);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}
	// #312: was a hand-built FRouteQuery that skipped AvoidRunways.
	if (!TestTrue(TEXT("dispatched"), Actor->DispatchAgent(TestGraph::Probe(Net, A, B, ETraversalClass::Aircraft), UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
	const int32 Id = Actor->GetTraffic()->GetNewestAgentId();

	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel is created with no asset"), Panel)) { return false; }

	FSelection None;
	Panel->Refresh(Actor, None);
	TestFalse(TEXT("hidden with nothing selected"), Panel->IsShownForTest());

	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();

	FSelection Sel; Sel.Kind = ESelectionKind::Aircraft; Sel.Id = Id;
	Panel->Refresh(Actor, Sel);
	TestTrue(TEXT("shown with an aircraft selected"), Panel->IsShownForTest());
	TestTrue(TEXT("the title names the aircraft"), Panel->TitleForTest().Contains(FString::FromInt(Id)));
	TestFalse(TEXT("Depart is greyed while taxiing"), Panel->IsDepartEnabledForTest());
	// THE BLOCKING BUG, PINNED: a disabled Depart used to paint LIGHTER (TextMuted, a label
	// slot lighter than Button) than an enabled one - backwards. The button's own background
	// never changes (UBuildBarWidget::RefreshState's rule); only the caption dims to TextMuted.
	TestEqual(TEXT("Depart's caption is muted while taxiing, not the button background"),
		Panel->DepartLabelColourForTest(), Style->InkMuted);

	for (int32 I = 0; I < 20000 && Actor->GetTraffic()->LastAgentPhaseForTest() != EAgentPhase::Parked; ++I) { Actor->Tick(1.0f / 30.0f); }
	Panel->Refresh(Actor, Sel);
	TestTrue(TEXT("Depart lights once parked"), Panel->IsDepartEnabledForTest());
	TestEqual(TEXT("Depart's caption returns to full Text once parked"),
		Panel->DepartLabelColourForTest(), Style->Ink);
	return true;
}

/**
 * THE SEAM issue #91 INTRODUCES. The two verbs used to hard-code the caption "Follow (C)" and
 * the ids selection.depart/selection.follow; now EnsureSlots finds them by walking BuildActions()
 * positionally and the caption/key are read off the row. Left unwired, two buttons would still
 * get built - FInspectorWidgetTest would not notice - just with no caption or key on them.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorVerbsFromRegistryTest,
	"AirportMgr.Inspector.VerbsComeFromBuildActions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorVerbsFromRegistryTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel is created with no asset"), Panel)) { return false; }
	if (!TestNotNull(TEXT("Depart button is built"), Panel->DepartButton.Get())) { return false; }
	if (!TestNotNull(TEXT("Follow button is built"), Panel->FollowButton.Get())) { return false; }

	const FBuildAction* DepartAction = nullptr;
	const FBuildAction* FollowAction = nullptr;
	for (const FBuildAction& A : BuildActions())
	{
		if (A.Id == FName(TEXT("selection.depart"))) { DepartAction = &A; }
		if (A.Id == FName(TEXT("selection.follow"))) { FollowAction = &A; }
	}
	if (!TestNotNull(TEXT("the registry has a depart row"), DepartAction)) { return false; }
	if (!TestNotNull(TEXT("the registry has a follow row"), FollowAction)) { return false; }

	const UTextBlock* DepartLabel = Cast<UTextBlock>(Panel->DepartButton->GetContent());
	const UTextBlock* FollowLabel = Cast<UTextBlock>(Panel->FollowButton->GetContent());
	if (!TestNotNull(TEXT("depart button has a label"), DepartLabel)) { return false; }
	if (!TestNotNull(TEXT("follow button has a label"), FollowLabel)) { return false; }
	TestEqual(TEXT("depart's caption comes from the registry, not a literal"),
		DepartLabel->GetText().ToString(), DepartAction->Label.ToString());
	TestEqual(TEXT("follow's caption comes from the registry, not a literal"),
		FollowLabel->GetText().ToString(), FollowAction->Label.ToString());

	// KEY IN THE TOOLTIP, not the caption: Follow has one (C), Depart does not. Checking for
	// the PARENTHESISED form, not a bare "C", because the raw letter would also match inside
	// unrelated words in the tooltip - "(C)" is the actual shape SetToolTipText produces.
	const FString ExpectedKey = FString::Printf(TEXT("(%s%s)"),
		FollowAction->bRequiresCtrl ? TEXT("Ctrl+") : TEXT(""), *FollowAction->Key.GetDisplayName().ToString());
	TestTrue(TEXT("follow's key reaches the tooltip as '(C)'"),
		Panel->FollowButton->GetToolTipText().ToString().Contains(ExpectedKey));
	TestFalse(TEXT("depart's caption no longer carries a parenthesised key"),
		DepartLabel->GetText().ToString().Contains(TEXT("(")));
	return true;
}

/**
 * AN IDLE TICK SETS NO TEXT. Refresh used to call SetText on all three fields every time it
 * ran, whether or not the facts had changed - and UTextBlock::SetText has no early-out of its
 * own (TextBlock.cpp), so a parked aircraft awaiting dispatch re-invalidated three text
 * layouts a tick for a sentence that had not changed since the last one. Issue #187.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorIdleTickSetsNoTextTest,
	"AirportMgr.Inspector.IdleTickSetsNoText",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorIdleTickSetsNoTextTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	URoadNetwork& Net = *Actor->Network;
	const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 0.0), false);
	const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(20000.0, 0.0), false);
	{
		FGuidelineEdge Edge;
		Edge.A = A; Edge.B = B;
		Edge.Control = FVector2D(10000.0, 0.0);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}
	// #312: was a hand-built FRouteQuery that skipped AvoidRunways.
	if (!TestTrue(TEXT("dispatched"), Actor->DispatchAgent(TestGraph::Probe(Net, A, B, ETraversalClass::Aircraft), UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
	const int32 Id = Actor->GetTraffic()->GetNewestAgentId();

	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel is created with no asset"), Panel)) { return false; }

	// AN IDLE TICK: nothing selected. This is the common case on a real HUD - most ticks have
	// no selection at all - and it must set no text on any of the three fields.
	FSelection None;
	Panel->Refresh(Actor, None);
	const int32 AfterFirstHiddenRefresh = Panel->SetTextCallCountForTest();
	Panel->Refresh(Actor, None);
	TestEqual(TEXT("repeating 'nothing selected' sets no text"),
		Panel->SetTextCallCountForTest(), AfterFirstHiddenRefresh);

	// SELECT THE AIRCRAFT: the first Refresh with real facts must set text - this is the
	// transition the gate must never swallow.
	FSelection Sel; Sel.Kind = ESelectionKind::Aircraft; Sel.Id = Id;
	Panel->Refresh(Actor, Sel);
	const int32 AfterFirstShownRefresh = Panel->SetTextCallCountForTest();
	TestTrue(TEXT("selecting something for the first time sets text"),
		AfterFirstShownRefresh > AfterFirstHiddenRefresh);

	// THE IDLE CASE THIS ISSUE IS ABOUT: the same selection, refreshed again with nothing
	// having moved (no Actor->Tick between the two calls) - the facts DescribeAgent returns
	// are identical, so the gate must add nothing.
	Panel->Refresh(Actor, Sel);
	TestEqual(TEXT("an unchanged selection refreshed again sets no more text"),
		Panel->SetTextCallCountForTest(), AfterFirstShownRefresh);

	return true;
}

/**
 * A PARKED AIRCRAFT COMPOSES NOTHING ON A REPEATED REFRESH - issue #309, one level upstream of
 * FInspectorIdleTickSetsNoTextTest above. That test proves SetText adds no more calls; it
 * does NOT prove the four Printfs and two NSLOCTEXT lookups that build the sentence SetText
 * then compares stopped running - they used to run every Refresh regardless, gated only at the
 * very end. This measures the earlier gate (FAircraftDisplay) directly, through
 * ComposeCountForTest, rather than trusting that an unchanged SetText count means an unchanged
 * compose count - which is exactly the assumption #187 shipped and #309 found false here.
 *
 * PARKED, not taxiing: a parked aircraft awaiting dispatch is the common idle case CLAUDE.md's
 * "airside-speedprofile" note and this file's own FInspectorWidgetTest both drive to, and it is
 * the scenario where heading/speed/altitude truly stop moving tick to tick - a taxiing aircraft
 * would legitimately recompose every tick and this test would fail for the wrong reason.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorIdleTickComposesNoTextTest,
	"AirportMgr.Inspector.IdleTickComposesNoText",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorIdleTickComposesNoTextTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	URoadNetwork& Net = *Actor->Network;
	const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 0.0), false);
	const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(20000.0, 0.0), false);
	{
		FGuidelineEdge Edge;
		Edge.A = A; Edge.B = B;
		Edge.Control = FVector2D(10000.0, 0.0);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}
	// #312: was a hand-built FRouteQuery that skipped AvoidRunways.
	if (!TestTrue(TEXT("dispatched"), Actor->DispatchAgent(TestGraph::Probe(Net, A, B, ETraversalClass::Aircraft), UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
	const int32 Id = Actor->GetTraffic()->GetNewestAgentId();

	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel is created with no asset"), Panel)) { return false; }

	// RUN IT TO PARKED, same loop FInspectorWidgetTest uses, so heading/speed/altitude actually
	// stop moving rather than merely being unobserved between two calls with no tick between.
	for (int32 I = 0; I < 20000 && Actor->GetTraffic()->LastAgentPhaseForTest() != EAgentPhase::Parked; ++I) { Actor->Tick(1.0f / 30.0f); }
	if (!TestEqual(TEXT("the agent parked"),
		static_cast<uint8>(Actor->GetTraffic()->LastAgentPhaseForTest()), static_cast<uint8>(EAgentPhase::Parked))) { return false; }

	FSelection Sel; Sel.Kind = ESelectionKind::Aircraft; Sel.Id = Id;

	// First Refresh composes - a real cost, not what this test measures.
	Panel->Refresh(Actor, Sel);
	const int32 Before = Panel->ComposeCountForTest();

	for (int32 Tick = 0; Tick < 10; ++Tick)
	{
		Panel->Refresh(Actor, Sel);
	}

	TestEqual(TEXT("ten idle refreshes of a PARKED aircraft compose the sentence zero more "
		"times - FAircraftDisplay read unchanged, so Refresh never reran the Printf/FString::Format "
		"work SetTextCallCountForTest's own gate only compared the RESULT of"),
		Panel->ComposeCountForTest(), Before);

	return true;
}

/**
 * A SUB-KNOT SPEED CHANGE STILL RECOMPOSES - PR #329 review, on FInspectorIdleTickComposesNoTextTest's
 * own key. The Facts line prints speed TWICE from the same Shown value at two different
 * precisions - m/s to one decimal (%.1f), knots to zero (%.0f) - and 1 kt is 0.514 m/s, finer
 * than a whole knot, so a key on the ROUNDED KNOT alone missed a real, displayed
 * m/s change whenever it landed on the same knot: 5.0 -> 5.1 m/s both round to 10 kt. The key
 * now uses SpeedTenthsRounded (tenths of m/s) for exactly this reason - see its own comment.
 *
 * PRECOMPUTED FACTS, not a simulated aircraft: this needs an EXACT, repeatable speed pair (5.0
 * and 5.1 m/s, same rounded knot), which a real dispatch would take many idle ticks to happen
 * upon by chance if it ever did - Refresh's own PrecomputedAgentFacts parameter exists for
 * exactly this bypass (see its class comment), and no world state beyond a non-null Target is
 * needed since the aircraft branch never touches Target when facts are precomputed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorSubKnotSpeedChangeRecomposesTest,
	"AirportMgr.Inspector.SubKnotSpeedChangeRecomposes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorSubKnotSpeedChangeRecomposesTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor"), Actor)) { return false; }

	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel is created with no asset"), Panel)) { return false; }

	FSelection Sel; Sel.Kind = ESelectionKind::Aircraft; Sel.Id = 1;

	FAgentFacts Facts;
	Facts.Id = 1;
	Facts.TypeName = TEXT("TestType");
	Facts.HeadingDegrees = 90.0;
	Facts.Altitude = 0.0;
	Facts.Destination = TEXT("Stand 1");
	Facts.Status = TEXT("Taxiing");
	Facts.bEngineRunning = true;
	Facts.bCanDepart = false;

	// 500 uu/s = 5.0 m/s = 9.7192 kt, rounds to 10.
	Facts.GroundSpeed = 500.0;
	Panel->Refresh(Actor, Sel, &Facts);
	const int32 Before = Panel->ComposeCountForTest();

	// 510 uu/s = 5.1 m/s (a real, %.1f-visible change) = 9.913584 kt - STILL rounds to 10.
	Facts.GroundSpeed = 510.0;
	Panel->Refresh(Actor, Sel, &Facts);

	TestEqual(TEXT("5.0 -> 5.1 m/s recomposes even though the rounded knot figure (10) did not "
		"change - keying on whole knots alone would have left this at Before"),
		Panel->ComposeCountForTest(), Before + 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorWindowDocksAboveTheBarTest,
	"AirportMgr.Inspector.WindowDocksAboveTheBar",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorWindowDocksAboveTheBarTest::RunTest(const FString& Parameters)
{
	// THE WINDOW NEVER COVERS THE BAR, however tall the bar grows (2026-09-27: the card sat a
	// fixed 72 uu above the screen's bottom while the bar was 118 uu tall). Through the HUD
	// layer's own wiring, not a hand-called DockAbove, so an unwired seam goes red here. Moved
	// from AirportMgr.Inspector.DocksAboveTheBar when the dock moved into UUiWindowHost.
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C) || !TestNotNull(TEXT("with a HUD layer"), C->GetHudForTest()))
	{
		return false;
	}
	// A headless controller has no local player, so BeginPlay never created the HUD's widgets;
	// they are put where CreateAll would have put them, then CreateAll's own last step runs.
	UBuildHudLayer* Hud = C->GetHudForTest();
	Hud->BuildBar = CreateWidget<UBuildBarWidget>(TestWorld.World, UBuildBarWidget::StaticClass());
	Hud->Inspector = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
	Hud->WindowHost = CreateWidget<UUiWindowHost>(TestWorld.World, UUiWindowHost::StaticClass());
	if (!TestNotNull(TEXT("bar"), Hud->BuildBar.Get()) || !TestNotNull(TEXT("inspector"), Hud->Inspector.Get())
		|| !TestNotNull(TEXT("host"), Hud->WindowHost.Get())) { return false; }
	Hud->WireWindows();
	TestTrue(TEXT("the HUD docks the host on ITS bar"), Hud->WindowHost->DockedBarForTest() == Hud->BuildBar);
	TestNotNull(TEXT("and hosts the inspector"), Hud->WindowHost->WindowForTest(TEXT("inspector")));

	UBuildBarWidget& Bar = *Hud->BuildBar;
	UUiWindowHost& Host = *Hud->WindowHost;
	const double Gap = Hud->Inspector->BarGap;
	const float Wide = static_cast<float>(Bar.SectionRowSizeForTest(6000.0f).X);
	if (!TestTrue(TEXT("the bar's row measures to a real width"), Wide > 100.0f)) { return false; }

	const float OneLine = Bar.BarReservedHeightForTest(Wide * 1.1f);
	Host.TickForTest(0.016f);
	TestEqual(TEXT("the window's bottom sits BarGap above the bar's top edge"),
		Host.WindowClearanceForTest(TEXT("inspector")), static_cast<double>(OneLine) + Gap, 0.5);

	// The row wraps (a narrow viewport, or the Selection section arriving): the bar grows, and the
	// window has to rise with it or the new line is hidden under the window.
	const float TwoLines = Bar.BarReservedHeightForTest(Wide * 0.66f);
	if (!TestTrue(TEXT("the wrapped bar is taller - otherwise this proves nothing"), TwoLines > OneLine + 10.0f)) { return false; }
	Host.TickForTest(0.016f);
	TestEqual(TEXT("the window rises with a growing bar"),
		Host.WindowClearanceForTest(TEXT("inspector")), static_cast<double>(TwoLines) + Gap, 0.5);

	// And back down when it shrinks - a window left floating is a gap the player reads as a bug.
	Bar.BarReservedHeightForTest(Wide * 1.1f);
	Host.TickForTest(0.016f);
	TestEqual(TEXT("and comes back down when the bar shrinks"),
		Host.WindowClearanceForTest(TEXT("inspector")), static_cast<double>(OneLine) + Gap, 0.5);

	// ONCE THE PLAYER MOVES IT, IT STAYS: docking would yank a placed window back over the bar.
	Host.MoveWindow(TEXT("inspector"), FVector2D(400.0, 300.0));
	const FBox2D Placed = Host.WindowRect(TEXT("inspector"));
	Bar.BarReservedHeightForTest(Wide * 0.66f);
	Host.TickForTest(0.016f);
	TestEqual(TEXT("a placed window does not follow the bar"), Host.WindowRect(TEXT("inspector")).Min, Placed.Min);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorFollowSaysUnfollowTest,
	"AirportMgr.Inspector.FollowSaysUnfollowWhileFollowing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorFollowSaysUnfollowTest::RunTest(const FString& Parameters)
{
	// THE BUTTON NAMES WHAT PRESSING IT WILL DO: "Unfollow" while the camera rides the
	// aircraft, the action's own label otherwise - so the bar and the panel agree at rest.
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel is created with no asset"), Panel)) { return false; }
	const FBuildAction* Follow = FindAction(FName(TEXT("selection.follow")));
	if (!TestNotNull(TEXT("Follow is in the one action list"), Follow)) { return false; }

	TestEqual(TEXT("at rest it reads as the action does"), Panel->FollowCaptionForTest(), Follow->Label.ToString());
	Panel->ShowFollowing(true);
	TestEqual(TEXT("while following it offers to stop"), Panel->FollowCaptionForTest(), FString(TEXT("Unfollow")));
	Panel->ShowFollowing(false);
	TestEqual(TEXT("and goes back when following ends"), Panel->FollowCaptionForTest(), Follow->Label.ToString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorDemandsTest,
	"AirportMgr.Inspector.ShowsDemands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorDemandsTest::RunTest(const FString& Parameters)
{
	// THE DEMANDS BLOCK (2026-09-28): what the aircraft wants, one line each - the fuel line from
	// AirportOps, the pushback line from its airframe.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel"), Panel)) { return false; }
	TestWorld.Actor->PlaceNode(FVector2D(-100000.0, -100000.0));

	FAgentFacts Facts;
	Facts.Id = 7;
	Facts.TypeName = TEXT("SR22");
	Facts.Phase = EAgentPhase::Parked;
	Facts.Pushback = TEXT("reverses itself");
	Facts.Fuel = TEXT("Fuel 300 L \u00B7 120 L left \u00B7 fuelling");
	Facts.Turnaround = TEXT("Turnaround 3 h \u00B7 1 h 12 min left");
	FSelection Sel; Sel.Kind = ESelectionKind::Aircraft; Sel.Id = 7;
	Panel->Refresh(TestWorld.Actor, Sel, &Facts);
	const FString Text = Panel->FactsForTest();
	TestTrue(TEXT("a Demands heading"), Text.Contains(TEXT("Demands")));
	TestTrue(TEXT("the fuel line"), Text.Contains(TEXT("Fuel 300 L \u00B7 120 L left \u00B7 fuelling")));
	TestTrue(TEXT("the pushback line"), Text.Contains(TEXT("Pushback reverses itself")));
	TestTrue(TEXT("and the turnaround contract"), Text.Contains(TEXT("Turnaround 3 h \u00B7 1 h 12 min left")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorTaxiwayCardTest,
	"AirportMgr.Inspector.TaxiwayCard",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorTaxiwayCardTest::RunTest(const FString& Parameters)
{
	// STRIP STAGE 6: ESelectionKind::Taxiway has its own card - not the stand branch the widget's
	// old if-chain fell through to for any kind it did not name (a stand card for segment index N
	// would describe ENTITY N). A restricted taxiway says so and names the obstruction.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel"), Panel)) { return false; }

	int32 F = INDEX_NONE;
	for (int32 W = 0; W < Actor->GetWidthCount(ERoadKind::Taxiway); ++W)
	{
		const URoadProfile* Each = Actor->ResolveWidthProfile(ERoadKind::Taxiway, W);
		if (Each != nullptr && IcaoCode::TaxiwayLetterForWidth(Each->GetTotalWidth()) == EIcaoCode::F) { F = W; }
	}
	if (!TestTrue(TEXT("an F width"), F != INDEX_NONE)) { return false; }
	const double WidthF = Actor->ResolveWidthProfile(ERoadKind::Taxiway, F)->GetTotalWidth();
	const URoadProfile* Road = Actor->ResolveProfileFor(ERoadKind::ServiceRoad, INDEX_NONE);
	if (!TestNotNull(TEXT("a road profile"), Road)) { return false; }
	const double RoadY = 0.5 * (WidthF + IcaoCode::TaxiwayStripFor(EIcaoCode::E, WidthF) + IcaoCode::TaxiwayStripFor(EIcaoCode::F, WidthF))
		+ Road->GetMaxHalfWidth();

	// Laid at the NARROWEST, then upgraded - the road is refused inside an F strip at lay time.
	Actor->ConnectNodes(Actor->PlaceNode({ -20000.0, 0.0 }), Actor->PlaceNode({ 20000.0, 0.0 }), ERoadKind::Taxiway, 0, EPavement::Tarmac);
	const int32 Seg = Actor->Network->GetSegments().Num() - 1;
	Actor->ConnectNodes(Actor->PlaceNode({ -3000.0, RoadY }), Actor->PlaceNode({ 3000.0, RoadY }), ERoadKind::ServiceRoad, INDEX_NONE, EPavement::Tarmac);
	if (!TestTrue(TEXT("upgraded to F"), Actor->UpgradeSegment(Seg, ERoadKind::Taxiway, F, EPavement::Tarmac))) { return false; }

	FSelection Sel; Sel.Kind = ESelectionKind::Taxiway; Sel.Id = Seg;
	Panel->Refresh(Actor, Sel, nullptr);
	TestTrue(TEXT("the panel shows"), Panel->IsShownForTest());
	TestTrue(FString::Printf(TEXT("titled as a taxiway: '%s'"), *Panel->TitleForTest()), Panel->TitleForTest().StartsWith(TEXT("Taxiway")));
	const FString Facts = Panel->FactsForTest();
	TestTrue(FString::Printf(TEXT("its letter: '%s'"), *Facts), Facts.Contains(TEXT("Code F")));
	TestTrue(FString::Printf(TEXT("and the restriction, naming the road: '%s'"), *Facts),
		Facts.Contains(TEXT("Restricted to Code E")) && Facts.Contains(TEXT("a service road")));
	return true;
}

namespace
{
	/** LogInspector at Warning - FLogLineSpy matches Log only. Unbuffered, issue #216's reason. */
	struct FInspectorWarningSpy : public FOutputDevice
	{
		int32 Count = 0;
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& InCategory) override
		{
			if (InCategory == FName(TEXT("LogInspector")) && Verbosity == ELogVerbosity::Warning) { ++Count; }
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorUnknownKindWarnsOnceTest,
	"AirportMgr.Inspector.UnknownKindWarnsOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorUnknownKindWarnsOnceTest::RunTest(const FString& Parameters)
{
	// A KIND WITH NO CARD hides the panel and says so ONCE per selection, not once per tick -
	// Refresh runs every NativeTick, and a warning a frame is a log nobody can read.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel"), Panel)) { return false; }
	FSelection Sel;
	Sel.Kind = static_cast<ESelectionKind>(200);   // no such kind - what an appended one looked like before its card
	Sel.Id = 1;

	FInspectorWarningSpy Spy;
	GLog->AddOutputDevice(&Spy);
	for (int32 Tick = 0; Tick < 5; ++Tick)
	{
		Panel->Refresh(TestWorld.Actor, Sel, nullptr);
	}
	Sel.Id = 2;
	Panel->Refresh(TestWorld.Actor, Sel, nullptr);
	Panel->Refresh(TestWorld.Actor, Sel, nullptr);
	GLog->RemoveOutputDevice(&Spy);

	TestFalse(TEXT("the panel hides"), Panel->IsShownForTest());
	TestEqual(TEXT("one warning per selection, not per tick"), Spy.Count, 2);
	return true;
}

/**
 * A stand's card is titled by its NUMBER - the one painted at its turn-off - never by its entity
 * index (taxiway strip stage 5). Built so the two differ: a stand placed and deleted first, so
 * the stand under test recycles slot 0 but is issued number 2. The old title read "Stand 0".
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorStandTitleIsTheNumberTest,
	"AirportMgr.Inspector.StandTitleIsTheNumber",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorStandTitleIsTheNumberTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor"), Actor)) { return false; }
	Actor->ClearNetwork();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();

	IRoadEditTarget* Target = Actor;
	const int32 Retired = Target->PlaceStand(FVector2D(-60000.0, 0.0), 0.0);
	if (!TestTrue(TEXT("a stand placed first"), Retired != INDEX_NONE)) { return false; }
	TestTrue(TEXT("and removed"), Actor->Network->RemoveEntity(Actor->Network->EntityIdAt(Retired)));
	const int32 Index = Target->PlaceStand(FVector2D(0.0, 0.0), 0.0);
	if (!TestTrue(TEXT("the stand under test placed"), Index != INDEX_NONE)) { return false; }
	const int32 Number = Actor->Network->GetEntities()[Index].StandNumber;
	if (!TestTrue(TEXT("its number differs from its index, or this test measures nothing"),
			Number == 2 && Index != Number)) { return false; }

	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel is created with no asset"), Panel)) { return false; }
	FSelection Sel; Sel.Kind = ESelectionKind::Stand; Sel.Id = Index;
	Panel->Refresh(Actor, Sel);
	TestTrue(TEXT("shown with a stand selected"), Panel->IsShownForTest());
	TestEqual(TEXT("the title is the stand's number"), Panel->TitleForTest(), FString(TEXT("Stand 2")));
	return true;
}

/**
 * THE UNSTICK ROW IS CONSUMED, not only declared (CLAUDE.md: "check where a list is CONSUMED") -
 * selection.unstick in BuildActions() is read by EnsureSlots BY ID and becomes the card's popup
 * button, captioned by the row. A row nothing reads is how ARoadBuildController::Tools shipped a
 * tool with no key.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorUnstickRowTest,
	"AirportMgr.Inspector.UnstickRowIsConsumed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorUnstickRowTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	const FBuildAction* Row = nullptr;
	for (const FBuildAction& A : BuildActions())
	{
		if (A.Id == FName(TEXT("selection.unstick"))) { Row = &A; }
	}
	if (!TestNotNull(TEXT("the registry has an unstick row"), Row)) { return false; }
	TestFalse(TEXT("with no key - a despawn is a misclick away"), Row->Key.IsValid());

	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel"), Panel)) { return false; }
	if (!TestNotNull(TEXT("the Unstick popup button is built from the row"), Panel->UnstickMenu.Get())) { return false; }
	const UUiButton* Button = Panel->UnstickMenu->GetButton();
	if (!TestNotNull(TEXT("it has a button"), Button)) { return false; }
	TestEqual(TEXT("captioned by the row, not a literal"), Button->GetLabel()->GetText().ToString(), Row->Label.ToString());
	return true;
}

/**
 * LIT WHEN STUCK. A stranded agent lights the button at once; one taxiing freely does not. The
 * light is the only thing that tells the player which of forty aircraft the button is FOR.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorUnstickHighlightTest,
	"AirportMgr.Inspector.UnstickLightsWhenStranded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorUnstickHighlightTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	URoadNetwork& Net = *Actor->Network;
	const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 0.0), false);
	const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(20000.0, 0.0), false);
	{
		FGuidelineEdge Edge;
		Edge.A = A; Edge.B = B;
		Edge.Control = FVector2D(10000.0, 0.0);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}
	if (!TestTrue(TEXT("dispatched"), Actor->DispatchAgent(TestGraph::Probe(Net, A, B, ETraversalClass::Aircraft), UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
	const int32 Id = Actor->GetTraffic()->GetNewestAgentId();
	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
	FSelection Sel; Sel.Kind = ESelectionKind::Aircraft; Sel.Id = Id;

	for (int32 I = 0; I < 30; ++I) { Actor->Tick(1.0f / 30.0f); }
	Panel->Refresh(Actor, Sel);
	TestFalse(TEXT("taxiing freely: not lit"), Panel->IsUnstickHighlightedForTest());

	UGroundTraffic* Model = Actor->GetTraffic()->GetModel();
	if (!TestTrue(TEXT("stranded"), FGroundTrafficTestAccess(*Model).Strand(Id))) { return false; }
	for (int32 I = 0; I < 5; ++I) { Actor->Tick(1.0f / 30.0f); }
	if (!TestEqual(TEXT("its phase is Stranded"), Model->FindAgent(Id)->Phase, EAgentPhase::Stranded)) { return false; }
	Panel->Refresh(Actor, Sel);
	TestTrue(TEXT("stranded: lit"), Panel->IsUnstickHighlightedForTest());
	return true;
}

/**
 * THE CONFIRM IS A SECOND CLICK, AND A CLOSE DISARMS IT. A bConfirm line's first click only arms it
 * (and re-captions it); the second chooses it; closing in between means the next open starts from
 * scratch - an armed Despawn surviving a close would be a one-click delete the next time.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUiMenuButtonConfirmTest,
	"AirportMgr.UI.MenuButton.ConfirmTakesTwoClicks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiMenuButtonConfirmTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();
	if (!TestNotNull(TEXT("a style"), Style)) { return false; }
	UUiMenuButton* Menu = CreateWidget<UUiMenuButton>(TestWorld.World, UUiMenuButton::StaticClass());
	Menu->Build(*Style, FText::FromString(TEXT("Verb")));
	Menu->Items = []()
	{
		TArray<FUiMenuItem> Out;
		FUiMenuItem& Plain = Out.AddDefaulted_GetRef();
		Plain.Label = FText::FromString(TEXT("Plain"));
		FUiMenuItem& Off = Out.AddDefaulted_GetRef();
		Off.Label = FText::FromString(TEXT("Off"));
		Off.bEnabled = false;
		Off.Why = FText::FromString(TEXT("because"));
		FUiMenuItem& Danger = Out.AddDefaulted_GetRef();
		Danger.Label = FText::FromString(TEXT("Danger"));
		Danger.bConfirm = true;
		Danger.ConfirmLabel = FText::FromString(TEXT("Sure?"));
		return Out;
	};

	// HEADLESS OPEN: the anchor's content callback, which is what the anchor itself calls on open.
	TestNotNull(TEXT("the popup builds"), Menu->BuildMenu());
	TestEqual(TEXT("three lines"), Menu->ShownItemsForTest().Num(), 3);

	Menu->Choose(1);
	TestEqual(TEXT("a disabled line chooses nothing"), Menu->ChosenCountForTest(), 0);

	Menu->Choose(2);
	TestEqual(TEXT("a confirm line's first click chooses nothing"), Menu->ChosenCountForTest(), 0);
	TestEqual(TEXT("it arms it"), Menu->ArmedForTest(), 2);

	Menu->HandleOpenChanged(false);
	TestEqual(TEXT("a close disarms it"), Menu->ArmedForTest(), INDEX_NONE);

	Menu->BuildMenu();
	Menu->Choose(2);
	TestEqual(TEXT("re-opened: the first click arms again, chooses nothing"), Menu->ChosenCountForTest(), 0);
	Menu->Choose(2);
	TestEqual(TEXT("the second click chooses it"), Menu->ChosenCountForTest(), 1);
	TestEqual(TEXT("that line"), Menu->LastChosenForTest(), 2);

	Menu->BuildMenu();
	Menu->Choose(0);
	TestEqual(TEXT("a plain line chooses on one click"), Menu->ChosenCountForTest(), 2);
	TestEqual(TEXT("that line"), Menu->LastChosenForTest(), 0);
	return true;
}

/**
 * THE HOLD AND DEADLOCK LINES, AT THE COMPOSITION (inspector hold info, 2026-09-30). Two aircraft
 * flying flights wait on each other past StallSeconds: each card's title leads with its
 * registration, names the OTHER in a deadlock line worded like the Deadlock alert, and says what it
 * holds at and for whom by registration; the Show button selects the one waited for. A third, with
 * no flight and no wait, keeps the id title and shows neither line nor button.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorHoldAndDeadlockTest,
	"AirportMgr.Inspector.HoldAndDeadlockLines",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorHoldAndDeadlockTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	URoadNetwork& Net = *Actor->Network;
	const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 0.0), false);
	const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(20000.0, 0.0), false);
	FGuidelineEdgeId Lane;
	{
		FGuidelineEdge Edge;
		Edge.A = A; Edge.B = B;
		Edge.Control = FVector2D(10000.0, 0.0);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Lane = Net.AddGuidelineEdge(MoveTemp(Edge));
	}
	int32 Ids[3] = {};
	for (int32& Id : Ids)
	{
		if (!TestTrue(TEXT("dispatched"), Actor->DispatchAgent(TestGraph::Probe(Net, A, B, ETraversalClass::Aircraft),
			UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
		Id = Actor->GetTraffic()->GetNewestAgentId();
	}
	const int32 One = Ids[0], Two = Ids[1], Loner = Ids[2];

	// FLIGHTS FOR TWO OF THEM - the board is where a registration lives; Airside has none.
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	UFlightBoard* Board = NewObject<UFlightBoard>(GetTransientPackage());
	auto Fly = [&](int32 AgentId, const TCHAR* Callsign)
	{
		UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
		Flight->AgentId = AgentId;
		Flight->Callsign = Callsign;
		Flight->AirlineName = FText::FromString(TEXT("Flying Club"));
		Flight->Phase = EFlightPhase::TaxiIn;
		Board->AddOffer(*Clock, Flight);
	};
	Fly(One, TEXT("G-SVBT"));
	Fly(Two, TEXT("G-HDVK"));

	UGroundTraffic& Traffic = *Actor->GetGroundTraffic();
	// 80 MOVEMENT SECONDS at a 1200 s day (72 game s per real s): 5760 game s, 96 game minutes.
	const double Stalled = 80.0;
	if (!TestTrue(TEXT("the stall is past StallSeconds, or no ring is reported"), Stalled > Traffic.Rules.StallSeconds)) { return false; }
	Clock->SetUniformDay(1200.0);
	FGroundTrafficTestAccess Access(Traffic);
	Access.ScriptWait(One, FTrafficResource::OfEdge(Lane), Two, Stalled);
	Access.ScriptWait(Two, FTrafficResource::OfNode(B), One, Stalled);

	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel is created with no asset"), Panel)) { return false; }
	Panel->UseFlightBoardForTest(Board);
	Panel->UseClockForTest(Clock);

	FAgentFacts Facts;
	InspectFacts::DescribeAgent(Traffic, &Net, One, Facts);
	FSelection Sel; Sel.Kind = ESelectionKind::Aircraft; Sel.Id = One;
	Panel->Refresh(Actor, Sel);
	TestEqual(TEXT("the title leads with the registration, then type and airline"), Panel->TitleForTest(),
		FString::Printf(TEXT("G-SVBT · %s · Flying Club"), *Facts.TypeName));
	TestTrue(TEXT("the deadlock line names the partner by registration"), Panel->DeadlockForTest().Contains(TEXT("Deadlocked with G-HDVK")));
	TestTrue(TEXT("and asks for the fix in the alert's own words"),
		Panel->DeadlockForTest().Contains(UOpsAlerts::DeadlockRemedy().ToString()));
	TestEqual(TEXT("the hold line names the blocker by registration, and the wait in GAME time - the turnaround line's unit and words, not 80 movement seconds"),
		Panel->StatusForTest(), FString(TEXT("Waiting behind G-HDVK · 1 h 36 min")));
	TestEqual(TEXT("the button offers to show it"), Panel->WaitingForCaptionForTest(), FString(TEXT("Show G-HDVK")));

	// THE OTHER CARD SAYS THE SAME RING from its end.
	Sel.Id = Two;
	Panel->Refresh(Actor, Sel);
	TestTrue(TEXT("its deadlock line names the first"), Panel->DeadlockForTest().Contains(TEXT("Deadlocked with G-SVBT")));
	TestTrue(TEXT("held at a node: a crossing"), Panel->StatusForTest().StartsWith(TEXT("Waiting at crossing for G-SVBT")));

	// THE ALERT AGREES: the same traffic, recomputed, raises one Deadlock in the same words for the fix.
	UOpsAlerts* Alerts = NewObject<UOpsAlerts>(GetTransientPackage());
	FOpsAlertSources Sources;
	Sources.Traffic = &Traffic;
	Alerts->Recompute(Sources, 0.0);
	const FOpsAlert* Raised = Alerts->GetAlerts().FindByPredicate([](const FOpsAlert& Each) { return Each.Key.Kind == EAlertKind::Deadlock; });
	if (TestNotNull(TEXT("the alert sees the ring the cards show"), Raised))
	{
		TestTrue(TEXT("and words the fix as the card does"), Raised->Text.ToString().Contains(UOpsAlerts::DeadlockRemedy().ToString()));
	}

	// THE BUTTON SELECTS THE BLOCKER, through the controller the alert Go uses.
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(Actor);
	TestTrue(TEXT("Show goes somewhere"), Panel->ShowWaitedFor(*C));
	TestEqual(TEXT("an agent is selected"), C->GetSelection().Kind, ESelectionKind::Aircraft);
	TestEqual(TEXT("the one this card waits for"), C->GetSelection().Id, One);

	// BELOW StallSeconds IT IS TRAFFIC, NOT A JAM: the card drops its deadlock line exactly when the
	// alert would not raise one - both read CurrentDeadlocks.
	Access.ScriptWait(One, FTrafficResource::OfEdge(Lane), Two, Traffic.Rules.StallSeconds * 0.5);
	Access.ScriptWait(Two, FTrafficResource::OfNode(B), One, Traffic.Rules.StallSeconds * 0.5);
	Sel.Id = One;
	Panel->Refresh(Actor, Sel);
	TestEqual(TEXT("a short wait shows no deadlock line"), Panel->DeadlockForTest(), FString());
	TestEqual(TEXT("but still the hold and its Show"), Panel->WaitingForCaptionForTest(), FString(TEXT("Show G-HDVK")));
	UOpsAlerts* Quiet = NewObject<UOpsAlerts>(GetTransientPackage());
	Quiet->Recompute(Sources, 0.0);
	TestFalse(TEXT("and the alert raises none either"), Quiet->GetAlerts().ContainsByPredicate(
		[](const FOpsAlert& Each) { return Each.Key.Kind == EAlertKind::Deadlock; }));

	// THE BLOCKER GONE: Show refuses and moves no selection.
	Traffic.RetireAgent(Two);
	Panel->Refresh(Actor, Sel);
	TestEqual(TEXT("still waiting on it, so this is the gone path and not the no-wait one"),
		Panel->WaitingForCaptionForTest(), FString(TEXT("Show G-HDVK")));
	TestFalse(TEXT("Show on a blocker that has gone goes nowhere"), Panel->ShowWaitedFor(*C));
	TestEqual(TEXT("and the selection stays where it was"), C->GetSelection().Id, One);

	// NO FLIGHT, NO WAIT: the id title, no deadlock line, no button.
	Sel.Id = Loner;
	Panel->Refresh(Actor, Sel);
	TestTrue(TEXT("an agent no flight owns keeps the id title"), Panel->TitleForTest().Contains(FString::Printf(TEXT("#%d"), Loner)));
	TestEqual(TEXT("no deadlock line for a mover"), Panel->DeadlockForTest(), FString());
	TestEqual(TEXT("no Show button for a mover"), Panel->WaitingForCaptionForTest(), FString());
	TestFalse(TEXT("and Show does nothing"), Panel->ShowWaitedFor(*C));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorFacilityCardTest,
	"AirportMgr.Inspector.FacilityCardRendersTheQuote",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorFacilityCardTest::RunTest(const FString& Parameters)
{
	// THE CARD RENDERS ONLY THE QUOTE (spec §4): every caption, enabled state and reason below comes from
	// the struct handed in - nothing is computed here, so a button cannot disagree with the rules.
	FAirsideTestWorld Bare(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), Bare.World)) { return false; }
	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(Bare.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel is created with no asset"), Panel)) { return false; }
	TestNotNull(TEXT("the code-built card has a buy-module button"), Panel->FacilityRows->BuyModuleButton.Get());
	TestNotNull(TEXT("and a buy-vehicle menu"), Panel->FacilityRows->BuyVehicleMenu.Get());

	FFacilityQuote Quote;
	Quote.Refusal = EPurchaseRefusal::None;
	Quote.Bays = 1;
	Quote.Vehicles = 1;
	FModuleOfferQuote& Shed = Quote.Modules.AddDefaulted_GetRef();
	Shed.Module = EDepotModule::Shed;
	Shed.Name = FText::FromString(TEXT("Shed"));
	Shed.PluralName = FText::FromString(TEXT("Sheds"));
	Shed.Owned = 1;
	Shed.Reserved = 3;
	Shed.Refusal = EPurchaseRefusal::CannotAfford;
	Shed.Label = FText::FromString(TEXT("Buy Shed 40,000"));
	for (const TCHAR* Code : { TEXT("FUEL"), TEXT("UTILITY") })
	{
		FVehicleOfferQuote& Offer = Quote.VehicleOffers.AddDefaulted_GetRef();
		Offer.TypeCode = Code;
		Offer.Refusal = EPurchaseRefusal::NoFreeBay;
		Offer.Label = FText::FromString(Code);
	}
	FFleetRowQuote& Row = Quote.Fleet.AddDefaulted_GetRef();
	Row.VehicleId = 7;
	Row.Line = TEXT("FUEL #7 · to stand 2 · 10,000 L");
	Row.Refusal = EPurchaseRefusal::VehicleBusy;
	Row.SellLabel = FText::FromString(TEXT("Sell 45,000"));

	Panel->FacilityRows->Show(Quote);
	TestTrue(TEXT("a facility's rows are shown"), Panel->FacilityRows->AreFacilityRowsShownForTest());
	TestEqual(TEXT("the shed line counts owned against reserved"), Panel->FacilityRows->ShedsTextForTest(), FString(TEXT("Sheds 1 / 3 space")));
	TestFalse(TEXT("an unaffordable shed is a disabled button"), Panel->FacilityRows->IsBuyModuleEnabledForTest());
	TestTrue(TEXT("whose caption says why"), Panel->FacilityRows->BuyModuleCaptionForTest().Contains(UFacilityPurchases::RefusalText(EPurchaseRefusal::CannotAfford).ToString()));
	TestEqual(TEXT("the vehicle line counts vehicles against bays"), Panel->FacilityRows->VehiclesTextForTest(), FString(TEXT("Vehicles 1 / 1 bays")));
	const TArray<FUiMenuItem> Items = Panel->FacilityRows->BuyVehicleItemsForTest();
	if (TestEqual(TEXT("one menu line per vehicle offer"), Items.Num(), 2))
	{
		TestFalse(TEXT("a full depot greys every line"), Items[0].bEnabled);
		TestTrue(TEXT("with the refusal as its reason"), Items[0].Why.EqualTo(UFacilityPurchases::RefusalText(EPurchaseRefusal::NoFreeBay)));
	}
	TestEqual(TEXT("one fleet row per vehicle"), Panel->FacilityRows->FleetRowCountForTest(), 1);
	TestFalse(TEXT("a busy vehicle's Sell is disabled"), Panel->FacilityRows->IsSellEnabledForTest(0));

	Panel->FacilityRows->Show(FFacilityQuote());
	TestFalse(TEXT("a card that is no facility shows no purchase rows"), Panel->FacilityRows->AreFacilityRowsShownForTest());

	FFacilityQuote Empty = Quote;
	Empty.Vehicles = 0;
	TestEqual(TEXT("an empty depot on a road says what to do"),
		FDepotCard::StatusWith(Empty, /*bReachable=*/true, TEXT("No jobs")), FString(TEXT("No vehicles \u2014 buy one")));
	TestEqual(TEXT("off the road, the road is the fix it names"),
		FDepotCard::StatusWith(Empty, /*bReachable=*/false, TEXT("Cannot dispatch")), FString(TEXT("Cannot dispatch")));
	TestEqual(TEXT("with a vehicle, the backlog stands"),
		FDepotCard::StatusWith(Quote, /*bReachable=*/true, TEXT("No jobs")), FString(TEXT("No jobs")));
	return true;
}

namespace
{
	/**
	 * THE DEPOT CARD, WIRED END TO END: an attached runtime, the player's plotted depot (one shed, no
	 * trucks - R3) on FacilityWiredTest's 50 x 24 m plot that reserves a second shed, a controller pointed
	 * at both with the depot selected, and a panel. Prefixed for the unity build.
	 */
	struct FInspectorDepotRig
	{
		FAirsideTestWorld World;
		UOpsRuntime* Runtime = nullptr;
		ARoadBuildController* Controller = nullptr;
		UInspectorWidget* Panel = nullptr;
		FEntityInstanceId Depot;
		FSelection DepotSelection;

		bool Build()
		{
			ARoadNetworkActor* Actor = World.Actor;
			if (Actor == nullptr) { return false; }
			Actor->PlaceNode(FVector2D(0.0, 40000.0));
			Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
			Runtime = NewObject<UOpsRuntime>();
			Runtime->Attach(Actor);
			const TArray<FVector2D> Plot = { FVector2D(0.0, 0.0), FVector2D(5000.0, 0.0), FVector2D(5000.0, 2400.0), FVector2D(0.0, 2400.0) };
			const int32 Index = Actor->PlaceEntityInPlot(Plot, Plot[0], Plot[1],
				{ EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump }, EPlaceableEntity::FuelDepot);
			if (Index == INDEX_NONE) { return false; }
			Depot = Actor->Network->EntityIdAt(Index);
			Controller = World.World->SpawnActor<ARoadBuildController>();
			if (Controller == nullptr) { return false; }
			// REGISTERED as play registers it: a headless world defers PostInitializeComponents, so the spawn
			// alone leaves GetFirstPlayerController null and the panel (UAirportMgrPanelWidget::Controller)
			// would find no controller - measured 2026-09-30, the card drew no purchase rows.
			World.World->AddController(Controller);
			Controller->SetTargetForTest(Actor);
			Controller->SetOpsRuntimeForTest(Runtime);
			DepotSelection.Kind = ESelectionKind::Stand;
			DepotSelection.Id = Index;
			Controller->SelectForTest(DepotSelection);
			Panel = CreateWidget<UInspectorWidget>(World.World, UInspectorWidget::StaticClass());
			return Panel != nullptr;
		}

		void Refresh() { Panel->Refresh(World.Actor, Controller->GetSelection()); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorDepotCardBuysTest,
	"AirportMgr.Inspector.DepotCardBuysThroughTheController",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorDepotCardBuysTest::RunTest(const FString& Parameters)
{
	// THE CARD'S BUY CLICKS REACH THE RULES: menu line -> ChooseVehicleToBuy -> selection.buy_vehicle's
	// TryRun -> UOpsRuntime::BuyVehicle, and Buy Shed -> selection.buy_module -> BuyModule. Each hop is a
	// seam a rename or an unbound delegate would cut with every widget test still green.
	FInspectorDepotRig Rig;
	if (!TestTrue(TEXT("setup: depot, runtime, controller and panel"), Rig.Build())) { return false; }
	TestTrue(TEXT("the controller names the selected depot"), Rig.Controller->SelectedFacility() == Rig.Depot);
	const UJobBoard* Board = Rig.Runtime->GetJobBoard();
	const ULedger* Ledger = Rig.Runtime->GetLedger();
	if (!TestNotNull(TEXT("a job board"), Board) || !TestNotNull(TEXT("a ledger"), Ledger)) { return false; }

	Rig.Refresh();
	TestTrue(TEXT("the depot's card is shown"), Rig.Panel->IsShownForTest());
	TestTrue(TEXT("with its purchase rows - the panel reached the controller's runtime"), Rig.Panel->FacilityRows->AreFacilityRowsShownForTest());
	const FFacilityQuote Before = Rig.Runtime->QuoteFacility(Rig.Depot);
	if (!TestTrue(TEXT("setup: an empty depot with a free bay and an affordable vehicle"),
		Before.Vehicles == 0 && Before.VehicleOffers.Num() > 0 && Before.VehicleOffers[0].Refusal == EPurchaseRefusal::None)) { return false; }
	if (!TestTrue(TEXT("setup: a second shed is reserved and affordable"),
		Before.Modules.Num() == 1 && Before.Modules[0].Refusal == EPurchaseRefusal::None)) { return false; }
	TestEqual(TEXT("the card's menu offers what the quote offers"), Rig.Panel->FacilityRows->BuyVehicleItemsForTest().Num(), Before.VehicleOffers.Num());

	const double Balance = Ledger->Balance();
	Rig.Panel->FacilityRows->ChooseBuyVehicleForTest(0);
	TestEqual(TEXT("choosing the first line bought one vehicle at the depot"), Board->VehiclesAt(Rig.Depot), 1);
	TestEqual(TEXT("and charged its price"), Ledger->Balance(), Balance - Before.VehicleOffers[0].Price, 0.01);

	Rig.Refresh();
	Rig.Panel->FacilityRows->ClickBuyModuleForTest();
	const FFacilityQuote After = Rig.Runtime->QuoteFacility(Rig.Depot);
	TestEqual(TEXT("Buy Shed bought the second shed through the controller"), After.Modules.Num() > 0 ? After.Modules[0].Owned : 0, 2);
	TestEqual(TEXT("and the depot has a second bay"), After.Bays, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorSellTakesTwoClicksTest,
	"AirportMgr.Inspector.SellTakesTwoClicks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorSellTakesTwoClicksTest::RunTest(const FString& Parameters)
{
	// A SALE CANNOT BE UNDONE, so the first click only ARMS it and the caption asks again (memory:
	// destructive gestures need a deliberate mode). Clicking away disarms: an armed sale surviving a
	// change of card would be a one-click sale the next time the depot is picked.
	FInspectorDepotRig Rig;
	if (!TestTrue(TEXT("setup: depot, runtime, controller and panel"), Rig.Build())) { return false; }
	const UJobBoard* Board = Rig.Runtime->GetJobBoard();
	const ULedger* Ledger = Rig.Runtime->GetLedger();
	if (!TestNotNull(TEXT("a job board"), Board) || !TestNotNull(TEXT("a ledger"), Ledger)) { return false; }
	const FFacilityQuote Offers = Rig.Runtime->QuoteFacility(Rig.Depot);
	if (!TestTrue(TEXT("setup: a vehicle on offer"), Offers.VehicleOffers.Num() > 0)) { return false; }
	const FPurchaseResult Bought = Rig.Runtime->BuyVehicle(Rig.Depot, Offers.VehicleOffers[0].TypeCode);
	if (!TestTrue(TEXT("setup: one vehicle bought"), Bought.Succeeded())) { return false; }
	const int32 Vehicle = Bought.VehicleId;

	Rig.Refresh();
	const FFacilityQuote Quote = Rig.Runtime->QuoteFacility(Rig.Depot);
	if (!TestTrue(TEXT("setup: the idle vehicle can be sold"), Quote.Fleet.Num() == 1 && Quote.Fleet[0].Refusal == EPurchaseRefusal::None)) { return false; }
	if (!TestEqual(TEXT("setup: one fleet row"), Rig.Panel->FacilityRows->FleetRowCountForTest(), 1)) { return false; }
	const double Refund = Quote.Fleet[0].Refund;
	const double Balance = Ledger->Balance();

	// FIRST CLICK: armed, nothing sold.
	Rig.Panel->FacilityRows->ClickSellForTest(0);
	TestEqual(TEXT("the first click arms this vehicle on the controller"), Rig.Controller->GetArmedSellVehicle(), Vehicle);
	TestNotNull(TEXT("the vehicle is still there"), Board->FindVehicle(Vehicle));
	TestEqual(TEXT("and no money moved"), Ledger->Balance(), Balance, 0.01);
	Rig.Refresh();
	TestTrue(TEXT("the caption asks again"), Rig.Panel->FacilityRows->SellCaptionForTest(0).Contains(TEXT("click again")));

	// CLICK AWAY AND BACK: disarmed.
	Rig.Controller->SelectForTest(FSelection());
	Rig.Refresh();
	TestEqual(TEXT("clearing the selection disarms the sale"), Rig.Controller->GetArmedSellVehicle(), 0);
	Rig.Controller->SelectForTest(Rig.DepotSelection);
	Rig.Refresh();
	TestFalse(TEXT("back on the depot, the caption no longer asks again"), Rig.Panel->FacilityRows->SellCaptionForTest(0).Contains(TEXT("click again")));
	Rig.Panel->FacilityRows->ClickSellForTest(0);
	TestNotNull(TEXT("so the next click only arms again - still not sold"), Board->FindVehicle(Vehicle));
	TestEqual(TEXT("armed once more"), Rig.Controller->GetArmedSellVehicle(), Vehicle);

	// SECOND CLICK: sold.
	Rig.Refresh();
	Rig.Panel->FacilityRows->ClickSellForTest(0);
	TestNull(TEXT("the second click sells it"), Board->FindVehicle(Vehicle));
	TestEqual(TEXT("crediting the resale"), Ledger->Balance(), Balance + Refund, 0.01);
	TestEqual(TEXT("and the controller is disarmed"), Rig.Controller->GetArmedSellVehicle(), 0);
	return true;
}

/**
 * THE CONTROLLER'S ARMED SALE IS NOT THE ROWS' TO CLEAR ALONE. A sale armed on a depot and then clicked away from - or replaced by
 * another selection - must not survive to the next time the depot is picked, and that holds for an inspector whose asset placed no
 * UInspectorFacilityRows too: there is no row to ask, and the controller's id is still armed. (Until review of #441 the widget asked
 * the rows and did nothing when it had none.)
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorDeselectDisarmsWithoutRowsTest,
	"AirportMgr.Inspector.DeselectDisarmsTheControllerWithoutRows",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorDeselectDisarmsWithoutRowsTest::RunTest(const FString& Parameters)
{
	FInspectorDepotRig Rig;
	if (!TestTrue(TEXT("setup: depot, runtime, controller and panel"), Rig.Build())) { return false; }
	Rig.Refresh();
	if (!TestNotNull(TEXT("setup: the panel built its purchase rows"), Rig.Panel->FacilityRows.Get())) { return false; }
	Rig.Panel->FacilityRows = nullptr;   // an inspector whose asset placed none
	Rig.Controller->ArmSellVehicle(42);
	if (!TestEqual(TEXT("setup: a sale armed on the controller"), Rig.Controller->GetArmedSellVehicle(), 42)) { return false; }

	Rig.Controller->SelectForTest(FSelection());
	Rig.Refresh();
	TestEqual(TEXT("clearing the selection disarms it, with no rows to ask"), Rig.Controller->GetArmedSellVehicle(), 0);

	Rig.Controller->SelectForTest(Rig.DepotSelection);
	Rig.Refresh();
	Rig.Controller->ArmSellVehicle(43);
	FSelection Other = Rig.DepotSelection;
	Other.Id += 1;
	Rig.Panel->Refresh(Rig.World.Actor, Other);
	TestEqual(TEXT("and so does a different selection"), Rig.Controller->GetArmedSellVehicle(), 0);
	return true;
}

/**
 * WHAT IT WAITS FOR AND THE RING move with no board revision (the #423 lines on PR E's keyed card): the stall and the
 * partner's wait are traffic state, re-read from InspectFacts::DescribeAgent every tick and gated only by FAircraftDisplay's
 * Waited and Partners fields. Each step below moves ONE of them, the flight board untouched, and the card must follow.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorWaitingOnRefreshesTest,
	"AirportMgr.Inspector.Cache.WaitingOnRefreshesTheCard",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorWaitingOnRefreshesTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	URoadNetwork& Net = *Actor->Network;
	const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 0.0), false);
	const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(20000.0, 0.0), false);
	FGuidelineEdgeId Lane;
	{
		FGuidelineEdge Edge;
		Edge.A = A; Edge.B = B;
		Edge.Control = FVector2D(10000.0, 0.0);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Lane = Net.AddGuidelineEdge(MoveTemp(Edge));
	}
	int32 Ids[2] = {};
	for (int32& Id : Ids)
	{
		if (!TestTrue(TEXT("dispatched"), Actor->DispatchAgent(TestGraph::Probe(Net, A, B, ETraversalClass::Aircraft),
			UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
		Id = Actor->GetTraffic()->GetNewestAgentId();
	}
	const int32 One = Ids[0], Two = Ids[1];
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	Clock->SetUniformDay(1200.0);
	UFlightBoard* Board = NewObject<UFlightBoard>(GetTransientPackage());
	auto Fly = [&](int32 AgentId, const TCHAR* Callsign)
	{
		UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
		Flight->AgentId = AgentId;
		Flight->Callsign = Callsign;
		Flight->Phase = EFlightPhase::TaxiIn;
		Board->AddOffer(*Clock, Flight);
	};
	Fly(One, TEXT("G-SVBT"));
	Fly(Two, TEXT("G-HDVK"));
	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel"), Panel)) { return false; }
	Panel->UseFlightBoardForTest(Board);
	Panel->UseClockForTest(Clock);

	UGroundTraffic& Traffic = *Actor->GetGroundTraffic();
	const double Long = Traffic.Rules.StallSeconds * 2.0;
	FGroundTrafficTestAccess Access(Traffic);
	Access.ScriptWait(One, FTrafficResource::OfEdge(Lane), Two, Long);
	Access.ScriptWait(Two, FTrafficResource::OfNode(B), One, Traffic.Rules.StallSeconds * 0.5);
	FSelection Sel; Sel.Kind = ESelectionKind::Aircraft; Sel.Id = One;
	Panel->Refresh(Actor, Sel);
	const uint32 BoardRevision = Board->Revision();
	TestEqual(TEXT("its partner is not yet stalled: no ring"), Panel->DeadlockForTest(), FString());
	const FString WaitedBefore = Panel->StatusForTest();
	TestTrue(FString::Printf(TEXT("the hold line names the blocker ('%s')"), *WaitedBefore), WaitedBefore.StartsWith(TEXT("Waiting behind G-HDVK")));

	// THE PARTNER STALLS TOO: a ring, with this aircraft's own wait unchanged - only FAircraftDisplay::Partners moves.
	Access.ScriptWait(Two, FTrafficResource::OfNode(B), One, Long);
	Panel->Refresh(Actor, Sel);
	TestTrue(FString::Printf(TEXT("the ring appears at once ('%s')"), *Panel->DeadlockForTest()),
		Panel->DeadlockForTest().Contains(TEXT("Deadlocked with G-HDVK")));
	TestEqual(TEXT("and its own wait line is as it was"), Panel->StatusForTest(), WaitedBefore);

	// ITS OWN WAIT GROWS, the ring and the blocker unchanged - only FAircraftDisplay::Waited moves.
	Access.ScriptWait(One, FTrafficResource::OfEdge(Lane), Two, Long * 4.0);
	Panel->Refresh(Actor, Sel);
	TestNotEqual(FString::Printf(TEXT("the wait line counts on ('%s')"), *Panel->StatusForTest()), Panel->StatusForTest(), WaitedBefore);
	TestTrue(TEXT("naming the same blocker"), Panel->StatusForTest().StartsWith(TEXT("Waiting behind G-HDVK")));
	TestEqual(TEXT("with no flight board revision moving - the card's own key saw both"), Board->Revision(), BoardRevision);
	return true;
}

/**
 * THE QUOTE FOLLOWS THE BALANCE, AND IS ASKED ONCE PER KEY (#441). The purchase quote reads the balance, which moves no
 * revision of the job board or the network - so it was asked EVERY TICK, outside the depot card's key, although ULedger::Revision
 * exists. The ledger is in the key now: a quiet frame asks nothing (the count stays flat), and the money running out is a
 * revision like any other, so the card is described again that tick and Buy greys with it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorDepotQuoteFollowsBalanceTest,
	"AirportMgr.Inspector.Cache.DepotQuoteFollowsTheBalance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorDepotQuoteFollowsBalanceTest::RunTest(const FString& Parameters)
{
	FInspectorDepotRig Rig;
	if (!TestTrue(TEXT("setup: depot, runtime, controller and panel"), Rig.Build())) { return false; }
	Rig.Refresh();
	if (!TestTrue(TEXT("setup: a second shed is affordable"), Rig.Panel->FacilityRows->IsBuyModuleEnabledForTest())) { return false; }
	const int32 Quoted = Rig.Panel->QuoteCountForTest();
	if (!TestTrue(TEXT("setup: the first frame asked for a quote"), Quoted > 0)) { return false; }
	for (int32 Frame = 0; Frame < 30; ++Frame) { Rig.Refresh(); }
	TestEqual(TEXT("thirty quiet frames ask the runtime for no more quotes"), Rig.Panel->QuoteCountForTest(), Quoted);

	ULedger* Ledger = Rig.Runtime->GetLedger();
	if (!TestNotNull(TEXT("a ledger"), Ledger)) { return false; }
	Ledger->Post(0.0, ELedgerCategory::Fleet, -(Ledger->Balance() + 1.0e9), FText::FromString(TEXT("test: spent")));
	Rig.Refresh();
	TestEqual(TEXT("the money moving asks once more - the ledger's revision is in the card's key"), Rig.Panel->QuoteCountForTest(), Quoted + 1);
	TestFalse(TEXT("and Buy greys the tick the money is gone"), Rig.Panel->FacilityRows->IsBuyModuleEnabledForTest());
	return true;
}

#endif
