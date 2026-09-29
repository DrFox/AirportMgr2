#include "UI/UiWindowHost.h"
#include "CoreMinimal.h"
#include "BuildActions.h"
#include "BuildBarWidget.h"
#include "BuildHudLayer.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Entities/EntityDefinition.h"
#include "Content/AirsideSettings.h"
#include "InspectorWidget.h"
#include "Misc/AutomationTest.h"
#include "Model/InspectFacts.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "Testing/AirsideTestGraph.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/Selection.h"
#include "Profiles/RoadProfile.h"
#include "Solve/IcaoCode.h"
#include "Model/GroundTraffic.h"
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
 * very end. This measures the earlier gate (FInspectorKey) directly, through
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
		"times - FInspectorKey read unchanged, so Refresh never reran the Printf/FString::Format "
		"work SetTextCallCountForTest's own gate only compared the RESULT of"),
		Panel->ComposeCountForTest(), Before);

	return true;
}

/**
 * A SUB-KNOT SPEED CHANGE STILL RECOMPOSES - PR #329 review, on FInspectorIdleTickComposesNoTextTest's
 * own key. The Facts line prints speed TWICE from the same Shown value at two different
 * precisions - m/s to one decimal (%.1f), knots to zero (%.0f) - and 1 kt is 0.514 m/s, finer
 * than a whole knot, so FInspectorKey keying on the ROUNDED KNOT alone missed a real, displayed
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

#endif
