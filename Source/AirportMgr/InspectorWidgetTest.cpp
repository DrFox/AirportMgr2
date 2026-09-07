#include "CoreMinimal.h"
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

	FSelection Sel; Sel.Kind = ESelectionKind::Aircraft; Sel.Id = Id;
	Panel->Refresh(Actor, Sel);
	TestTrue(TEXT("shown with an aircraft selected"), Panel->IsShownForTest());
	TestTrue(TEXT("the title names the aircraft"), Panel->TitleForTest().Contains(FString::FromInt(Id)));
	TestFalse(TEXT("Depart is greyed while taxiing"), Panel->IsDepartEnabledForTest());

	for (int32 I = 0; I < 20000 && Actor->GetTraffic()->LastAgentPhaseForTest() != EAgentPhase::Parked; ++I) { Actor->Tick(1.0f / 30.0f); }
	Panel->Refresh(Actor, Sel);
	TestTrue(TEXT("Depart lights once parked"), Panel->IsDepartEnabledForTest());
	return true;
}

#endif
