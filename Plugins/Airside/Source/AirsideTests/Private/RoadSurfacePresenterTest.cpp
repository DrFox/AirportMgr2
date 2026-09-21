#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Present/RoadSurfacePresenter.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Triangles the component's LIVE mesh actually holds - the same ground truth every
	 *  triangle-count test in this module reads, rather than trusting a builder ran. */
	int32 Triangles(UDynamicMeshComponent* Component)
	{
		return Component != nullptr ? Component->GetDynamicMesh()->GetMeshRef().TriangleCount() : -1;
	}
}

/**
 * #81's table-driven layer pipeline (RebuildLayer, ESurfaceLayer) has no test of its own -
 * only TrafficForwardersTest names URoadSurfacePresenter in passing (issue #194). Two things
 * that pipeline promises and nothing measured:
 *
 * 1. Every layer that Rebuild can populate reaches BOTH its own component (a triangle count)
 *    AND the material slot its own FSurfaceSettings knob names - not merely "some component
 *    got some material", which a Road<->Apron swap in RebuildLayer's argument order would
 *    still pass.
 * 2. RebuildSurfaceOnly (issue #165) and RebuildMarkingsOnly (issue #179) are each half of
 *    Rebuild, split so a drag frame and a flag toggle do not re-derive the guideline graph -
 *    and the split is only real if the layers each one is supposed to SKIP truly do not move.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadSurfacePresenterLayerPipelineTest,
	"Airside.Present.RoadSurfacePresenterLayerPipeline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadSurfacePresenterLayerPipelineTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor spawned"), Actor))
	{
		return false;
	}

	// THREE DISTINCT, REAL MaterialInterfaces - the engine's own built-in defaults, one per
	// domain, so a slot mix-up (Apron's mesh skinned with the road's material, say) shows as
	// the WRONG asset rather than merely "a material of some kind". Free of both a content
	// asset and a NewObject<UMaterial>() this module has never needed elsewhere.
	//
	// SET BEFORE ANYTHING ELSE TOUCHES THE ACTOR, deliberately: RunwayMarkingMaterialInstance
	// (RoadSurfacePresenter.cpp) caches its dynamic instance on FIRST USE and never remakes it,
	// so a rebuild BEFORE these were assigned would bake the content-default material into
	// that cache for the rest of the test - the exact trap this ordering avoids.
	UMaterialInterface* SurfaceMat = UMaterial::GetDefaultMaterial(MD_Surface);
	UMaterialInterface* ApronMat = UMaterial::GetDefaultMaterial(MD_PostProcess);
	UMaterialInterface* RubberMat = UMaterial::GetDefaultMaterial(MD_UI);
	Actor->SurfaceMaterial = SurfaceMat;
	Actor->ApronMaterial = ApronMat;
	Actor->RubberMaterial = RubberMat;

	// A NODE FIRST, PURELY TO BRING THE NETWORK INTO BEING - the facade creates URoadNetwork
	// lazily inside PlaceNode; see AirsideTestFixtures.h's TestGuide::LayRunway for the same
	// reason stated at more length.
	Actor->PlaceNode(FVector2D(-200000.0, -200000.0));
	if (!TestNotNull(TEXT("the actor has a network"), Actor->Network.Get()))
	{
		return false;
	}
	URoadNetwork& Net = *Actor->Network;

	// A RUNWAY (Road, RunwayPaint and RunwayRubber all read it - the last two are a pure
	// function of the strip's own length and width, no traffic needed, see
	// FRunwayMarkingBuilder::BuildRubber) crossed by a HAND-AUTHORED taxiway - bDerived=false
	// on every edge, which is what makes this survive FRoadGuidelineBuilder::Build's own
	// sweep (RoadGuidelineBuilder.cpp: "bDerived == false edges are the player's... they are
	// left in place"). TestGraph::Join defaults bDerived TRUE (the shape FCrossingFixture
	// itself uses, for the model-level tests that never call Rebuild), which a Topology
	// rebuild - AddApron below fires one - would otherwise wipe on the very first pass, taking
	// the holding-position bar's only incident edges with it.
	URoadProfile* Runway = TestProfiles::Runway();
	const FRoadNodeId RA = Net.AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId RB = Net.AddNode(FVector2D(100000.0, 0.0));
	const FRoadSegmentId Strip = Net.AddStraightSegment(RA, RB, Runway);

	TestGraph::FJoinOptions Authored;
	Authored.bDerived = false;
	const FGuidelineNodeId S = TestGraph::Node(Net, 50000.0, -20000.0);
	const FGuidelineNodeId H = TestGraph::Node(Net, 50000.0, -3000.0);
	const FGuidelineNodeId X = TestGraph::Node(Net, 50000.0, 0.0);
	const FGuidelineNodeId N = TestGraph::Node(Net, 50000.0, 20000.0);
	TestGraph::Join(Net, S, H, Authored);
	TestGraph::Join(Net, H, X, Authored);
	TestGraph::Join(Net, X, N, Authored);
	TestTrue(TEXT("the holding position is set"), Net.SetRunwayHoldingPositionForTest(H, Strip));

	TestTrue(TEXT("an apron pad, away from the strip, feeds the Apron layer"),
		Actor->AddApron({ FVector2D(200000.0, 200000.0), FVector2D(200000.0, 210000.0),
			FVector2D(210000.0, 210000.0), FVector2D(210000.0, 200000.0) }) != INDEX_NONE);

	Actor->RebuildMesh();
	URoadSurfacePresenter* Presenter = Actor->GetPresenter();
	if (!TestNotNull(TEXT("the actor has a presenter"), Presenter))
	{
		return false;
	}

	// --- 1. EVERY LAYER REACHES ITS OWN COMPONENT AND ITS OWN MATERIAL SLOT ----------------
	UDynamicMeshComponent* RoadComponent = Presenter->GetLayerComponentForTest(ESurfaceLayer::Road);
	UDynamicMeshComponent* ApronComponent = Presenter->GetLayerComponentForTest(ESurfaceLayer::Apron);
	UDynamicMeshComponent* HoldingComponent = Presenter->GetLayerComponentForTest(ESurfaceLayer::HoldingPaint);
	UDynamicMeshComponent* RunwayPaintComponent = Presenter->GetLayerComponentForTest(ESurfaceLayer::RunwayPaint);
	UDynamicMeshComponent* RunwayRubberComponent = Presenter->GetLayerComponentForTest(ESurfaceLayer::RunwayRubber);

	const int32 RoadTriangles0 = Triangles(RoadComponent);
	const int32 ApronTriangles0 = Triangles(ApronComponent);
	const int32 HoldingTriangles0 = Triangles(HoldingComponent);
	const int32 RunwayPaintTriangles0 = Triangles(RunwayPaintComponent);
	const int32 RunwayRubberTriangles0 = Triangles(RunwayRubberComponent);

	TestTrue(TEXT("Road: the strip's pavement built"), RoadTriangles0 > 0);
	TestTrue(TEXT("Apron: the pad built"), ApronTriangles0 > 0);
	TestTrue(TEXT("HoldingPaint: the crossing's bar built"), HoldingTriangles0 > 0);
	TestTrue(TEXT("RunwayPaint: threshold/centreline markings built"), RunwayPaintTriangles0 > 0);
	TestTrue(TEXT("RunwayRubber: touchdown patches built"), RunwayRubberTriangles0 > 0);

	// Road resolves through EffectiveMaterialSet - with no authored MaterialSet, that is a
	// single slot carrying SurfaceMaterial exactly (see EffectiveMaterialSet's own comment).
	TestEqual(TEXT("Road's slot is the surface material"),
		RoadComponent != nullptr ? RoadComponent->GetMaterial(0) : nullptr, SurfaceMat);
	// Apron's material is Settings.ApronMaterial directly - never a dynamic instance.
	TestEqual(TEXT("Apron's slot is the apron material, not the road's"),
		ApronComponent != nullptr ? ApronComponent->GetMaterial(0) : nullptr, ApronMat);
	// HoldingPaint is deliberately painted with the ROAD's own material (UV1=0 reads as
	// MarkingColor) - see RebuildMarkings' own comment for why that is not a mix-up.
	TestEqual(TEXT("HoldingPaint's slot is the road's own material, on purpose"),
		HoldingComponent != nullptr ? HoldingComponent->GetMaterial(0) : nullptr, SurfaceMat);
	// RunwayPaint is a dynamic instance of the surface material (white MarkingColor) - the
	// ASSET is still the surface material, one level down through Parent.
	if (UMaterialInstanceDynamic* RunwayPaintMID = RunwayPaintComponent != nullptr
		? Cast<UMaterialInstanceDynamic>(RunwayPaintComponent->GetMaterial(0)) : nullptr)
	{
		UMaterialInterface* RunwayPaintParent = RunwayPaintMID->Parent;
		TestEqual(TEXT("RunwayPaint's instance is parented to the surface material"),
			RunwayPaintParent, SurfaceMat);
	}
	else
	{
		AddError(TEXT("RunwayPaint's material is not the dynamic instance RebuildRunwayMarkings makes"));
	}
	// RunwayRubber's material is Settings.RubberMaterial directly - "the material ASSET, not
	// a dynamic instance", per RebuildRunwayRubber's own comment.
	TestEqual(TEXT("RunwayRubber's slot is the rubber material, not the road's"),
		RunwayRubberComponent != nullptr ? RunwayRubberComponent->GetMaterial(0) : nullptr, RubberMat);

	// --- 2. RebuildSurfaceOnly SKIPS THE DERIVED LAYERS (issue #165) -----------------------
	//
	// A SECOND holding position, on a node RebuildMarkings has not yet painted a bar at -
	// this is what makes "HoldingPaint did not move" a measurement rather than a tautology:
	// nothing else about the network changed, but a rebuild THAT DID touch HoldingPaint would
	// have to paint one more bar.
	TestTrue(TEXT("a second holding position is set, for RebuildMarkingsOnly to find later"),
		Net.SetRunwayHoldingPositionForTest(N, Strip));

	Presenter->RebuildSurfaceOnly(Net, Actor->MakeSurfaceSettingsForTest());

	TestEqual(TEXT("RebuildSurfaceOnly: HoldingPaint untouched despite the new holding position"),
		Triangles(HoldingComponent), HoldingTriangles0);
	TestTrue(TEXT("RebuildSurfaceOnly: Road still (re)built"), Triangles(RoadComponent) > 0);
	TestTrue(TEXT("RebuildSurfaceOnly: Apron still (re)built"), Triangles(ApronComponent) > 0);
	TestTrue(TEXT("RebuildSurfaceOnly: RunwayPaint still (re)built"), Triangles(RunwayPaintComponent) > 0);
	TestTrue(TEXT("RebuildSurfaceOnly: RunwayRubber still (re)built"), Triangles(RunwayRubberComponent) > 0);

	const int32 RoadAfterSurfaceOnly = Triangles(RoadComponent);
	const int32 ApronAfterSurfaceOnly = Triangles(ApronComponent);
	const int32 RunwayPaintAfterSurfaceOnly = Triangles(RunwayPaintComponent);
	const int32 RunwayRubberAfterSurfaceOnly = Triangles(RunwayRubberComponent);

	// --- 3. RebuildMarkingsOnly TOUCHES ONLY HoldingPaint (issue #179) ---------------------
	Presenter->RebuildMarkingsOnly(Net, Actor->MakeSurfaceSettingsForTest());

	TestTrue(TEXT("RebuildMarkingsOnly: HoldingPaint now shows the second bar"),
		Triangles(HoldingComponent) > HoldingTriangles0);
	TestEqual(TEXT("RebuildMarkingsOnly: Road untouched"), Triangles(RoadComponent), RoadAfterSurfaceOnly);
	TestEqual(TEXT("RebuildMarkingsOnly: Apron untouched"), Triangles(ApronComponent), ApronAfterSurfaceOnly);
	TestEqual(TEXT("RebuildMarkingsOnly: RunwayPaint untouched"),
		Triangles(RunwayPaintComponent), RunwayPaintAfterSurfaceOnly);
	TestEqual(TEXT("RebuildMarkingsOnly: RunwayRubber untouched"),
		Triangles(RunwayRubberComponent), RunwayRubberAfterSurfaceOnly);

	return true;
}

#endif
