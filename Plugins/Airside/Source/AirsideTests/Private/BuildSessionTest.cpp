#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Tool/BuildSession.h"
#include "Tool/SnapGuideChain.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildSessionTest,
	"Airside.Tool.BuildSession",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildSessionTest::RunTest(const FString& Parameters)
{
	const TConstArrayView<FToolRegistration> Registry = ToolRegistry();

	// 1. Every key reachable, and every key reaching exactly one tool - the two failure
	// modes CLAUDE.md records this project has shipped: a tool with no key, and (had two
	// entries shared a key) a key whose meaning depended on registration order.
	for (int32 Outer = 0; Outer < Registry.Num(); ++Outer)
	{
		for (int32 Inner = Outer + 1; Inner < Registry.Num(); ++Inner)
		{
			TestFalse(
				FString::Printf(TEXT("registry entries %d and %d must not share a key"), Outer, Inner),
				Registry[Outer].Key == Registry[Inner].Key);
		}
	}

	// 2. FBuildSession is built FROM the registry, not a second list that merely happens to
	// match it today.
	FBuildSession Session;
	TestEqual(TEXT("a session holds exactly as many tools as the registry lists"),
		Session.NumTools(), Registry.Num());
	TestEqual(TEXT("a fresh session opens in registry index 0 (Select)"), Session.GetActiveToolIndex(), 0);

	// 3. Selecting index i yields the tool the registry SAYS lives at i, by name - the
	// check that would have caught the editor module carrying a shorter, silently
	// re-numbered copy of this list before issue #33.
	for (int32 Index = 0; Index < Registry.Num(); ++Index)
	{
		Session.SelectTool(Index);
		const IBuildTool* Active = Session.GetActiveTool();
		if (!TestNotNull(FString::Printf(TEXT("index %d selects a real tool"), Index), Active))
		{
			continue;
		}

		TestTrue(
			FString::Printf(TEXT("tool %d's own display name agrees with its registration"), Index),
			Active->GetDisplayName().EqualTo(Registry[Index].Name));
	}

	// 4. RecordPlaneHit/LastPlaneHit - the one shared home for the fallback that used to be
	// ARoadBuildController::LastPlaneHit and URoadBuildEditorTool::HoverPosition separately
	// (issue #92). Pinned directly rather than only through MakeContext's fallback branch,
	// so a future change to that branch cannot stop exercising the pair without a test
	// noticing.
	{
		FBuildSession PlaneHitSession;
		TestTrue(TEXT("a fresh session's last plane hit is the origin"),
			PlaneHitSession.LastPlaneHit().Equals(FVector2D::ZeroVector, 1e-6));

		PlaneHitSession.RecordPlaneHit(FVector2D(1234.0, -500.0));
		TestTrue(TEXT("RecordPlaneHit's value reads back exactly"),
			PlaneHitSession.LastPlaneHit().Equals(FVector2D(1234.0, -500.0), 1e-6));
	}

	// 5. WHICH TOOLS GUIDE A GESTURE THAT HAS NOT STARTED, and it is FIVE of the nine - ruled
	// 2026-09-20: "a lot of the tools would benefit from snapping to guides before the first
	// place of the road. It doesnt make sense for all of them, but some it does."
	//
	// THE LIST IS WRITTEN HERE, BY ID, AND NOT ASKED OF THE TOOLS. Walking the registry and
	// comparing each tool's WantsFreeStartGuides() against itself would pass however the
	// virtual was answered - the vacuous shape this file's item 2 exists to avoid. Naming the
	// five is what makes a sixth tool opting in, or one of these quietly dropping out, a
	// failure rather than a change nobody notices.
	{
		const TSet<FName> FreeStarts = {
			FName(TEXT("Taxiway")),   // in line with an existing one, or a gap from a pair
			FName(TEXT("Apron")),     // an outline started flush with a road edge
			FName(TEXT("Stand")),     // level with a row of stands - see FStandPlaceTool
			FName(TEXT("Runway")),    // a threshold in line with another runway
			FName(TEXT("Road")),      // the same argument as Taxiway; one class, two entries
		};

		FBuildSession IdleSession;
		for (int32 Index = 0; Index < Registry.Num(); ++Index)
		{
			IdleSession.SelectTool(Index);
			const IBuildTool* Active = IdleSession.GetActiveTool();
			if (Active == nullptr) { continue; }

			const bool bExpected = FreeStarts.Contains(Registry[Index].Id);
			TestEqual(
				*FString::Printf(TEXT("'%s' answers the free-start question as ruled"),
					*Registry[Index].Id.ToString()),
				Active->WantsFreeStartGuides(), bExpected);

			// AND THE ANSWER REACHES THE ANCHOR. The virtual is a declaration; this is the
			// behaviour, and the two are only connected because every opted-in override
			// DELEGATES to the base instead of returning false. An override that forgot lands
			// exactly here - see IBuildTool::DescribeGuideAnchor.
			//
			// NULL NETWORK AND NULL TARGET ON PURPOSE: the first click of a session has
			// neither, and a tool must answer this without looking at either.
			FGuideAnchor Anchor;
			const bool bOffered = Active->DescribeGuideAnchor(nullptr, nullptr, Anchor);
			TestEqual(
				*FString::Printf(TEXT("'%s' offers an idle anchor exactly when it opted in"),
					*Registry[Index].Id.ToString()),
				bOffered, bExpected);

			// FLAGGED, because the flag is what FBuildSession::MakeContext branches on to put
			// the cursor in Origin. An anchor offered without it would swing around (0,0).
			TestEqual(
				*FString::Printf(TEXT("'%s' marks that idle anchor as a free start"),
					*Registry[Index].Id.ToString()),
				Anchor.bFreeStart, bExpected);
		}
	}

	return true;
}

#endif
