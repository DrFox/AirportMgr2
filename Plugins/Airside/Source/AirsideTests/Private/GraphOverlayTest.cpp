#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/RoadGuideline.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/GraphOverlay.h"
#include "Tool/RoadBuildTool.h"
#include "Profiles/RoadProfile.h"
#include "StandFixture.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.
	struct FGraphSink : public IToolPreviewSink
	{
		TMap<EPreviewStyle, int32> Markers;
		TMap<EPreviewStyle, int32> LineCounts;

		virtual void Marker(const FVector2D&, EPreviewStyle Style) override
		{
			Markers.FindOrAdd(Style)++;
		}
		virtual void Line(const FVector2D&, const FVector2D&, EPreviewStyle Style) override
		{
			LineCounts.FindOrAdd(Style)++;
		}
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle Style) override
		{
			Markers.FindOrAdd(Style)++;
		}
		virtual void Label(const FVector2D&, const FString&, EPreviewStyle) override {}

		int32 CountMarkers(EPreviewStyle Style) const
		{
			const int32* Found = Markers.Find(Style);
			return Found != nullptr ? *Found : 0;
		}
		int32 CountLines(EPreviewStyle Style) const
		{
			const int32* Found = LineCounts.Find(Style);
			return Found != nullptr ? *Found : 0;
		}
	};

	/**
	 * A junction, three through nodes, a stub, and one placed stand - one of each degree
	 * the overlay has to tell apart, plus an entity with resolved anchors.
	 *
	 * Nodes and segments go through ARoadNetworkActor, like GuidelineOverlayTest's own
	 * fixture, because ConnectNodes lives on IRoadEditTarget rather than on URoadNetwork
	 * itself. The entity goes straight through URoadNetwork::PlaceEntity, as
	 * Airside.Model.Entity's own fixture does - resolving an anchor needs no actor and no
	 * solver, only the definition and a pose.
	 */
	struct FGraphOverlayFixture
	{
		ARoadNetworkActor* Actor = nullptr;
		FRoadNodeId Stub;

		explicit FGraphOverlayFixture()
		{
			Actor = NewObject<ARoadNetworkActor>(GetTransientPackage());
			if (Actor == nullptr)
			{
				return;
			}

			// Network itself is created lazily, inside Facade->PlaceNode's EnsureNetwork -
			// checking it null before the first PlaceNode call would always be true.
			//
			// A junction at the centre with three spokes, so Centre lands on NodeJunction
			// and each spoke's far end lands on NodeThrough (one incident segment).
			const int32 Centre = Actor->PlaceNode(FVector2D(0.0, 0.0));
			const int32 East = Actor->PlaceNode(FVector2D(6000.0, 0.0));
			const int32 North = Actor->PlaceNode(FVector2D(0.0, 6000.0));
			const int32 South = Actor->PlaceNode(FVector2D(0.0, -6000.0));
			Actor->ConnectNodes(Centre, East);
			Actor->ConnectNodes(Centre, North);
			Actor->ConnectNodes(Centre, South);

			// No segment at all - the case DrawNodes used to catch with StubColour, because
			// it draws no pavement whatsoever.
			Stub = Actor->Network->AddNode(FVector2D(20000.0, 20000.0));

			UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
			Actor->Network->PlaceEntity(Stand, Stand->Anchors, FVector2D(9000.0, 9000.0), 0.0);
		}

		URoadNetwork* Network() const { return Actor != nullptr ? Actor->Network.Get() : nullptr; }
	};

	/** Tally EPreviewStyle::NodeStub/NodeThrough/NodeJunction from the model directly. */
	void CountNodeStyles(const URoadNetwork& Network, int32& OutStub, int32& OutThrough, int32& OutJunction)
	{
		OutStub = 0;
		OutThrough = 0;
		OutJunction = 0;
		for (const FRoadNode& Node : Network.GetNodes())
		{
			if (!Node.bAlive)
			{
				continue;
			}
			const int32 Degree = Node.Incident.Num();
			if (Degree == 0) { ++OutStub; }
			else if (Degree >= 3) { ++OutJunction; }
			else { ++OutThrough; }
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGraphOverlayTest,
	"Airside.Tool.GraphOverlay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGraphOverlayTest::RunTest(const FString& Parameters)
{
	FGraphOverlayFixture Fixture;
	if (!TestNotNull(TEXT("fixture network built"), Fixture.Network()))
	{
		return false;
	}
	URoadNetwork& Network = *Fixture.Network();

	int32 ExpectedStub = 0;
	int32 ExpectedThrough = 0;
	int32 ExpectedJunction = 0;
	CountNodeStyles(Network, ExpectedStub, ExpectedThrough, ExpectedJunction);
	if (!TestTrue(TEXT("the fixture has one of each node degree"),
		ExpectedStub == 1 && ExpectedThrough == 3 && ExpectedJunction == 1))
	{
		return false;
	}

	const int32 AliveEntities = Network.GetEntities().Num();
	if (!TestEqual(TEXT("the fixture placed one stand"), AliveEntities, 1))
	{
		return false;
	}
	const int32 ExpectedAnchors = Network.GetEntities()[0].ResolvedAnchors.Num();
	if (!TestTrue(TEXT("the stand resolved several anchors"), ExpectedAnchors >= 4))
	{
		return false;
	}

	// 1. Node degree becomes the matching context style, measured against the model rather
	//    than a number typed here - see CountNodeStyles.
	{
		FGraphSink Sink;
		GraphOverlay::Describe(Network, Sink);

		TestEqual(TEXT("one marker per stub node"), Sink.CountMarkers(EPreviewStyle::NodeStub), ExpectedStub);
		TestEqual(TEXT("one marker per through node"), Sink.CountMarkers(EPreviewStyle::NodeThrough), ExpectedThrough);
		TestEqual(TEXT("one marker per junction node"), Sink.CountMarkers(EPreviewStyle::NodeJunction), ExpectedJunction);
	}

	// 2. A placed stand gets ONE ServiceAnchor marker per resolved anchor and its design
	//    aircraft's footprint (Snap lines, from StandPreview::DescribeBody) - and nothing
	//    else since 2026-09-27: the stop mark and pose ring left for the painted stop bar,
	//    and the fixtures, their Heal lines and the service-point Pending rings left because
	//    all three sat concentric with the anchor ring and read as one target of three
	//    circles (user report). Missing the footprint would mean GraphOverlay stopped calling
	//    DescribeBody, the duplication issue #95 was filed against.
	{
		FGraphSink Sink;
		GraphOverlay::Describe(Network, Sink);

		TestEqual(TEXT("no StandPose ring - the painted stop bar shows the stop now"),
			Sink.CountMarkers(EPreviewStyle::StandPose), 0);
		TestEqual(TEXT("one ServiceAnchor marker per resolved anchor"),
			Sink.CountMarkers(EPreviewStyle::ServiceAnchor), ExpectedAnchors);
		TestEqual(TEXT("no fixture Snap markers - concentric with the anchor ring"),
			Sink.CountMarkers(EPreviewStyle::Snap), 0);
		TestEqual(TEXT("no Heal lines to the fixtures"), Sink.CountLines(EPreviewStyle::Heal), 0);
		TestEqual(TEXT("no Pending at all - neither the stop mark nor the service points"),
			Sink.CountMarkers(EPreviewStyle::Pending), 0);
		TestTrue(TEXT("StandPreview::DescribeBody still drew the design aircraft's footprint"),
			Sink.CountLines(EPreviewStyle::Snap) > 0);
	}

	// 3. DescribeNodes/DescribeStands stay INDEPENDENT - ARoadBuildHUD gates them on
	//    separate bDrawNodes/bDrawStands flags, so a caller asking for only one must get
	//    none of the other's styles. A single combined function (what Describe itself is)
	//    would force the two to rise and fall together, which is exactly the coupling
	//    issue #95's reviewer rejected.
	{
		FGraphSink NodesOnly;
		GraphOverlay::DescribeNodes(Network, NodesOnly);

		TestTrue(TEXT("DescribeNodes alone draws the node styles"),
			NodesOnly.CountMarkers(EPreviewStyle::NodeStub) + NodesOnly.CountMarkers(EPreviewStyle::NodeThrough)
				+ NodesOnly.CountMarkers(EPreviewStyle::NodeJunction) > 0);
		TestEqual(TEXT("DescribeNodes alone draws no StandPose"),
			NodesOnly.CountMarkers(EPreviewStyle::StandPose), 0);
		TestEqual(TEXT("DescribeNodes alone draws no ServiceAnchor"),
			NodesOnly.CountMarkers(EPreviewStyle::ServiceAnchor), 0);

		FGraphSink StandsOnly;
		GraphOverlay::DescribeStands(Network, StandsOnly);

		TestTrue(TEXT("DescribeStands alone draws the footprint"),
			StandsOnly.CountLines(EPreviewStyle::Snap) > 0);
		TestTrue(TEXT("DescribeStands alone draws ServiceAnchor"),
			StandsOnly.CountMarkers(EPreviewStyle::ServiceAnchor) > 0);
		TestEqual(TEXT("DescribeStands alone draws no node style"),
			StandsOnly.CountMarkers(EPreviewStyle::NodeStub) + StandsOnly.CountMarkers(EPreviewStyle::NodeThrough)
				+ StandsOnly.CountMarkers(EPreviewStyle::NodeJunction), 0);
	}

	// 4. Killing a node takes its marker off the screen, same contract GuidelineOverlay's
	//    dead-edge test measures - a road removed in the model must not linger in the
	//    overlay.
	{
		FGraphSink Before;
		GraphOverlay::Describe(Network, Before);

		Network.RemoveNode(Fixture.Stub);

		FGraphSink After;
		GraphOverlay::Describe(Network, After);

		TestEqual(TEXT("removing the stub node leaves no NodeStub marker"),
			After.CountMarkers(EPreviewStyle::NodeStub), 0);
		TestTrue(TEXT("no other node marker grew to compensate"),
			After.CountMarkers(EPreviewStyle::NodeThrough) == Before.CountMarkers(EPreviewStyle::NodeThrough)
				&& After.CountMarkers(EPreviewStyle::NodeJunction) == Before.CountMarkers(EPreviewStyle::NodeJunction));
	}

	return true;
}

// Named apart from Airside.Tool.GraphOverlay rather than as a dotted child of it: UE's test
// tree drops a bare-named test once a child exists under its name.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotOverlayMarkersTest,
	"Airside.Tool.DepotOverlayMarkers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotOverlayMarkersTest::RunTest(const FString& Parameters)
{
	// A placed fuel depot drew an aircraft's stop mark and pose ring at its road connection -
	// two circles on the road that, zoomed out, nobody could name (2026-09-27). Its pose is a
	// truck's, not a nose gear's, so the overlay draws neither there. A stand beside it used to
	// still get both, which kept this from passing by dropping the rings for everyone; since
	// the stand's own rings went too (2026-09-27, painted stop bar - see
	// Airside.Tool.StandOverlayMarkers), the stand beside it is proved present by its footprint.
	URoadNetwork* Network = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	if (!TestNotNull(TEXT("a network"), Network) || !TestNotNull(TEXT("a depot definition"), Depot)
		|| !TestNotNull(TEXT("a stand definition"), Stand))
	{
		return false;
	}

	Network->PlaceEntity(Depot, Depot->Anchors, FVector2D(0.0, 4000.0), UE_DOUBLE_PI * 0.5,
		/*DesignWingspan=*/0.0, Depot->PoseRole, Depot->Trucks);

	{
		FGraphSink Sink;
		GraphOverlay::DescribeStands(*Network, Sink);

		TestEqual(TEXT("a placed depot draws no aircraft stop mark"), Sink.CountMarkers(EPreviewStyle::Pending), 0);
		TestEqual(TEXT("a placed depot draws no aircraft pose ring"), Sink.CountMarkers(EPreviewStyle::StandPose), 0);
		// A POINT-PLACED depot keeps its footprint box: with no plot it is the only geometry
		// the thing has.
		TestEqual(TEXT("a point-placed depot still draws its footprint box, four sides"),
			Sink.CountLines(EPreviewStyle::Snap), 4);
	}

	// A DRAWN depot does not. Its box is centred on the pose - the road connection - so on a
	// plot it was an amber rectangle straddling the gate (reported 2026-09-27), and the plot
	// the player drew already says how big the depot is.
	{
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = FVector2D(-20000.0, 1000.0);
		Placement.Heading = UE_DOUBLE_HALF_PI;
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = { FVector2D(-21000.0, 0.0), FVector2D(-19000.0, 0.0),
		                      FVector2D(-19000.0, 2400.0), FVector2D(-21000.0, 2400.0) };
		Network->PlaceEntity(Placement);

		FGraphSink Sink;
		GraphOverlay::DescribeStands(*Network, Sink);
		TestEqual(TEXT("a drawn depot adds no footprint box - still only the point-placed one's four sides"),
			Sink.CountLines(EPreviewStyle::Snap), 4);
		TestEqual(TEXT("and no stop mark either"), Sink.CountMarkers(EPreviewStyle::Pending), 0);
	}

	Network->PlaceEntity(Stand, Stand->Anchors, FVector2D(9000.0, 9000.0), 0.0);

	{
		FGraphSink Sink;
		GraphOverlay::DescribeStands(*Network, Sink);

		// Since 2026-09-27 a stand draws no rings at its pose either - its painted stop bar
		// shows the stop - so what proves the overlay still reaches the stand is its footprint.
		TestEqual(TEXT("the stand draws no pose ring either"), Sink.CountMarkers(EPreviewStyle::StandPose), 0);
		TestEqual(TEXT("nor any stop mark or service-point ring"), Sink.CountMarkers(EPreviewStyle::Pending), 0);
		TestTrue(TEXT("but the stand still draws its aircraft footprint beyond the depot's four sides"),
			Sink.CountLines(EPreviewStyle::Snap) > 4);
	}

	return true;
}

/**
 * THE STAND OVERLAY, MEASURED BY POSITION (2026-09-27, user report: "remove the circles from the
 * centre of the stand ... the debug lines to the service points ... only need to be one small
 * circle each rather than the concentric"). A count per style cannot see concentricity, so this
 * records WHERE each marker lands: nothing at the pose, and exactly one marker of any style at
 * each resolved anchor's node.
 */
namespace StandOverlayMarkersTest
{
	struct FPlacedSink : public IToolPreviewSink
	{
		TArray<TPair<FVector2D, EPreviewStyle>> Markers;
		int32 HealLines = 0;
		int32 SnapLines = 0;

		virtual void Marker(const FVector2D& At, EPreviewStyle Style) override { Markers.Emplace(At, Style); }
		virtual void Line(const FVector2D&, const FVector2D&, EPreviewStyle Style) override
		{
			if (Style == EPreviewStyle::Heal) { ++HealLines; }
			if (Style == EPreviewStyle::Snap) { ++SnapLines; }
		}
		virtual void CrossMark(const FVector2D& At, const FVector2D&, EPreviewStyle Style) override { Markers.Emplace(At, Style); }
		virtual void Label(const FVector2D&, const FString&, EPreviewStyle) override {}

		int32 MarkersNear(const FVector2D& At) const
		{
			int32 Count = 0;
			for (const TPair<FVector2D, EPreviewStyle>& M : Markers)
			{
				if (FVector2D::Distance(M.Key, At) <= 1.0) { ++Count; }
			}
			return Count;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandOverlayMarkersTest,
	"Airside.Tool.StandOverlayMarkers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandOverlayMarkersTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	if (!TestNotNull(TEXT("a network"), Network) || !TestNotNull(TEXT("a stand definition"), Stand)) { return false; }
	Network->PlaceEntity(Stand, Stand->Anchors, FVector2D(9000.0, 9000.0), 0.0);
	const FEntityInstance& Placed = Network->GetEntities()[0];
	if (!TestTrue(TEXT("the stand resolved anchors to measure"), Placed.ResolvedAnchors.Num() >= 4)) { return false; }

	StandOverlayMarkersTest::FPlacedSink Sink;
	GraphOverlay::DescribeStands(*Network, Sink);

	TestEqual(TEXT("no marker of any style at the stand's pose - the painted stop bar shows the stop"),
		Sink.MarkersNear(Placed.Position), 0);
	TestEqual(TEXT("no Heal line from the pose to a fixture"), Sink.HealLines, 0);
	TestTrue(TEXT("the design aircraft's footprint is still drawn"), Sink.SnapLines > 0);

	int32 Anchors = 0;
	for (const FResolvedAnchor& Anchor : Placed.ResolvedAnchors)
	{
		const FGuidelineNode* Node = Network->GetGuidelineNode(Anchor.Node);
		if (Node == nullptr) { continue; }
		++Anchors;
		TestEqual(FString::Printf(TEXT("exactly one marker at anchor %s - one small circle, not concentric rings"),
			*Anchor.Id.ToString()), Sink.MarkersNear(Node->Position), 1);
	}
	TestEqual(TEXT("and every marker drawn is one of those anchor rings"), Sink.Markers.Num(), Anchors);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandInStripFlaggedTest,
	"Airside.Tool.StandInStripFlagged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandInStripFlaggedTest::RunTest(const FString& Parameters)
{
	// STRIP STAGE 6: a stand a grown strip swallowed is CLOSED to new arrivals, and the overlay
	// says so - its outline in the Refused style. PAINT stays untouched (red stand paint was
	// removed on purpose, RoadSurfacePresenter); the overlay is the flag.
	URoadNetwork* Network = NewObject<URoadNetwork>(GetTransientPackage());
	Network->AddStraightSegment(Network->AddNode({ -20000.0, 0.0 }), Network->AddNode({ 20000.0, 0.0 }),
		URoadProfile::MakeTransient(2400.0, 1600.0));   // E: strip edge 12 + 28 = 40 m off
	UEntityDefinition* Def = UEntityDefinition::MakeStandTransient(EIcaoCode::B);
	const FEntityInstanceId Stand = ServiceLinkFixture::PlaceStand(*Network, *Def, FVector2D(0.0, 5000.0), 0.0);
	FRoadNetworkTestAccess(*Network).SetEntityOutlineForTest(Stand,
		{ { -2000.0, 3000.0 }, { 2000.0, 3000.0 }, { 2000.0, 7000.0 }, { -2000.0, 7000.0 } });
	{
		FGraphSink Sink;
		GraphOverlay::DescribeStands(*Network, Sink);
		TestTrue(TEXT("a stand inside the strip is outlined Refused"), Sink.CountLines(EPreviewStyle::Refused) >= 4);
	}
	FRoadNetworkTestAccess(*Network).SetEntityOutlineForTest(Stand,
		{ { -2000.0, 5000.0 }, { 2000.0, 5000.0 }, { 2000.0, 9000.0 }, { -2000.0, 9000.0 } });
	{
		FGraphSink Sink;
		GraphOverlay::DescribeStands(*Network, Sink);
		TestEqual(TEXT("one clear of it is not"), Sink.CountLines(EPreviewStyle::Refused), 0);
	}
	return true;
}

#endif
