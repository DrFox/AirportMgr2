#include "CoreMinimal.h"
#include "BuildActions.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Content/AirsideSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "InspectorWidget.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"
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
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Ctx = GEngine->CreateNewWorldContext(EWorldType::Game);
	Ctx.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
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
	FRouteQuery Q; Q.Start = A; Q.Goal = B; Q.Class = ETraversalClass::Aircraft;
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
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Ctx = GEngine->CreateNewWorldContext(EWorldType::Game);
	Ctx.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(World, UInspectorWidget::StaticClass());
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

#endif
