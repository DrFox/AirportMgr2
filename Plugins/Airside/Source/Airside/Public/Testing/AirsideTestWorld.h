#pragma once

// TEST-ONLY, and guarded because of it: WITH_DEV_AUTOMATION_TESTS is 0 in a shipping build,
// so a fixture header with no consumer left in that config must not even try to compile -
// see CLAUDE.md on plain enums and UHT for the sibling rule this mirrors (a thing that is
// not there for one build config must not be reasoned about as though it were).
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Present/AirsideBuildingsActor.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/RoadEditTarget.h"

/**
 * A world - and, by default, a network actor - to test through the composition root rather
 * than the model alone. RAII so a test that returns early (TestTrue(...) { return false; },
 * this codebase's idiom throughout) still tears the world down.
 *
 * ISSUE #189: this is the CreateWorld / CreateNewWorldContext / SpawnActor<ARoadNetworkActor>
 * / DestroyWorldContext + DestroyWorld sequence that AirsideTests (via AirsideTestFixtures.h,
 * which now forwards here rather than declaring its own copy), 10 AirportMgr test files, 2
 * AirportOpsTests files and 1 AirsideEditor file each wrote out by hand - 20+ identical
 * five-line copies, and world teardown is the copy most likely to drift: a missing
 * DestroyWorldContext leaks a world into the NEXT test and shows up as a crash in an
 * unrelated file. Public here, not in AirsideTests, because AirportMgr/AirportOpsTests/
 * AirsideEditor all already depend on the Airside module (never the reverse) and none of
 * them may depend on a test module - a Public header behind WITH_DEV_AUTOMATION_TESTS costs
 * nothing in a shipping build and needs no new module.
 *
 * bSpawnActor=false SKIPS ARoadNetworkActor: a widget or controller test (BuildActionsTest,
 * ToastStackWidgetTest, ...) wants a bare UWorld to spawn its OWN actor in - or none at all -
 * and has no use for a road network actor riding along uninvited. RoadBuildEdModeSessionTest
 * also passes false: it needs URoadBuildEdMode::MakeReselectContext to FIND OR CREATE the
 * actor itself (see WorldOverrideForTest's own comment), and a pre-spawned one here would
 * leave that call path untested.
 *
 * WorldType defaults to EWorldType::Game, what every caller but one wanted; that one
 * (RoadBuildEdModeSessionTest, an editor-mode test) passes EWorldType::Editor to match what
 * URoadBuildEdMode::GetWorld() returns in the real editor - nothing FindOrCreate or GetWorld's
 * override reads branches on it, so this is cosmetic fidelity, not a functional requirement.
 */
struct FAirsideTestWorld
{
	UWorld* World = nullptr;

	/** Null when constructed with bSpawnActor=false. */
	ARoadNetworkActor* Actor = nullptr;

	/**
	 * Null when constructed with bSpawnActor=false. Spawned AFTER Actor, so it finds and binds
	 * to it on registration - the real path, not a test-only BindTo. EVERY TEST THAT SPAWNS A
	 * ROAD NETWORK GETS ONE because plots stopped being the road network's to draw on
	 * 2026-09-22; a fixture without it would pass every plot test that forgot to ask.
	 */
	AAirsideBuildingsActor* Buildings = nullptr;

	explicit FAirsideTestWorld(bool bSpawnActor = true, EWorldType::Type WorldType = EWorldType::Game)
	{
		World = UWorld::CreateWorld(WorldType, false);
		if (World == nullptr) { return; }
		FWorldContext& Context = GEngine->CreateNewWorldContext(WorldType);
		Context.SetCurrentWorld(World);
		if (bSpawnActor)
		{
			Actor = World->SpawnActor<ARoadNetworkActor>();
			Buildings = World->SpawnActor<AAirsideBuildingsActor>();
		}
	}

	~FAirsideTestWorld()
	{
		if (World == nullptr) { return; }
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	}

	FAirsideTestWorld(const FAirsideTestWorld&) = delete;
	FAirsideTestWorld& operator=(const FAirsideTestWorld&) = delete;
};

/**
 * Captures every line logged to CATEGORY at exactly ELogVerbosity::Log while this spy is
 * registered (AddOutputDevice/RemoveOutputDevice bracketing a single call) - the shape
 * RoadRebuildLogQuietTest's FLogRoadMeshLogSpy and GroundTrafficTest's FLogAirsideTrafficSpy
 * each hand-wrote, one field apart. Issue #216d: a second near-identical copy is how the
 * SAME bug (below) gets shipped twice, so this is the one place that carries the fix, not a
 * pattern for each new spy to remember to copy.
 *
 * ISSUE #216 ROOT CAUSE (fourth/fifth sighting on RoadRebuildLogQuietTest, before this class
 * existed): an FOutputDevice that does not override CanBeUsedOnMultipleThreads defaults to
 * false, which FOutputDeviceRedirectorState::AddOutputDevice (Engine/Source/Runtime/Core/
 * Private/Misc/OutputDeviceRedirector.cpp) files under BUFFERED, not unbuffered.
 * LaunchEngineLoop.cpp calls GLog->TryStartDedicatedPrimaryThread() at boot (unless
 * "-NoLogThread" is passed, which Run-AirsideTests.ps1 does not), and once that thread exists
 * IT becomes the redirector's "primary thread" - so a UE_LOG call from THIS test's own game
 * thread no longer qualifies for synchronous delivery to a buffered device (see Serialize()'s
 * IsPrimaryThread(ThreadId) check); the line is enqueued instead and only reaches this spy
 * whenever that background thread next wakes and drains the queue, entirely independent of
 * when AddOutputDevice/RemoveOutputDevice bracket the call. Under the full suite's log volume
 * that thread lags far enough behind that lines are still queued - sometimes all of them,
 * sometimes only the tail - when RemoveOutputDevice fires, which is why isolated
 * `-Filter` runs (far less log volume, the thread stays caught up) do not reproduce it.
 * Engine/Source/Runtime/Core/Public/Misc/AutomationTest.h's own FAutomationTestOutputDevice
 * and FAutomationTestMessageFilter hit exactly this and override this the same way, captioned
 * "Make it unbuffered by returning true" - every spy of this shape needs the identical
 * override for the identical reason: it is added and removed around a narrow window and must
 * not race a thread it does not control. Check-Architecture.ps1 rule 10 enforces the override
 * on any OTHER FOutputDevice subclass a future test writes by hand instead of using this one.
 */
struct FLogLineSpy : public FOutputDevice
{
	/** The log category this spy watches; set at construction, never changed. */
	FName Category;

	/** CapturedLines.Num(), kept as its own field so a caller need not spell the array out
	 *  for a bare count (RoadRebuildLogQuietTest's DragSpy.Count != 0 check, for one). */
	int32 Count = 0;

	/** Verbatim, not re-derived from Count: a failure should name the actual line rather than
	 *  ask a human to reproduce it under a debugger to find out (issue #216, third sighting). */
	TArray<FString> CapturedLines;

	explicit FLogLineSpy(FName InCategory) : Category(InCategory) {}

	virtual bool CanBeUsedOnMultipleThreads() const override
	{
		return true;
	}

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& InCategory) override
	{
		if (InCategory == Category && Verbosity == ELogVerbosity::Log)
		{
			++Count;
			CapturedLines.Add(FString(V));
		}
	}
};

/**
 * Every IRoadEditTarget pure virtual defaulted to an inert answer (false / INDEX_NONE /
 * nullptr / empty) - issue #189. RunwayToolTest.cpp's FFakeRunwayTarget and
 * TaxiwayWidthTest.cpp's FFakeWidthTarget each hand-stubbed all 35 of them and differed in
 * only two or three; deriving from this and overriding just the ones a test actually cares
 * about is the SAME fake with the boilerplate held once, matching every non-virtual overload
 * the interface itself provides (ConnectNodes, PlaceRunway, PlaceStand, UpdateGhost,
 * DispatchAgent, GetStandDefinition all stay reachable through `using` here so a derived
 * fake need not re-declare them).
 *
 * NOT for every call site: SelectToolTest.cpp and GuidelineOverlayTest.cpp spawn a whole
 * FAirsideTestWorld to get an IRoadEditTarget because their tests exercise the ACTOR's own
 * implementation (traffic, guideline graph, entities) and a null target would answer nothing
 * they ask - converting those to this fake is a different, larger change and is left alone
 * here (see issue #189's "not done").
 */
struct FNullEditTarget : IRoadEditTarget
{
	virtual const URoadNetwork* GetNetwork() const override { return nullptr; }
	virtual int32 PlaceNode(FVector2D) override { return INDEX_NONE; }
	virtual bool ConnectNodes(int32, int32, ERoadKind, int32) override { return false; }
	using IRoadEditTarget::ConnectNodes;
	virtual int32 ConnectGuidelines(int32, int32) override { return INDEX_NONE; }
	virtual bool PlaceRunway(FVector2D, FVector2D, URoadProfile*, const FRunwayFacts&) override { return false; }
	using IRoadEditTarget::PlaceRunway;
	virtual bool SetRunwayFacts(int32, const FRunwayFacts&) override { return false; }
	virtual double GetMinimumRunwayLength() const override { return 0.0; }
	virtual int32 GetRunwayProfileCount() const override { return 0; }
	virtual URoadProfile* ResolveRunwayProfile(int32) const override { return nullptr; }
	virtual int32 GetTaxiwayProfileCount() const override { return 0; }
	virtual URoadProfile* ResolveTaxiwayProfile(int32) const override { return nullptr; }
	virtual URoadProfile* ResolveProfileFor(ERoadKind, int32) override { return nullptr; }
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
	virtual bool MoveApronCorner(int32, int32, FVector2D) override { return false; }
	virtual int32 PlaceEntity(FVector2D, double, EPlaceableEntity) override { return INDEX_NONE; }
	using IRoadEditTarget::PlaceStand;
	virtual int32 PlaceEntityInPlot(const TArray<FVector2D>&, FVector2D, FVector2D,
		const TArray<EDepotModule>&, EPlaceableEntity) override { return INDEX_NONE; }
	virtual bool DeleteEntity(int32) override { return false; }
	virtual int32 FindEntityAt(FVector2D, double) const override { return INDEX_NONE; }
	virtual const UEntityDefinition* GetEntityDefinition(EPlaceableEntity) const override { return nullptr; }
	using IRoadEditTarget::GetStandDefinition;
	virtual void UpdateGhost(int32, const FRoadSnapResult&, bool, ERoadKind, int32) override {}
	using IRoadEditTarget::UpdateGhost;
	virtual void HideGhost() override {}
	virtual bool MakeLiveNodeId(int32, FRoadNodeId&) const override { return false; }
	virtual FRoutePlan FindRoute(FGuidelineNodeId, FGuidelineNodeId, ETraversalClass, double,
		ERouteErrand Errand = ERouteErrand::PlayerIssued) const override
	{
		return FRoutePlan();
	}
	virtual bool DispatchAgent(const FRoutePlan&, const FAirframe&, ETraversalClass) override { return false; }
	virtual bool DispatchAgent(const FRoutePlan&, const FVehicle&, ETraversalClass) override { return false; }
	using IRoadEditTarget::DispatchAgent;
	virtual void RebuildMesh() override {}

	/** A FAKE RESOLVES NOTHING COMPOSITE, same as ResolveProfileFor above - the real table
	 *  lives on ARoadNetworkActor and is pinned by Airside.Tool.PlotSpecsComeFromTheTarget
	 *  (issue #181). Most derived fakes never touch a plot; the ones that do override this. */
	virtual TArray<PlotYard::FKitSpec> ResolveDepotKits() const override { return {}; }
};

#endif // WITH_DEV_AUTOMATION_TESTS
