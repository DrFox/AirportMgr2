#include "CoreMinimal.h"
#include "InteractiveToolManager.h"
#include "Misc/AutomationTest.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildEdMode.h"
#include "RoadBuildEditorTool.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/RoadBuildTool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * Counts its own BuildPreview calls - no PRODUCTION IBuildTool does, since BuildPreview is
	 * const and none of the eight implementors have a reason to instrument themselves for a
	 * question only this review round of issue #304 asks. Everything else is the smallest
	 * legal IBuildTool: the five pure virtuals, each a no-op or a trivial answer.
	 */
	struct FCountingSpyTool : IBuildTool
	{
		mutable int32 BuildPreviewCalls = 0;

		virtual FText GetDisplayName() const override { return FText::FromString(TEXT("Spy")); }
		virtual void OnClick(const FToolContext& Context) override {}
		virtual void OnCancel(const FToolContext& Context) override {}
		virtual bool IsIdle() const override { return true; }

		virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override
		{
			++BuildPreviewCalls;

			// A REAL LABEL, so the same call this test counts is also what
			// CollectPreviewLabelTextForTest reads back - proving the cache actually carries
			// what BuildPreview described, not just that a counter moved.
			Sink.Label(FVector2D::ZeroVector, TEXT("spy label"), EPreviewStyle::Pending);
		}
	};
}

/**
 * REVIEW ROUND 2 OF ISSUE #304: the first version of the Label fix ran Tool->BuildPreview TWICE
 * every frame - once from Render (a real PDI, drawing markers/lines) and once more,
 * independently, from DrawHUD's own CollectPreviewLabels, purely to re-collect the SAME labels
 * Render's own call had already produced into its Sink and thrown away. FViewportPreviewSink::
 * Label collects regardless of whether PDI is real, so the fix is to CACHE what Render's one
 * call already gathers (PendingLabels) and have DrawHUD only ever READ it.
 *
 * NO PRODUCTION IBuildTool exposes a call count, so this measures the claim with FCountingSpyTool
 * rather than a real tool - CachePreviewLabelsForTest takes the active tool EXPLICITLY for
 * exactly this reason (see its own header comment). CachePreviewLabelsForTest stands in for
 * Render (same "no live viewport" substitution SetViewCentreDistanceForTest/
 * HoverFrameContextForTest already use); CollectPreviewLabelTextForTest stands in for DrawHUD,
 * which after this fix takes no tool at all - it only reads the cache.
 *
 * WRITTEN RED FIRST: against the first version of the fix, DrawHUD's own CollectPreviewLabels
 * call meant a second BuildPreview always ran the moment anything asked for the label text, so
 * BuildPreviewCalls read 2 by the end of this test rather than the 1 asserted below.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBuildEditorToolLabelCacheCountTest,
	"Airside.Editor.RenderCachesLabelsOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBuildEditorToolLabelCacheCountTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World))
	{
		return false;
	}
	if (!TestNotNull(TEXT("a target actor"), TestWorld.Actor))
	{
		return false;
	}

	URoadBuildEdMode* Mode = NewObject<URoadBuildEdMode>(GetTransientPackage());
	URoadBuildEditorToolBuilder* Builder = NewObject<URoadBuildEditorToolBuilder>(Mode);
	Builder->ToolIndex = 0;   // Select - irrelevant, since the spy stands in for whatever is active.
	FToolBuilderState State;
	State.ToolManager = NewObject<UInteractiveToolManager>(Mode);
	URoadBuildEditorTool* Tool = Cast<URoadBuildEditorTool>(Builder->BuildTool(State));
	if (!TestNotNull(TEXT("the builder made a tool"), Tool))
	{
		return false;
	}

	Tool->Setup();
	Tool->SetTargetForTest(TestWorld.Actor);

	// A REAL HOVER, so bHoverValid is true - CachePreviewLabelsForTest refuses (and resets the
	// cache) otherwise, the identical guard Render's own would apply.
	const double SurfaceZ = TestWorld.Actor->SurfaceZ;
	Tool->OnUpdateHover(FInputDeviceRay(
		FRay(FVector(0.0, 0.0, SurfaceZ + 1000.0), FVector(0.0, 0.0, -1.0)), FVector2D::ZeroVector));

	FCountingSpyTool Spy;

	// ONE CALL, standing in for Render.
	Tool->CachePreviewLabelsForTest(Spy);
	TestEqual(TEXT("Render's own call is the only one so far"), Spy.BuildPreviewCalls, 1);

	// "DrawHUD", reading the cache twice over (a player's mouse sitting still costs more than
	// one DrawHUD call per Render) - MUST cost ZERO further BuildPreview calls either time.
	const TArray<FString> FirstRead = Tool->CollectPreviewLabelTextForTest();
	TestEqual(TEXT("DrawHUD's first read runs BuildPreview no further times"), Spy.BuildPreviewCalls, 1);
	const TArray<FString> SecondRead = Tool->CollectPreviewLabelTextForTest();
	TestEqual(TEXT("nor does a second DrawHUD read on an unmoved frame"), Spy.BuildPreviewCalls, 1);

	TestTrue(TEXT("the cached label reached DrawHUD's read"), FirstRead.Contains(TEXT("spy label")));
	TestEqual(TEXT("and reads back identically the second time"), FirstRead.Num(), SecondRead.Num());

	Tool->Shutdown(EToolShutdownType::Cancel);
	return true;
}

#endif
