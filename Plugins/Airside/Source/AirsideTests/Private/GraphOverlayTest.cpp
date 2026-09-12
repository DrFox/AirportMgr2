#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/GraphOverlay.h"
#include "Tool/RoadBuildTool.h"

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

	// 2. A placed entity gets its own committed-pose marker, its resolved anchors each get
	//    a ServiceAnchor marker, AND StandPreview::Describe still ran - Snap/Heal from the
	//    definition's fixtures, Pending for the stop mark. Missing any of the three would
	//    mean GraphOverlay reimplemented (or dropped) part of what StandPreview already
	//    does, which is the exact duplication issue #95 was filed against.
	{
		FGraphSink Sink;
		GraphOverlay::Describe(Network, Sink);

		TestEqual(TEXT("one StandPose marker per placed entity"),
			Sink.CountMarkers(EPreviewStyle::StandPose), AliveEntities);
		TestEqual(TEXT("one ServiceAnchor marker per resolved anchor"),
			Sink.CountMarkers(EPreviewStyle::ServiceAnchor), ExpectedAnchors);
		TestTrue(TEXT("StandPreview::Describe drew the definition's fixtures as Snap"),
			Sink.CountMarkers(EPreviewStyle::Snap) >= ExpectedAnchors);
		TestTrue(TEXT("StandPreview::Describe drew a Heal line per fixture"),
			Sink.CountLines(EPreviewStyle::Heal) >= ExpectedAnchors);
		// AT LEAST one per entity, not exactly: MakeStandTransient's design aircraft also
		// gets a Pending marker per service point (fuel, catering, ...) from the SAME call,
		// which is fine - the count only has to prove the stop mark is among them.
		TestTrue(TEXT("StandPreview::Describe drew at least the Pending stop mark"),
			Sink.CountMarkers(EPreviewStyle::Pending) >= AliveEntities);
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

		TestTrue(TEXT("DescribeStands alone draws StandPose"),
			StandsOnly.CountMarkers(EPreviewStyle::StandPose) > 0);
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

#endif
