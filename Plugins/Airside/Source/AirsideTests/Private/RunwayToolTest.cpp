#include "CoreMinimal.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RunwayFacts.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Tool/BuildSession.h"
#include "Tool/RunwayTool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Collects labels, so what the tool says it will lay can be read back. M2Rwy prefix: unity build. */
	struct FM2RwyToolSink : IToolPreviewSink
	{
		TArray<FString> Labels;
		virtual void Marker(const FVector2D&, EPreviewStyle) override {}
		virtual void Line(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void Label(const FVector2D&, const FString& Text, EPreviewStyle) override { Labels.Add(Text); }
	};

	/** The first live runway segment's slot index, or INDEX_NONE. */
	int32 M2RwyFirstRunwaySegment(const URoadNetwork& Net)
	{
		for (int32 Index = 0; Index < Net.GetSegments().Num(); ++Index)
		{
			const FRoadSegment& Segment = Net.GetSegments()[Index];
			if (Segment.bAlive && Segment.Profile != nullptr && Segment.Profile->bContinuousThroughJunctions)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}
}

/**
 * THE RUNWAY TOOL CHOOSES WIDTH, SURFACE AND APPROACH on its own key, and writes the facts
 * onto every segment of the strip it lays; reclassifying a strip through the actor is one
 * undoable edit. A real world and actor, because the claim reaches the undo stack.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayToolTest,
	"Airside.Tool.Runway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayToolTest::RunTest(const FString& Parameters)
{
	// The width cycle walks the content set's RunwayProfiles, so the set must be there:
	// without it the tool has no widths and the cycle would be vacuous.
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	if (!TestNotNull(TEXT("the content set is configured (DefaultAirside.ini)"), Content)) { return false; }
	if (!TestTrue(TEXT("and names at least two runway widths"), Content->RunwayProfiles.Num() >= 2)) { return false; }

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(0.0, 90000.0));   // brings the network into being; contributes no surface

	// 1. THROUGH THE SESSION: the registry's runway tool, reselected on its own key.
	FBuildSession Session;
	int32 RunwayIndex = INDEX_NONE;
	for (int32 Index = 0; Index < ToolRegistry().Num(); ++Index)
	{
		if (ToolRegistry()[Index].Key == EKeys::Six) { RunwayIndex = Index; }
	}
	if (!TestTrue(TEXT("the registry has the runway tool on 6"), RunwayIndex != INDEX_NONE)) { return false; }
	FToolContext Plain;
	Plain.Target = Actor;
	Session.SelectTool(RunwayIndex, Plain);
	FRunwayTool* Tool = static_cast<FRunwayTool*>(Session.GetActiveTool());
	if (!TestNotNull(TEXT("the runway tool is active"), Tool)) { return false; }
	TestEqual(TEXT("it starts on the first width"), Tool->WidthIndex, 0);
	TestEqual(TEXT("tarmac"), Tool->Surface, ERunwaySurface::Tarmac);
	TestEqual(TEXT("visual"), Tool->Approach, ERunwayApproach::Visual);

	Session.SelectTool(RunwayIndex, Plain);
	TestEqual(TEXT("6 again cycles the width - the caller NextWidth never had"), Tool->WidthIndex, 1);

	FToolContext Insert = Plain;
	Insert.bInsertModifier = true;
	Session.SelectTool(RunwayIndex, Insert);
	TestEqual(TEXT("Shift+6 cycles the surface"), Tool->Surface, ERunwaySurface::Concrete);
	TestEqual(TEXT("and leaves the width alone"), Tool->WidthIndex, 1);

	FToolContext Remove = Plain;
	Remove.bRemoveModifier = true;
	Session.SelectTool(RunwayIndex, Remove);
	TestEqual(TEXT("Ctrl+6 cycles the approach"), Tool->Approach, ERunwayApproach::NonPrecision);
	Session.SelectTool(RunwayIndex, Remove);
	TestEqual(TEXT("and again"), Tool->Approach, ERunwayApproach::Precision);
	Session.SelectTool(RunwayIndex, Remove);
	TestEqual(TEXT("wrapping"), Tool->Approach, ERunwayApproach::Visual);
	Session.SelectTool(RunwayIndex, Remove);

	// The label says all three before a click commits them.
	FM2RwyToolSink Sink;
	Tool->BuildPreview(Plain, Sink);
	TestTrue(TEXT("the idle label names the surface"), Sink.Labels.Num() > 0 && Sink.Labels.Last().Contains(TEXT("concrete")));
	TestTrue(TEXT("and the approach"), Sink.Labels.Num() > 0 && Sink.Labels.Last().Contains(TEXT("non-precision")));

	// 2. PLACEMENT WRITES THE FACTS onto the strip.
	FToolContext First = Plain;
	First.Cursor = FVector2D(0.0, 0.0);
	Tool->OnClick(First);
	FToolContext Second = Plain;
	Second.Cursor = FVector2D(150000.0, 0.0);
	Tool->OnClick(Second);
	TestTrue(TEXT("the tool is idle again"), Tool->IsIdle());
	const URoadNetwork* Net = Actor->GetNetwork();
	if (!TestNotNull(TEXT("the actor has a network"), Net)) { return false; }
	const int32 Placed = M2RwyFirstRunwaySegment(*Net);
	if (!TestTrue(TEXT("a runway was placed"), Placed != INDEX_NONE)) { return false; }
	const FRoadSegment& Segment = Net->GetSegments()[Placed];
	TestEqual(TEXT("with the tool's surface on it"), Segment.Runway.Surface, ERunwaySurface::Concrete);
	TestEqual(TEXT("and the tool's approach"), Segment.Runway.Approach, ERunwayApproach::NonPrecision);
	TestEqual(TEXT("and the tool's width"), Segment.Profile->GetTotalWidth(), Content->RunwayProfiles[1].LoadSynchronous()->GetTotalWidth());

	// 3. RECLASSIFYING THROUGH THE ACTOR is one undoable edit over the whole chain.
	Actor->SplitSegment(Placed, FVector2D(75000.0, 0.0));
	FRunwayFacts Grass;
	Grass.Surface = ERunwaySurface::Grass;
	Grass.Approach = ERunwayApproach::Visual;
	const int32 AnyHalf = M2RwyFirstRunwaySegment(*Actor->GetNetwork());
	if (!TestTrue(TEXT("the split left a runway segment"), AnyHalf != INDEX_NONE)) { return false; }
	TestTrue(TEXT("the facts are set through the actor"), Actor->SetRunwayFacts(AnyHalf, Grass));
	int32 RunwaySegments = 0;
	for (const FRoadSegment& S : Actor->GetNetwork()->GetSegments())
	{
		if (!S.bAlive || S.Profile == nullptr || !S.Profile->bContinuousThroughJunctions) { continue; }
		++RunwaySegments;
		TestEqual(TEXT("every segment of the strip is grass"), S.Runway.Surface, ERunwaySurface::Grass);
	}
	TestEqual(TEXT("both halves were seen"), RunwaySegments, 2);
	TestTrue(TEXT("setting the same facts again is true and no edit"), Actor->SetRunwayFacts(AnyHalf, Grass));
	TestTrue(TEXT("undo"), Actor->Undo());
	TestEqual(TEXT("takes the whole strip back to concrete"),
		Actor->GetNetwork()->GetSegments()[AnyHalf].Runway.Surface, ERunwaySurface::Concrete);

	// A taxiway is refused, and the refusal does not cost an undo step.
	const int32 TaxiA = Actor->PlaceNode(FVector2D(0.0, 50000.0));
	const int32 TaxiB = Actor->PlaceNode(FVector2D(20000.0, 50000.0));
	TestTrue(TEXT("a taxiway"), Actor->ConnectNodes(TaxiA, TaxiB));
	int32 TaxiSegment = INDEX_NONE;
	for (int32 Index = 0; Index < Actor->GetNetwork()->GetSegments().Num(); ++Index)
	{
		const FRoadSegment& S = Actor->GetNetwork()->GetSegments()[Index];
		if (S.bAlive && (S.Profile == nullptr || !S.Profile->bContinuousThroughJunctions)) { TaxiSegment = Index; }
	}
	if (!TestTrue(TEXT("found the taxiway"), TaxiSegment != INDEX_NONE)) { return false; }
	TestFalse(TEXT("a taxiway refuses runway facts"), Actor->SetRunwayFacts(TaxiSegment, Grass));
	return true;
}

#endif
