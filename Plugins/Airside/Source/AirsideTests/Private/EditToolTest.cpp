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
	FEditHandlesAreDeclaredForEveryRegistryEntryTest,
	"Airside.Tool.EditHandlesAreDeclaredForEveryRegistryEntry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditHandlesAreDeclaredForEveryRegistryEntryTest::RunTest(const FString& Parameters)
{
	// NAMES, NOT COUNTS. CLAUDE.md: where UE forces two lists to agree the consumer checks
	// IDENTITY and logs both on mismatch. A count would pass on a table where two entries
	// had swapped their handle kinds, which is precisely the drift this field exists to
	// make impossible.
	const TMap<FName, EEditHandleKind> Expected = {
		{ TEXT("Select"),          EEditHandleKind::None            },
		{ TEXT("Taxiway"),         EEditHandleKind::AirsideNode     },
		{ TEXT("Apron"),           EEditHandleKind::ApronCorner     },
		{ TEXT("Stand"),           EEditHandleKind::None            },
		{ TEXT("Guideline"),       EEditHandleKind::None            },
		{ TEXT("Runway"),          EEditHandleKind::RunwayThreshold },
		{ TEXT("HoldingPosition"), EEditHandleKind::None            },
		{ TEXT("Road"),            EEditHandleKind::ServiceRoadNode },
		{ TEXT("FuelDepot"),       EEditHandleKind::None            },
	};

	for (const FToolRegistration& Entry : ToolRegistry())
	{
		const EEditHandleKind* Want = Expected.Find(Entry.Id);
		if (Want == nullptr)
		{
			// A NEW TOOL MUST DECLARE WHAT EDIT MEANS FOR IT, even when the answer is None.
			// Failing rather than defaulting is the point: the default is silent, and a tool
			// that should have been editable would simply never light a handle.
			AddError(FString::Printf(
				TEXT("registry entry '%s' is not named in this test - a new tool must say "
					 "what Edit exposes for it, even if that is None"), *Entry.Id.ToString()));
			continue;
		}
		TestTrue(*FString::Printf(
			TEXT("'%s' declares the edit handles this test expects"), *Entry.Id.ToString()),
			Entry.EditHandles == *Want);
	}

	TestEqual(TEXT("and the table holds no entry beyond the ones named here"),
		ToolRegistry().Num(), Expected.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEditHandlesReachTheContextTest,
	"Airside.Tool.EditHandlesReachTheContext",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditHandlesReachTheContextTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	FBuildSession Session;
	FBuildSessionTunables Tunables;

	// DECLARING IS HALF THE WORK - the other half is the value reaching the tool. A field
	// filled in the registry and never read is the dead-list bug this codebase has shipped
	// three times (ToolCommandList, GetModeCommands, ARoadBuildController::Tools).
	Session.SelectTool(1);                       // Taxiway
	Session.SetGestureMode(EGestureMode::Edit);
	TestTrue(TEXT("the lit tool's handle kind reaches the context"),
		Session.MakeContext(Actor, FVector2D::ZeroVector, Tunables, false, false).EditHandles
			== EEditHandleKind::AirsideNode);

	// THE LIT TOOL FILTERS, so switching it changes what is grabbable without leaving Edit.
	Session.SelectTool(2);                       // Apron
	TestTrue(TEXT("switching the lit tool switches the handle kind, with Edit still held"),
		Session.MakeContext(Actor, FVector2D::ZeroVector, Tunables, false, false).EditHandles
			== EEditHandleKind::ApronCorner);

	// AND NONE OUTSIDE EDIT, so nothing downstream can act on a handle kind while the build
	// tool is the one running.
	Session.SetGestureMode(EGestureMode::Build);
	TestTrue(TEXT("no handles are offered while the mode is Build"),
		Session.MakeContext(Actor, FVector2D::ZeroVector, Tunables, false, false).EditHandles
			== EEditHandleKind::None);
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
