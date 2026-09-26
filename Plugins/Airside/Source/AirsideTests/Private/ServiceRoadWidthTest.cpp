#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadDrawTool.h"
#include "Tool/RoadEditTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

// ROAD WIDTH TIERS (spec 2026-09-23 §1): the road tool cycles Narrow / Standard / Wide the way
// the taxiway tool cycles ICAO widths, through the SAME seam keyed by kind - one width list
// per kind, not a second parallel pair of calls.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceRoadWidthTest,
	"Airside.Tool.ServiceRoadWidth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceRoadWidthTest::RunTest(const FString& Parameters)
{
	// A REAL WORLD and the real content set, for Airside.Tool.TaxiwayWidth's reason: a fake
	// can record a call, not make a road happen.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	const int32 Count = Actor->GetWidthCount(ERoadKind::ServiceRoad);
	if (!TestEqual(TEXT("the content set declares three road tiers - run "
		"Tools/Python/build_road_profiles.py if this fails"), Count, 3))
	{
		return false;
	}
	TestTrue(TEXT("the taxiway list is a different list"),
		Actor->GetWidthCount(ERoadKind::Taxiway) != Count
		|| Actor->ResolveWidthProfile(ERoadKind::Taxiway, 0) != Actor->ResolveWidthProfile(ERoadKind::ServiceRoad, 0));

	double Previous = 0.0;
	for (int32 Tier = 0; Tier < Count; ++Tier)
	{
		const URoadProfile* Profile = Actor->ResolveWidthProfile(ERoadKind::ServiceRoad, Tier);
		if (!TestNotNull(TEXT("each tier loads"), Profile)) { return false; }
		TestTrue(TEXT("tiers run narrow to wide, the order the tool cycles in"), Profile->GetTotalWidth() > Previous);
		TestEqual(TEXT("every tier is two-way: a lane each way"), Profile->Guidelines.Num(), 2);
		Previous = Profile->GetTotalWidth();
	}

	// The default is untouched until asked for: a fresh road tool lays what it always laid.
	{
		FRoadDrawTool Tool(ERoadKind::ServiceRoad);
		Tool.OnClick(TestTool::ContextAt(*Actor, FVector2D(0.0, 0.0)));
		Tool.OnClick(TestTool::ContextAt(*Actor, FVector2D(8000.0, 0.0)));
		const TArray<FRoadSegment>& Segments = Actor->Network->GetSegments();
		if (TestEqual(TEXT("one road laid"), Segments.Num(), 1))
		{
			TestEqual(TEXT("at the default tier"), Segments[0].Profile.Get(),
				Actor->ResolveProfileFor(ERoadKind::ServiceRoad, INDEX_NONE));
		}
	}

	// Key-again cycles, wraps, and the chosen tier reaches the road.
	{
		FRoadDrawTool Tool(ERoadKind::ServiceRoad);
		const FToolContext Context = TestTool::ContextAt(*Actor, FVector2D::ZeroVector);
		// FROM WHAT IS LIT (2026-09-26): the road's default tier usually IS a content tier, so
		// it lights, and the key steps on from it - see TaxiwayWidthTest block 2. The walk to
		// the widest is then driven to index 2 rather than counted in presses.
		TArray<FToolVariantAxis> Axes;
		Tool.GetVariantAxes(Context, Axes);
		const int32 Lit = Axes.Num() > 0 ? Axes[0].Current : INDEX_NONE;
		Tool.OnReselect(Context);
		TestEqual(TEXT("the first press steps on from what is lit"), Tool.GetWidthIndex(),
			Lit == INDEX_NONE ? 0 : (Lit + 1) % Count);
		for (int32 Guard = 0; Guard < Count && Tool.GetWidthIndex() != 2; ++Guard)
		{
			Tool.OnReselect(Context);
		}
		TestEqual(TEXT("and walks to the widest"), Tool.GetWidthIndex(), 2);

		const int32 Before = Actor->Network->GetSegments().Num();
		Tool.OnClick(TestTool::ContextAt(*Actor, FVector2D(0.0, 20000.0)));
		Tool.OnClick(TestTool::ContextAt(*Actor, FVector2D(8000.0, 20000.0)));
		const TArray<FRoadSegment>& Segments = Actor->Network->GetSegments();
		if (TestEqual(TEXT("a second road laid"), Segments.Num(), Before + 1))
		{
			TestEqual(TEXT("the WIDE tier reached the road - not a counter that lays the default"),
				Segments.Last().Profile.Get(), Actor->ResolveWidthProfile(ERoadKind::ServiceRoad, 2));
		}

		Tool.OnReselect(Context);
		TestEqual(TEXT("then wraps"), Tool.GetWidthIndex(), 0);
	}
	return true;
}

namespace
{
	/** A target with no road tiers at all - what a project that never ran the script has. */
	struct FNoTiersTarget : FNullEditTarget
	{
		virtual int32 GetWidthCount(ERoadKind) const override { return 0; }
		virtual URoadProfile* ResolveWidthProfile(ERoadKind, int32) const override { return nullptr; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceRoadWidthEmptyTest,
	"Airside.Tool.ServiceRoadWidthEmpty",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceRoadWidthEmptyTest::RunTest(const FString& Parameters)
{
	// Review focus 1: nothing to cycle is said, and the tool keeps the default.
	FNoTiersTarget Target;
	FToolContext Context;
	Context.Target = &Target;
	FRoadDrawTool Tool(ERoadKind::ServiceRoad);
	Tool.OnReselect(Context);
	TestEqual(TEXT("with no tiers the tool stays on the default"), Tool.GetWidthIndex(), INDEX_NONE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadWidthResolutionTest,
	"Airside.Present.RoadWidthResolution",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadWidthResolutionTest::RunTest(const FString& Parameters)
{
	// Review focus 2: a level's own override is the DEFAULT; a cycled tier is the content's.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	if (!TestEqual(TEXT("three tiers"), Actor->GetWidthCount(ERoadKind::ServiceRoad), 3)) { return false; }

	TestEqual(TEXT("no override: the default is the narrowest tier"),
		Actor->ResolveProfileFor(ERoadKind::ServiceRoad, INDEX_NONE), Actor->ResolveWidthProfile(ERoadKind::ServiceRoad, 0));

	URoadProfile* Override = URoadProfile::MakeServiceRoadTransient(320.0);
	Actor->ServiceRoadProfile = Override;
	TestEqual(TEXT("an override is what the level lays by default"),
		Actor->ResolveProfileFor(ERoadKind::ServiceRoad, INDEX_NONE), Override);
	TestEqual(TEXT("but a cycled tier is the content set's tier, not the override"),
		Actor->ResolveProfileFor(ERoadKind::ServiceRoad, 1), Actor->ResolveWidthProfile(ERoadKind::ServiceRoad, 1));
	TestTrue(TEXT("and it is a road, never a taxiway"),
		Actor->ResolveProfileFor(ERoadKind::ServiceRoad, 1)->Guidelines.Num() == 2);
	return true;
}

#endif
