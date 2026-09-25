#include "CoreMinimal.h"
#include "BuildActions.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
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
#include "Testing/AirsideTestWorld.h"
#include "Tool/Selection.h"
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
	FRouteQuery Q; Q.Errand = ERouteErrand::GraphProbe; Q.Policy = FRoutePolicy::For(Q.Errand); Q.Start = A; Q.Goal = B; Q.Class = ETraversalClass::Aircraft;
	if (!TestTrue(TEXT("dispatched"), Actor->DispatchAgent(RouteSearch::Find(Net, Q), UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
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
		Panel->DepartLabelColourForTest(), Style->TextMuted);

	for (int32 I = 0; I < 20000 && Actor->GetTraffic()->LastAgentPhaseForTest() != EAgentPhase::Parked; ++I) { Actor->Tick(1.0f / 30.0f); }
	Panel->Refresh(Actor, Sel);
	TestTrue(TEXT("Depart lights once parked"), Panel->IsDepartEnabledForTest());
	TestEqual(TEXT("Depart's caption returns to full Text once parked"),
		Panel->DepartLabelColourForTest(), Style->Text);
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
	FRouteQuery Q; Q.Errand = ERouteErrand::GraphProbe; Q.Policy = FRoutePolicy::For(Q.Errand); Q.Start = A; Q.Goal = B; Q.Class = ETraversalClass::Aircraft;
	if (!TestTrue(TEXT("dispatched"), Actor->DispatchAgent(RouteSearch::Find(Net, Q), UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
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
	FRouteQuery Q; Q.Errand = ERouteErrand::GraphProbe; Q.Policy = FRoutePolicy::For(Q.Errand); Q.Start = A; Q.Goal = B; Q.Class = ETraversalClass::Aircraft;
	if (!TestTrue(TEXT("dispatched"), Actor->DispatchAgent(RouteSearch::Find(Net, Q), UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
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

#endif
