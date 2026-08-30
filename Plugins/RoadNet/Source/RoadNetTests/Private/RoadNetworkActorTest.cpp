#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadMeshBuilder.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadSlotMap.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/RoadSnap.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadNetworkActorTest,
	"RoadNet.Present.NetworkActor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadNetworkActorTest::RunTest(const FString& Parameters)
{
	ARoadNetworkActor* Actor = NewObject<ARoadNetworkActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("actor constructed"), Actor))
	{
		return false;
	}

	// The facade owns creating the network. Until Slice 3's build tool exists nothing
	// else ever would, which is exactly why RebuildMesh used to do nothing at all.
	TestTrue(TEXT("no network before the first edit"), Actor->Network == nullptr);

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(40000.0, 0.0));

	TestTrue(TEXT("placing a node creates the network"), Actor->Network != nullptr);
	TestTrue(TEXT("node handles are real"), A != INDEX_NONE && B != INDEX_NONE);
	TestEqual(TEXT("two nodes placed"), Actor->Network->GetNodes().Num(), 2);

	// Connecting is the only way a segment appears, so URoadNetwork stays the sole
	// owner of the graph invariants the solver depends on - above all the incident
	// lists being sorted by bearing.
	TestTrue(TEXT("connecting two placed nodes succeeds"), Actor->ConnectNodes(A, B));
	TestEqual(TEXT("one segment created"), Actor->Network->GetSegments().Num(), 1);

	// Rejections are reported through the return value rather than swallowed. A build
	// tool that silently drops an edit is indistinguishable from one that is broken.
	TestFalse(TEXT("a node cannot connect to itself"), Actor->ConnectNodes(A, A));
	TestFalse(TEXT("an out-of-range index is rejected"), Actor->ConnectNodes(A, 9999));
	TestFalse(TEXT("a negative index is rejected"), Actor->ConnectNodes(-1, B));
	TestEqual(TEXT("rejected connections created nothing"), Actor->Network->GetSegments().Num(), 1);

	// Picking an existing node is what makes a junction authorable at all: without it
	// every click would start a road disconnected from the last one.
	TestEqual(TEXT("finds the node under the cursor"),
		Actor->FindNodeNear(FVector2D(500.0, 0.0), 1000.0), A);
	TestEqual(TEXT("finds the nearer of two candidates"),
		Actor->FindNodeNear(FVector2D(39000.0, 0.0), 5000.0), B);
	TestEqual(TEXT("nothing inside the radius"),
		Actor->FindNodeNear(FVector2D(20000.0, 0.0), 100.0), INDEX_NONE);

	// Two segments meeting at one node is a junction - the shape Slice 2a exists to
	// render without a seam, and the thing this facade has to make reachable at runtime.
	const int32 C = Actor->PlaceNode(FVector2D(0.0, 40000.0));
	TestTrue(TEXT("a second segment can join the same node"), Actor->ConnectNodes(A, C));
	TestEqual(TEXT("the centre node carries two incident segments"),
		Actor->Network->GetNodes()[A].Incident.Num(), 2);

	// A profile is required to solve, so the facade supplies one when none is authored.
	TestTrue(TEXT("a profile was supplied for the segments"),
		Actor->Network->GetSegment(Actor->Network->GetNodes()[A].Incident[0])->Profile != nullptr);

	// End to end at the scale the runtime tool actually produces: two clicks about a
	// thousand uu apart with the default fallback profile must yield real triangles.
	// Everything above proves the graph is right, which is not the same as proving the
	// mesh comes out - and a graph that solves to an empty buffer looks, on screen,
	// exactly like a click that did nothing.
	{
		ARoadNetworkActor* Drawn = NewObject<ARoadNetworkActor>(GetTransientPackage());
		const int32 P = Drawn->PlaceNode(FVector2D(0.0, 0.0));
		const int32 Q = Drawn->PlaceNode(FVector2D(1000.0, 0.0));
		TestTrue(TEXT("two clicks connect"), Drawn->ConnectNodes(P, Q));

		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Drawn->Network);
		TestEqual(TEXT("both ends of a lone road solve"), Solved.FailedNodes, 0);
		TestEqual(TEXT("both ends produce a junction result"), Solved.SolvedNodes, 2);

		// Segments before junctions, mirroring RebuildMesh. This test exists to reproduce
		// that path end to end, so building in the order RebuildMesh forbids would make it
		// a reproduction of something the production code refuses to do.
		FRoadMeshBuilder Builder(Drawn->SurfaceZ);

		const TArray<FRoadSegment>& Segments = Drawn->Network->GetSegments();
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			if (!Segments[Index].bAlive)
			{
				continue;
			}
			FRoadSegmentId SegmentId;
			SegmentId.Index = Index;
			SegmentId.Generation = Segments[Index].Generation;
			Builder.AddSegment(*Drawn->Network, SegmentId, Drawn->RibbonSegments);
		}

		for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
		{
			static const TArray<FRoadSegmentId> NoArms;
			const TArray<FRoadSegmentId>* Arms = Solved.NodeArmSegments.Find(Pair.Key);
			Builder.AddJunction(*Drawn->Network, Pair.Key, Pair.Value, Arms ? *Arms : NoArms);
		}

		const FRoadMeshBuffers& Drawn2D = Builder.GetBuffers();
		TestTrue(
			FString::Printf(TEXT("a two-click road has triangles (got %d verts, %d tris)"),
				Drawn2D.Positions.Num(), Drawn2D.Indices.Num() / 3),
			Drawn2D.Indices.Num() > 0);

		// What FDynamicMeshSink actually does with those buffers. AppendTriangle REFUSES
		// a non-manifold or duplicate triangle rather than failing, so a buffer that is
		// correct as a triangle soup can still arrive at the component as vertices with
		// nothing joining them - which renders as nothing at all, indistinguishable from
		// a click that did nothing. Nothing above this point would notice.
		UE::Geometry::FDynamicMesh3 Mesh;
		for (const FVector3d& Position : Drawn2D.Positions)
		{
			Mesh.AppendVertex(Position);
		}

		int32 Rejected = 0;
		for (int32 Slot = 0; Slot + 2 < Drawn2D.Indices.Num(); Slot += 3)
		{
			if (Mesh.AppendTriangle(
					Drawn2D.Indices[Slot], Drawn2D.Indices[Slot + 1], Drawn2D.Indices[Slot + 2]) < 0)
			{
				++Rejected;
			}
		}

		TestEqual(
			FString::Printf(TEXT("every triangle survives AppendTriangle (%d of %d rejected)"),
				Rejected, Drawn2D.Indices.Num() / 3),
			Rejected, 0);
		TestTrue(
			FString::Printf(TEXT("the mesh handed to the component has triangles (%d)"),
				Mesh.TriangleCount()),
			Mesh.TriangleCount() > 0);
	}

	Actor->ClearNetwork();
	TestEqual(TEXT("clearing empties the nodes"), Actor->Network->GetNodes().Num(), 0);
	TestEqual(TEXT("clearing empties the segments"), Actor->Network->GetSegments().Num(), 0);

	// Splitting is what makes a T-junction authorable. Without it a junction can only form
	// where a node was already placed, so a taxiway run into a road already drawn has
	// nothing to land on. Block-scoped with its own names: V7 makes shadowing an error and
	// A, B, C are already taken above.
	{
		const int32 SplitFrom = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 SplitTo = Actor->PlaceNode(FVector2D(4000.0, 0.0));
		TestTrue(TEXT("split fixture connects"), Actor->ConnectNodes(SplitFrom, SplitTo));
		TestEqual(TEXT("one segment before the split"), Actor->Network->GetSegments().Num(), 1);

		const int32 DoomedGeneration = Actor->Network->GetSegments()[0].Generation;

		TestEqual(TEXT("an index that is not a segment is refused"),
			Actor->SplitSegment(77, FVector2D(2000.0, 0.0)), INDEX_NONE);
		TestEqual(TEXT("a refused split changes nothing"), Actor->Network->GetSegments().Num(), 1);

		const int32 Middle = Actor->SplitSegment(0, FVector2D(2000.0, 0.0));
		TestTrue(TEXT("the split creates a node"), Middle != INDEX_NONE);
		TestEqual(TEXT("three nodes after the split"), Actor->Network->GetNodes().Num(), 3);

		// Everything below indexes by Middle. Guarded rather than trusted: a split that
		// refuses returns INDEX_NONE, and indexing the array with it takes the whole
		// suite down with an access violation instead of reporting one failed assertion.
		if (Actor->Network->GetNodes().IsValidIndex(Middle))
		{

			// The original handle must be DEAD, not merely pointing at different data. The
			// slot is recycled immediately by the first replacement segment, so a caller
			// holding the old handle would otherwise silently address one half of the split
			// as though it were the whole road. Only the generation counter separates them.
			FRoadSegmentId Stale;
			Stale.Index = 0;
			Stale.Generation = DoomedGeneration;
			TestFalse(TEXT("the original segment handle no longer resolves"),
				RoadSlot::IsValid<FRoadSegmentId, FRoadSegment>(Actor->Network->GetSegments(), Stale));

			int32 LiveHalves = 0;
			for (const FRoadSegment& Half : Actor->Network->GetSegments())
			{
				if (Half.bAlive)
				{
					++LiveHalves;
				}
			}
			TestEqual(TEXT("the split leaves exactly two live segments"), LiveHalves, 2);

			TestTrue(TEXT("the new node sits exactly where it was asked for"),
				Actor->Network->GetNodes()[Middle].Position == FVector2D(2000.0, 0.0));

			// Incidence is the half that a split gets wrong quietly: a road that renders
			// correctly can still have a middle node the solver sees only one arm at.
			TestEqual(TEXT("the middle node joins both halves"),
				Actor->Network->GetNodes()[Middle].Incident.Num(), 2);
			TestEqual(TEXT("the near end keeps exactly one segment"),
				Actor->Network->GetNodes()[SplitFrom].Incident.Num(), 1);
			TestEqual(TEXT("the far end keeps exactly one segment"),
				Actor->Network->GetNodes()[SplitTo].Incident.Num(), 1);

			// Both halves must carry the original's profile, or the road changes width at a
			// point the player only asked to put a node on.
			TestTrue(TEXT("both halves inherit a profile"),
				Actor->Network->GetSegments()[0].Profile != nullptr
				&& Actor->Network->GetSegments()[1].Profile != nullptr);

		}

		// A split landing on an endpoint would produce a zero-length segment, which the
		// solver cannot trim - both its ends would sit on the same point.
		TestEqual(TEXT("splitting at an endpoint is refused"),
			Actor->SplitSegment(0, FVector2D(0.0, 0.0)), INDEX_NONE);
		TestEqual(TEXT("the refused degenerate split created no node"),
			Actor->Network->GetNodes().Num(), 3);
	}

	// The ghost is built on a DUPLICATE of the network, and this is the assertion that
	// says so. FRoadNetworkSolver::SolveAll takes a non-const network and writes trim
	// distances and cut vertices into it, so a preview solved against the real graph would
	// leave the real road's stored geometry describing a segment nobody built - correct on
	// screen until the next rebuild, then wrong, with nothing reporting it.
	{
		Actor->ClearNetwork();
		const int32 GhostFrom = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 GhostTo = Actor->PlaceNode(FVector2D(6000.0, 0.0));
		TestTrue(TEXT("ghost fixture connects"), Actor->ConnectNodes(GhostFrom, GhostTo));
		Actor->RebuildMesh();

		// Captured AFTER a real solve, so these are the values a correct ghost must not
		// disturb - not the zeroes an unsolved segment would hold.
		const FRoadSegment Before = Actor->Network->GetSegments()[0];
		const int32 NodesBefore = Actor->Network->GetNodes().Num();
		const int32 SegmentsBefore = Actor->Network->GetSegments().Num();
		TestTrue(TEXT("the fixture segment really was solved"), Before.bSolvedA && Before.bSolvedB);

		FRoadSnapResult Hypothetical;
		Hypothetical.Kind = ERoadSnapKind::Free;
		Hypothetical.Position = FVector2D(0.0, 6000.0);

		FRoadMeshBuffers GhostBuffers;
		TestTrue(TEXT("the ghost builds"),
			Actor->BuildGhostBuffers(GhostFrom, Hypothetical, GhostBuffers));
		TestTrue(TEXT("the ghost has triangles"), GhostBuffers.Indices.Num() > 0);

		// The graph itself must not have grown.
		TestEqual(TEXT("the ghost adds no node to the real network"),
			Actor->Network->GetNodes().Num(), NodesBefore);
		TestEqual(TEXT("the ghost adds no segment to the real network"),
			Actor->Network->GetSegments().Num(), SegmentsBefore);

		// Compared BITWISE, the same discipline the weld contract uses. A solve that
		// leaked into the real segment would move these by a hair, not by a mile, and a
		// tolerance here would report success on exactly the damage being looked for.
		const FRoadSegment& After = Actor->Network->GetSegments()[0];
		TestTrue(TEXT("the real segment's A cuts are untouched"),
			After.LeftCutA == Before.LeftCutA && After.RightCutA == Before.RightCutA);
		TestTrue(TEXT("the real segment's B cuts are untouched"),
			After.LeftCutB == Before.LeftCutB && After.RightCutB == Before.RightCutB);
		TestTrue(TEXT("the real segment's trims are untouched"),
			After.TrimA == Before.TrimA && After.TrimB == Before.TrimB);

		// Generations too: a ghost that mutated the real graph and tidied up after itself
		// would still have burned slots, and every outstanding handle with them.
		TestEqual(TEXT("the real segment's generation is untouched"),
			After.Generation, Before.Generation);
		TestEqual(TEXT("the start node's incidence is untouched"),
			Actor->Network->GetNodes()[GhostFrom].Incident.Num(), 1);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
