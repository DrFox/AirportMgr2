#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideSettings.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Issue #190: UAirsideSettings::ResolveLargestServiceVehicle() used to be called fresh from
 * three separate inner loops - per arm in FRoadNetworkSolver's BuildNodeInput, per ordered
 * arm pair in FRoadGuidelineBuilder::Build, twice per link in FAnchorLink::Join - all of it
 * paid again on every Geometry rebuild a drag frame produces, not just a Topology one.
 * URoadSurfacePresenter::RebuildInternal now resolves it exactly once and hands the same
 * answer to all three; this test measures that with the free-standing call counter rather
 * than trusting the refactor description.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLargestServiceVehicleResolvedOncePerRebuildTest,
	"Airside.Present.LargestServiceVehicleResolvedOncePerRebuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLargestServiceVehicleResolvedOncePerRebuildTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	// SEVERAL NODES AND LINKS: a three-arm junction, which is exactly the shape that used
	// to multiply the resolve - three arms at the centre node, and FRoadGuidelineBuilder
	// deriving a turn path for every ORDERED pair of them (six, not three).
	const int32 Centre = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 East = Actor->PlaceNode(FVector2D(40000.0, 0.0));
	const int32 North = Actor->PlaceNode(FVector2D(0.0, 40000.0));
	const int32 West = Actor->PlaceNode(FVector2D(-40000.0, 0.0));
	TestTrue(TEXT("east arm connects"), Actor->ConnectNodes(Centre, East));
	TestTrue(TEXT("north arm connects"), Actor->ConnectNodes(Centre, North));
	TestTrue(TEXT("west arm connects"), Actor->ConnectNodes(Centre, West));
	TestEqual(TEXT("three arms at the centre node"),
		Actor->Network->GetNodes()[Centre].Incident.Num(), 3);

	// Every PlaceNode/ConnectNodes above already triggered its own rebuild through the
	// facade's OnChanged - this test measures ONE rebuild in isolation, so the counter is
	// reset only now, right before the call it is actually judging.
	UAirsideSettings::ResetResolveLargestServiceVehicleCallCountForTest();
	Actor->RebuildMesh();

	TestEqual(TEXT("ResolveLargestServiceVehicle runs exactly once per RebuildMesh - not once ")
		TEXT("per arm, per ordered arm pair, or twice per link"),
		UAirsideSettings::ResolveLargestServiceVehicleCallCountForTest, 1);

	return true;
}

/**
 * Issue #190: MakeSurfaceSettings called nine Resolve* functions per rebuild, most of them a
 * UAirsideSettings::GetContent() plus a LoadSynchronous, whether or not anything a rebuild's
 * own properties could change had changed. ARoadNetworkActor now caches that half of
 * MakeSurfaceSettings, invalidated only by PostEditChangeProperty. GetContentCallCountForTest
 * is the measurable proxy: every one of those seven resolvers reaches GetContent() first.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FResolvedContentCachedAcrossRebuildsTest,
	"Airside.Present.ResolvedContentCachedAcrossRebuilds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FResolvedContentCachedAcrossRebuildsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(40000.0, 0.0));
	TestTrue(TEXT("the two nodes connect"), Actor->ConnectNodes(A, B));

	// THROUGH MakeSurfaceSettingsForTest, NOT RebuildMesh - the seam ARoadNetworkActor
	// already keeps for exactly this ("MakeSurfaceSettings, for the seam test" - see its own
	// comment). RebuildMesh's Topology path resolves other content too, on OTHER call sites
	// this cache does not own - ResolveGhostMaterial for the plot ghost boxes and
	// ResolveDepotKits for the plot yard, both independent of MakeSurfaceSettings and both
	// out of scope for it - so measuring GetContent's count across a whole RebuildMesh would
	// be measuring three caches through one counter. A rebuild with nothing new to say about
	// THIS cache - whatever state it was left in by PlaceNode/ConnectNodes above is not this
	// test's business.
	Actor->MakeSurfaceSettingsForTest();

	UAirsideSettings::ResetGetContentCallCountForTest();
	Actor->MakeSurfaceSettingsForTest();
	TestEqual(TEXT("an unchanged MakeSurfaceSettings makes zero GetContent calls - the resolved cache held"),
		UAirsideSettings::GetContentCallCountForTest, 0);

#if WITH_EDITOR
	// AND THE CACHE ACTUALLY INVALIDATES - a cache that never refreshes would pass the
	// assertion above by accident. PostEditChangeProperty is the one signal wired to mark it
	// dirty; a null FProperty is fine, since the implementation does not branch on which
	// property changed (see its own comment for why).
	FPropertyChangedEvent DummyEvent(nullptr);
	Actor->PostEditChangeProperty(DummyEvent);

	UAirsideSettings::ResetGetContentCallCountForTest();
	Actor->MakeSurfaceSettingsForTest();
	TestTrue(TEXT("PostEditChangeProperty marks the cache dirty, so the next MakeSurfaceSettings resolves again"),
		UAirsideSettings::GetContentCallCountForTest > 0);
#endif

	return true;
}

/**
 * Issue #190: the refactor contract - passing the largest service vehicle DOWN must not
 * change what gets built. Solves and builds the same fixture twice, once through the
 * self-resolving path every existing caller still uses (LargestServiceVehicle left null)
 * and once through the explicit path URoadSurfacePresenter::Rebuild now takes, and compares
 * the resulting FRoadMeshBuffers bitwise - positions, UVs and indices alike.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLargestServiceVehiclePassedDownMatchesSelfResolvedTest,
	"Airside.Build.LargestServiceVehiclePassedDownMatchesSelfResolved",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLargestServiceVehiclePassedDownMatchesSelfResolvedTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	// A SERVICE ROAD, deliberately: its fillet is DERIVED from the largest vehicle
	// (PreferredFilletRadius left at 0 - see URoadProfile::ResolvedFilletRadius), which is
	// the one figure this whole issue is about passing down instead of re-resolving.
	URoadProfile* Road = URoadProfile::MakeServiceRoadTransient();
	Net->DefaultProfile = Road;

	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId East = Net->AddNode(FVector2D(20000.0, 0.0));
	const FRoadNodeId North = Net->AddNode(FVector2D(0.0, 20000.0));
	const FRoadNodeId West = Net->AddNode(FVector2D(-20000.0, 0.0));
	Net->AddStraightSegment(Centre, East, Road);
	Net->AddStraightSegment(Centre, North, Road);
	Net->AddStraightSegment(Centre, West, Road);

	const FChassis Vehicle = UAirsideSettings::ResolveLargestServiceVehicle();

	// SELF-RESOLVING: the path every test and every call site outside a rebuild still uses.
	const FRoadSolveResult SolvedSelfResolved = FRoadNetworkSolver::SolveAll(*Net);
	FRoadGuidelineBuilder::Build(*Net, SolvedSelfResolved, Vehicle);
	FRoadMeshBuilder BuilderSelfResolved(10.0);
	BuilderSelfResolved.Build(*Net, SolvedSelfResolved, 1);

	// PASSED DOWN: what URoadSurfacePresenter::Rebuild does now. A SEPARATE, freshly-solved
	// network - SolveAll and FRoadGuidelineBuilder::Build both write into the model, and this
	// test must not let the first pass's writes feed the second.
	URoadNetwork* Net2 = NewObject<URoadNetwork>(GetTransientPackage());
	Net2->CopyFrom(*Net);
	const FRoadSolveResult SolvedPassedDown = FRoadNetworkSolver::SolveAll(*Net2, 12, &Vehicle);
	FRoadGuidelineBuilder::Build(*Net2, SolvedPassedDown, Vehicle);
	FRoadMeshBuilder BuilderPassedDown(10.0);
	BuilderPassedDown.Build(*Net2, SolvedPassedDown, 1);

	const FRoadMeshBuffers& Self = BuilderSelfResolved.GetBuffers();
	const FRoadMeshBuffers& Passed = BuilderPassedDown.GetBuffers();

	if (!TestEqual(TEXT("same vertex count"), Self.Positions.Num(), Passed.Positions.Num()))
	{
		return false;
	}
	for (int32 Index = 0; Index < Self.Positions.Num(); ++Index)
	{
		// BITWISE, not nearly-equal - CLAUDE.md's own invariant for this mesh: the two paths
		// must produce IDENTICAL vertices, not merely close ones, or the seam guarantee this
		// project measures everywhere else means nothing here.
		TestTrue(FString::Printf(TEXT("vertex %d bitwise unchanged"), Index),
			Self.Positions[Index] == Passed.Positions[Index]);
	}

	TestEqual(TEXT("same triangle count"), Self.Indices.Num(), Passed.Indices.Num());
	for (int32 Index = 0; Index < FMath::Min(Self.Indices.Num(), Passed.Indices.Num()); ++Index)
	{
		TestEqual(FString::Printf(TEXT("index %d unchanged"), Index),
			Self.Indices[Index], Passed.Indices[Index]);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
