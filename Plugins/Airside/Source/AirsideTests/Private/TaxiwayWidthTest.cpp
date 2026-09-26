#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadDrawTool.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/RoadEditTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	/**
	 * A target that records the width index the tool hands it, and nothing else.
	 *
	 * THE SAME SHAPE AS FFakeRunwayTarget and for the same stated reason: the tool must be
	 * testable without UAirsideContent, because the whole point of the seam (#78) is that a
	 * tool does not know the content set. Grep this file for UAirsideContent and find
	 * nothing - if that ever stops being true, the tool has grown a content dependency.
	 * Every virtual this does not override is FNullEditTarget's inert default (#189),
	 * including GetRunwayProfileCount/ResolveRunwayProfile - this fake is the TAXIWAY tool's -
	 * and ResolveProfileFor, since a fake resolves nothing composite (the real rule lives on
	 * ARoadNetworkActor, pinned by Airside.Present.ProfileResolutionIsOneRule).
	 */
	struct FFakeWidthTarget : FNullEditTarget
	{
		TArray<URoadProfile*> TaxiwayProfiles;

		/** What the tool asked for, last time it asked. INDEX_NONE means "the default". */
		mutable int32 LastConnectWidth = -2;
		mutable int32 LastGhostWidth = -2;
		int32 Connects = 0;

		virtual int32 PlaceNode(FVector2D) override { return Connects; }
		virtual bool ConnectNodes(int32, int32, ERoadKind, int32 WidthIndex) override
		{
			LastConnectWidth = WidthIndex;
			++Connects;
			return true;
		}
		using IRoadEditTarget::ConnectNodes;
		virtual void UpdateGhost(int32, const FRoadSnapResult&, bool, ERoadKind, int32 WidthIndex) override
		{
			LastGhostWidth = WidthIndex;
		}
		using IRoadEditTarget::UpdateGhost;

		virtual int32 GetWidthCount(ERoadKind Kind) const override { return Kind == ERoadKind::Taxiway ? TaxiwayProfiles.Num() : 0; }
		virtual URoadProfile* ResolveWidthProfile(ERoadKind Kind, int32 Index) const override
		{
			if (Kind != ERoadKind::Taxiway || TaxiwayProfiles.Num() == 0)
			{
				return nullptr;
			}
			// Clamped, mirroring ARoadNetworkActor's own contract - a fake that did not
			// clamp would let a test pass against behaviour a real target refuses.
			return TaxiwayProfiles[FMath::Clamp(Index, 0, TaxiwayProfiles.Num() - 1)];
		}
	};

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaxiwayWidthTest,
	"Airside.Tool.TaxiwayWidth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiwayWidthTest::RunTest(const FString& Parameters)
{
	// A REAL WORLD for the drawing, because the click path runs through the facade and the
	// actor - a fake thin enough to record a call is not thin enough to make one happen,
	// which is how the first version of this test passed its cycling assertions and proved
	// nothing about what got laid (#104's lesson again).
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	const int32 Count = Actor->GetWidthCount(ERoadKind::Taxiway);
	if (!TestTrue(TEXT("the content set declares standard taxiway widths - run "
		"Tools/Python/build_road_profiles.py if this fails"), Count > 1))
	{
		return false;
	}

	// 1. THE DEFAULT IS UNCHANGED UNTIL ASKED FOR, and this is the assertion that matters
	//    most. Every taxiway drawn before this feature came from the actor's own instance
	//    tuning - ARoadNetworkActor::ResolveProfile, "this is per-instance tuning" - and a
	//    width cycle that silently re-pointed the default at the content set would
	//    re-profile every road on the airport the moment the tool was selected.
	{
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		TestEqual(TEXT("a fresh tool has chosen no width"), Tool.GetWidthIndex(), INDEX_NONE);

		Tool.OnClick(TestTool::ContextAt(*Actor, FVector2D(0.0, 0.0)));
		Tool.OnClick(TestTool::ContextAt(*Actor, FVector2D(4000.0, 0.0)));

		const TArray<FRoadSegment>& Segments = Actor->Network->GetSegments();
		if (TestEqual(TEXT("one segment was laid"), Segments.Num(), 1))
		{
			TestEqual(TEXT("and it carries the level's own profile, as it always did"),
				Segments[0].Profile.Get(), Actor->ResolveProfile());
		}
	}

	// 2. RESELECTING CYCLES, wrapping at the end. Key-again is the runway tool's gesture
	//    (FRunwayTool::OnReselect) and this is deliberately the same one. IT STEPS FROM THE LIT
	//    OPTION (2026-09-26, the variant row): the level's default may light a preset, and a
	//    first press that jumped back to the narrowest would read as the row and the key
	//    disagreeing. So the expectation is derived from what is lit, not assumed to be 0.
	{
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		FToolContext Context = TestTool::ContextAt(*Actor, FVector2D::ZeroVector);

		TArray<FToolVariantAxis> Axes;
		Tool.GetVariantAxes(Context, Axes);
		const int32 Lit = Axes.Num() > 0 ? Axes[0].Current : INDEX_NONE;
		const int32 First = Lit == INDEX_NONE ? 0 : (Lit + 1) % Count;

		Tool.OnReselect(Context);
		TestEqual(TEXT("the first press steps on from what is lit"), Tool.GetWidthIndex(), First);
		for (int32 Press = 1; Press < Count; ++Press)
		{
			Tool.OnReselect(Context);
		}
		TestEqual(TEXT("and a full lap of the list comes back round to one before it"),
			Tool.GetWidthIndex(), (First + Count - 1) % Count);
		Tool.OnReselect(Context);
		TestEqual(TEXT("then wraps to where it started"), Tool.GetWidthIndex(), First);
	}

	// 3. THE CHOSEN WIDTH REACHES THE ROAD. Without this the cycle is a counter that logs a
	//    number and lays the same taxiway - the "a list nothing consumes" failure this
	//    codebase has shipped three times.
	{
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		// A PICK, not a press: which index a press lands on now depends on what the level's
		// default lights (block 2), and this block needs the narrowest specifically.
		Tool.SelectVariant(TestTool::ContextAt(*Actor, FVector2D::ZeroVector), 0, 0);

		const URoadProfile* Narrowest = Actor->ResolveWidthProfile(ERoadKind::Taxiway, 0);
		if (!TestNotNull(TEXT("the narrowest profile loads"), Narrowest)) { return false; }

		const int32 Before = Actor->Network->GetSegments().Num();
		Tool.OnClick(TestTool::ContextAt(*Actor, FVector2D(0.0, 8000.0)));
		Tool.OnClick(TestTool::ContextAt(*Actor, FVector2D(4000.0, 8000.0)));

		const TArray<FRoadSegment>& Segments = Actor->Network->GetSegments();
		if (TestEqual(TEXT("a second road was laid"), Segments.Num(), Before + 1))
		{
			// Compared by WIDTH rather than by pointer: the figure is what the player sees
			// and what the junction geometry reads, and it says which profile landed
			// without the test caring which asset object backs it.
			const URoadProfile* Laid = Segments.Last().Profile.Get();
			if (TestNotNull(TEXT("the second road carries a profile"), Laid))
			{
				TestEqual(TEXT("at the width that was cycled to"),
					Laid->GetTotalWidth(), Narrowest->GetTotalWidth(), 1.0);
				TestNotEqual(TEXT("which is not the level's default, or this proves nothing"),
					Narrowest->GetTotalWidth(), Actor->ResolveProfile()->GetTotalWidth());
			}
		}
	}

	// 4. A SERVICE ROAD CYCLES ITS OWN TIERS, NEVER THE TAXIWAYS'. Until 2026-09-23 a road
	//    ignored the cycle outright, because the only list was the taxiways' and key 9 would
	//    have laid a road at a taxiway's width, paving wide for vans. The seam keys the list by
	//    kind now (Airside.Tool.ServiceRoadWidth covers the tiers); what must still hold is the
	//    danger this block was written for.
	{
		FRoadDrawTool Road(ERoadKind::ServiceRoad);
		Road.OnReselect(TestTool::ContextAt(*Actor, FVector2D::ZeroVector));
		const URoadProfile* Chosen = Actor->ResolveProfileFor(ERoadKind::ServiceRoad, Road.GetWidthIndex());
		TestTrue(TEXT("a road's first width is a road - two lanes, never a taxiway's one aircraft line"),
			Chosen != nullptr && Chosen->Guidelines.Num() == 2);
		TestTrue(TEXT("and narrower than the narrowest taxiway"),
			Chosen != nullptr && Chosen->GetTotalWidth() < Actor->ResolveWidthProfile(ERoadKind::Taxiway, 0)->GetTotalWidth());
	}

	// 5. AN EMPTY LIST REFUSES rather than choosing nothing quietly - a project that has
	//    not run build_road_profiles.py is a real state, and the one FRunwayTool's own
	//    empty-list branch exists for. A fake target is the only way to have no content.
	{
		FFakeWidthTarget Empty;
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		FToolContext Context;
		Context.Target = &Empty;
		Tool.OnReselect(Context);
		TestEqual(TEXT("with nothing to cycle, no width is chosen"),
			Tool.GetWidthIndex(), INDEX_NONE);
	}

	return true;
}

#endif
