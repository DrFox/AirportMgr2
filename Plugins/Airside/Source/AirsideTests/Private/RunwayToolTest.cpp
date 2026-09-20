#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
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

	/**
	 * A target with N TRANSIENT runway profiles and nothing else real (issue #78).
	 *
	 * Before #78, FRunwayTool called UAirsideSettings::GetContent() directly, so testing its
	 * width cycle needed a project content set with at least two runway profiles authored -
	 * see M2RwyFirstRunwaySegment's own test below, which still spawns a real actor for the
	 * placement/undo/reclassify assertions that genuinely need one. This fixture proves the
	 * OTHER half moved: GetRunwayProfileCount/ResolveRunwayProfile, WidthIndex cycling and the
	 * BuildPreview label all now go through IRoadEditTarget, with no reachable path back to
	 * UAirsideContent - grep this file for it and find nothing but the two #includes above,
	 * kept only for the actor-based test that still legitimately needs real content.
	 *
	 * Every OTHER IRoadEditTarget virtual is an inert default: FRunwayTool's OnClick,
	 * BuildPreview (with no threshold placed) and NextWidth call nothing else on Target, and a
	 * body that did would be a sign this tool grew a new content dependency unnoticed.
	 */
	struct FFakeRunwayTarget : IRoadEditTarget
	{
		TArray<URoadProfile*> Profiles;

		virtual const URoadNetwork* GetNetwork() const override { return nullptr; }
		virtual int32 PlaceNode(FVector2D) override { return INDEX_NONE; }
		virtual bool ConnectNodes(int32, int32, ERoadKind, int32) override { return false; }
		using IRoadEditTarget::ConnectNodes;
		virtual int32 ConnectGuidelines(int32, int32) override { return INDEX_NONE; }
		virtual bool PlaceRunway(FVector2D, FVector2D, URoadProfile*, const FRunwayFacts&) override { return false; }
		using IRoadEditTarget::PlaceRunway;
		virtual bool SetRunwayFacts(int32, const FRunwayFacts&) override { return false; }
		virtual double GetMinimumRunwayLength() const override { return 0.0; }
		virtual bool DisconnectGuideline(int32) override { return false; }
		virtual bool SetIntermediateHoldingPosition(int32, bool) override { return false; }
		virtual int32 SplitSegment(int32, FVector2D) override { return INDEX_NONE; }
		virtual bool DeleteNode(int32) override { return false; }
		virtual bool DeleteSegment(int32) override { return false; }
		virtual bool MoveNode(int32, FVector2D) override { return false; }
		virtual bool MergeNodes(int32, int32) override { return false; }
		virtual void BeginInteractiveEdit(const FString&) override {}
		virtual void EndInteractiveEdit(bool) override {}
		virtual FRoadDeletionPlan PlanNodeDeletion(int32) const override { return FRoadDeletionPlan(); }
		virtual int32 AddApron(const TArray<FVector2D>&) override { return INDEX_NONE; }
		virtual bool DeleteApron(int32) override { return false; }
		virtual int32 FindApronAt(FVector2D) const override { return INDEX_NONE; }
		virtual int32 PlaceEntity(FVector2D, double, EPlaceableEntity) override { return INDEX_NONE; }
		virtual int32 PlaceEntityInPlot(const TArray<FVector2D>&, FVector2D, FVector2D,
			const TArray<EDepotModule>&, EPlaceableEntity) override { return INDEX_NONE; }
		using IRoadEditTarget::PlaceStand;
		virtual bool DeleteEntity(int32) override { return false; }
		virtual int32 FindEntityAt(FVector2D, double) const override { return INDEX_NONE; }
		virtual const UEntityDefinition* GetEntityDefinition(EPlaceableEntity) const override { return nullptr; }
		using IRoadEditTarget::GetStandDefinition;
		virtual void UpdateGhost(int32, const FRoadSnapResult&, bool, ERoadKind, int32) override {}
		using IRoadEditTarget::UpdateGhost;
		virtual void HideGhost() override {}
		virtual bool MakeLiveNodeId(int32, FRoadNodeId&) const override { return false; }
		virtual FRoutePlan FindRoute(FGuidelineNodeId, FGuidelineNodeId, ETraversalClass, double) const override
		{
			return FRoutePlan();
		}
		using IRoadEditTarget::DispatchAgent;
		virtual bool DispatchAgent(const FRoutePlan&, const FAirframe&, ETraversalClass) override { return false; }
		virtual void RebuildMesh() override {}

		/** No taxiway widths here: this fake is the RUNWAY tool's, and a taxiway list it
		 *  never uses would be a fixture pretending to describe something it does not. */
		virtual int32 GetTaxiwayProfileCount() const override { return 0; }
		virtual URoadProfile* ResolveTaxiwayProfile(int32) const override { return nullptr; }

		// A FAKE RESOLVES NOTHING COMPOSITE. The real rule lives on ARoadNetworkActor and is
		// pinned by Airside.Present.ProfileResolutionIsOneRule; these fakes exist to watch what
		// a tool ASKS FOR, not to re-implement what the actor answers.
		virtual URoadProfile* ResolveProfileFor(ERoadKind, int32) override { return nullptr; }

		virtual int32 GetRunwayProfileCount() const override { return Profiles.Num(); }
		virtual URoadProfile* ResolveRunwayProfile(int32 Index) const override
		{
			if (Profiles.Num() == 0)
			{
				return nullptr;
			}
			// Clamped, mirroring ARoadNetworkActor::ResolveRunwayProfile's own contract - a
			// fake that did not clamp would let a test pass against behaviour real targets
			// refuse.
			return Profiles[FMath::Clamp(Index, 0, Profiles.Num() - 1)];
		}
	};
}

/**
 * THE TOOL NO LONGER KNOWS UAirsideContent (issue #78): GetRunwayProfileCount and
 * ResolveRunwayProfile are the only two calls ProfileForWidth/NextWidth/BuildPreview make for
 * a width, and a fake target with transient profiles proves it - no project content set
 * required, unlike Airside.Tool.Runway below.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayProfileTargetTest,
	"Airside.Tool.RunwayProfileTarget",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayProfileTargetTest::RunTest(const FString& Parameters)
{
	FFakeRunwayTarget Target;
	Target.Profiles = {
		URoadProfile::MakeTransient(2300.0, 1500.0),
		URoadProfile::MakeTransient(3000.0, 1500.0),
		URoadProfile::MakeTransient(4500.0, 1500.0),
	};

	FToolContext Context;
	Context.Target = &Target;

	FRunwayTool Tool;
	TestEqual(TEXT("starts on the first width"), Tool.WidthIndex, 0);

	FM2RwyToolSink IdleSink;
	Tool.BuildPreview(Context, IdleSink);
	TestTrue(TEXT("the idle preview names the first profile's width"),
		IdleSink.Labels.Num() > 0 && IdleSink.Labels.Last().Contains(TEXT("23 m")));

	Tool.NextWidth(Context);
	TestEqual(TEXT("NextWidth cycles to the fake target's second profile"), Tool.WidthIndex, 1);

	FM2RwyToolSink SecondSink;
	Tool.BuildPreview(Context, SecondSink);
	TestTrue(TEXT("the preview now names the SECOND profile's width, from the fake target"),
		SecondSink.Labels.Num() > 0 && SecondSink.Labels.Last().Contains(TEXT("30 m")));

	Tool.NextWidth(Context);
	Tool.NextWidth(Context);
	TestEqual(TEXT("a third NextWidth wraps back to the first profile"), Tool.WidthIndex, 0);

	// CLAMPED, not refused: a WidthIndex left over from a longer list (e.g. after content is
	// edited down) must still resolve to SOMETHING rather than crash or draw nothing.
	Tool.WidthIndex = 99;
	FM2RwyToolSink ClampedSink;
	Tool.BuildPreview(Context, ClampedSink);
	TestTrue(TEXT("an out-of-range WidthIndex clamps to the last profile rather than refusing"),
		ClampedSink.Labels.Num() > 0 && ClampedSink.Labels.Last().Contains(TEXT("45 m")));
	Tool.WidthIndex = 0;

	// AN EMPTY TARGET (no profiles at all - what a fresh, unconfigured project looks like)
	// refuses cleanly rather than crashing, and NextWidth leaves WidthIndex untouched.
	FFakeRunwayTarget Empty;
	FToolContext EmptyContext;
	EmptyContext.Target = &Empty;

	FRunwayTool EmptyTool;
	EmptyTool.NextWidth(EmptyContext);
	TestEqual(TEXT("NextWidth on an empty target changes nothing"), EmptyTool.WidthIndex, 0);

	FM2RwyToolSink EmptySink;
	EmptyTool.BuildPreview(EmptyContext, EmptySink);
	TestTrue(TEXT("the idle preview on an empty target says so rather than naming a width"),
		EmptySink.Labels.Num() > 0 && EmptySink.Labels.Last().Contains(TEXT("no runway profile")));

	return true;
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

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
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
