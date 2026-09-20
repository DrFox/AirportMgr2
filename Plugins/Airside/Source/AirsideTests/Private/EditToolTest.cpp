#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/BuildSession.h"
#include "Tool/EditTool.h"
#include "Tool/RoadBuildTool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Prefixed against the UNITY build - these test files share one translation unit. */
	struct FEditToolSink : public IToolPreviewSink
	{
		TMap<EPreviewStyle, int32> Markers;
		TMap<EPreviewStyle, int32> Lines;
		TArray<FString> Labels;

		virtual void Marker(const FVector2D&, EPreviewStyle Style) override
		{
			Markers.FindOrAdd(Style)++;
		}
		virtual void Line(const FVector2D&, const FVector2D&, EPreviewStyle Style) override
		{
			Lines.FindOrAdd(Style)++;
		}
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle Style) override
		{
			Markers.FindOrAdd(Style)++;
		}
		virtual void Label(const FVector2D&, const FString& Text, EPreviewStyle) override
		{
			Labels.Add(Text);
		}

		int32 CountMarkers(EPreviewStyle Style) const
		{
			const int32* Found = Markers.Find(Style);
			return Found != nullptr ? *Found : 0;
		}
		int32 CountLines(EPreviewStyle Style) const
		{
			const int32* Found = Lines.Find(Style);
			return Found != nullptr ? *Found : 0;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEditModeSuppressesTheBuildToolTest,
	"Airside.Tool.EditModeSuppressesTheBuildTool",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditModeSuppressesTheBuildToolTest::RunTest(const FString& Parameters)
{
	// A REAL WORLD, not a bare NewObject: this goes through the facade, and a half-built
	// actor is not evidence about it (#104).
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	FBuildSession Session;
	TestTrue(TEXT("a session opens in Build, so nothing is editable until it is asked for"),
		Session.GetGestureMode() == EGestureMode::Build);

	// Taxiway (registry index 1), so the tool under test is one that DOES build on a click.
	Session.SelectTool(1);
	Session.SetGestureMode(EGestureMode::Edit);

	FBuildSessionTunables Tunables;
	const FToolContext Context =
		Session.MakeContext(Actor, FVector2D(1000.0, 1000.0), Tunables, false, false);

	IBuildTool* Active = Session.GetActiveTool();
	if (!TestNotNull(TEXT("a tool is active in Edit"), Active)) { return false; }
	Active->OnClick(Context);

	// THE WHOLE POINT OF THE MODE. A click that built here is the misclick this feature
	// exists to remove, only running in the opposite direction.
	const URoadNetwork* Network = Actor->GetNetwork();
	const int32 Nodes = Network != nullptr ? Network->GetNodes().Num() : 0;
	TestEqual(TEXT("a click in Edit builds nothing, because the build tool does not run"),
		Nodes, 0);

	// AND THE SAME CLICK IN BUILD DOES BUILD, so this is measuring the mode rather than a
	// tool that was broken all along - the control the rule needs to mean anything.
	Session.SetGestureMode(EGestureMode::Build);
	Session.GetActiveTool()->OnClick(
		Session.MakeContext(Actor, FVector2D(1000.0, 1000.0), Tunables, false, false));
	TestEqual(TEXT("the identical click in Build lays a node"),
		Actor->GetNetwork()->GetNodes().Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEditModeNamesItselfTest,
	"Airside.Tool.EditModeNamesItself",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditModeNamesItselfTest::RunTest(const FString& Parameters)
{
	// The overlay draws GetDisplayName so the active mode is never a thing you have to
	// remember. An edit tool reporting the build tool's name would make the mode invisible
	// at exactly the moment the player needs to know they are in it.
	FBuildSession Session;
	Session.SelectTool(1);
	const FString BuildName = Session.GetActiveTool()->GetDisplayName().ToString();

	Session.SetGestureMode(EGestureMode::Edit);
	const FString EditName = Session.GetActiveTool()->GetDisplayName().ToString();

	TestEqual(TEXT("the edit tool names itself Edit"), EditName, FString(TEXT("Edit")));
	TestNotEqual(TEXT("and that is not the lit build tool's name"), EditName, BuildName);
	return true;
}

#endif
