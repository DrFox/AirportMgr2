#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadMeshBuilder.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/MeshNormals.h"
#include "Model/RoadApron.h"
#include "Model/RoadNetwork.h"
#include "Components/DynamicMeshComponent.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/RoadGeom.h"
#include "Tool/ApronDrawTool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FToolContext ApronAt(ARoadNetworkActor* Actor, const FVector2D& Where)
	{
		FToolContext Context;
		Context.Target = Actor;
		Context.Cursor = Where;
		Context.SnapRadius = 150.0;
		Context.Snap.Kind = ERoadSnapKind::Free;
		Context.Snap.Position = Where;
		return Context;
	}

	int32 LiveAprons(const ARoadNetworkActor* Actor)
	{
		int32 Alive = 0;
		for (const FApronSurface& Apron : Actor->Network->GetAprons())
		{
			if (Apron.bAlive) { ++Alive; }
		}
		return Alive;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApronDrawToolTest,
	"Airside.Tool.ApronDraw",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FApronDrawToolTest::RunTest(const FString& Parameters)
{
	ARoadNetworkActor* Actor = NewObject<ARoadNetworkActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("actor constructed"), Actor))
	{
		return false;
	}

	// --- Drawing an outline, corner by corner ----------------------------------------
	{
		Actor->ClearNetwork();
		FApronDrawTool Tool;
		TestTrue(TEXT("a fresh tool has nothing part-drawn"), Tool.IsIdle());

		Tool.OnClick(ApronAt(Actor, FVector2D(0.0, 0.0)));
		TestFalse(TEXT("the first click starts an outline"), Tool.IsIdle());
		TestEqual(TEXT("with one corner"), Tool.GetCorners().Num(), 1);

		Tool.OnClick(ApronAt(Actor, FVector2D(8000.0, 0.0)));
		Tool.OnClick(ApronAt(Actor, FVector2D(8000.0, 6000.0)));
		Tool.OnClick(ApronAt(Actor, FVector2D(0.0, 6000.0)));
		TestEqual(TEXT("four corners placed"), Tool.GetCorners().Num(), 4);
		TestEqual(TEXT("and nothing committed yet"), LiveAprons(Actor), 0);

		// Clicking near the first corner closes it. Near, not exactly on it - the cursor
		// never lands on a stored coordinate.
		Tool.OnClick(ApronAt(Actor, FVector2D(60.0, 40.0)));
		TestTrue(TEXT("closing on the first corner ends the outline"), Tool.IsIdle());
		TestEqual(TEXT("and commits one apron"), LiveAprons(Actor), 1);
		TestEqual(TEXT("with the four corners drawn, not the closing click"),
			Actor->Network->GetAprons()[0].Outline.Num(), 4);
	}

	// Three corners is the least that encloses anything, so closing is not offered before.
	{
		Actor->ClearNetwork();
		FApronDrawTool Tool;

		Tool.OnClick(ApronAt(Actor, FVector2D(0.0, 0.0)));
		Tool.OnClick(ApronAt(Actor, FVector2D(4000.0, 0.0)));

		// Back on the first corner with only two down: this must add a corner, not close.
		Tool.OnClick(ApronAt(Actor, FVector2D(50.0, 0.0)));
		TestFalse(TEXT("two corners cannot be closed into an apron"), Tool.IsIdle());
		TestEqual(TEXT("the click placed a corner instead"), Tool.GetCorners().Num(), 3);
		TestEqual(TEXT("and committed nothing"), LiveAprons(Actor), 0);
	}

	// --- Backing out one corner at a time ---------------------------------------------
	{
		Actor->ClearNetwork();
		FApronDrawTool Tool;

		Tool.OnClick(ApronAt(Actor, FVector2D(0.0, 0.0)));
		Tool.OnClick(ApronAt(Actor, FVector2D(5000.0, 0.0)));
		Tool.OnClick(ApronAt(Actor, FVector2D(5000.0, 5000.0)));

		Tool.OnCancel(ApronAt(Actor, FVector2D(0.0, 0.0)));
		TestEqual(TEXT("cancel removes the last corner only"), Tool.GetCorners().Num(), 2);
		TestFalse(TEXT("and the outline is still in progress"), Tool.IsIdle());

		Tool.OnCancel(ApronAt(Actor, FVector2D(0.0, 0.0)));
		Tool.OnCancel(ApronAt(Actor, FVector2D(0.0, 0.0)));
		TestTrue(TEXT("cancelling past the first corner ends the outline"), Tool.IsIdle());
		TestEqual(TEXT("and leaves nothing behind"), LiveAprons(Actor), 0);
	}

	// --- A corner that would tangle the outline is refused ----------------------------
	{
		Actor->ClearNetwork();
		FApronDrawTool Tool;

		// Three corners of a square, then a fourth placed so the edge to it crosses the
		// first edge - the classic figure-eight.
		Tool.OnClick(ApronAt(Actor, FVector2D(0.0, 0.0)));
		Tool.OnClick(ApronAt(Actor, FVector2D(6000.0, 0.0)));
		Tool.OnClick(ApronAt(Actor, FVector2D(6000.0, 6000.0)));

		Tool.OnClick(ApronAt(Actor, FVector2D(3000.0, -3000.0)));
		TestEqual(TEXT("a corner that would cross the outline is refused"),
			Tool.GetCorners().Num(), 3);

		// And a legal one still lands, so the refusal is about crossing rather than about
		// having run out of corners.
		Tool.OnClick(ApronAt(Actor, FVector2D(0.0, 6000.0)));
		TestEqual(TEXT("a corner that does not cross still lands"), Tool.GetCorners().Num(), 4);
	}

	// --- Leaving the tool -------------------------------------------------------------
	{
		Actor->ClearNetwork();
		FApronDrawTool Tool;

		Tool.OnClick(ApronAt(Actor, FVector2D(0.0, 0.0)));
		Tool.OnClick(ApronAt(Actor, FVector2D(3000.0, 0.0)));
		Tool.OnDeactivate(ApronAt(Actor, FVector2D(0.0, 0.0)));

		TestTrue(TEXT("switching tools discards a part-drawn outline"), Tool.IsIdle());
		TestEqual(TEXT("and commits nothing"), LiveAprons(Actor), 0);
	}

	// --- The facade's own guards ------------------------------------------------------
	{
		Actor->ClearNetwork();

		TestEqual(TEXT("two corners are not an apron"),
			Actor->AddApron({ FVector2D(0.0, 0.0), FVector2D(1000.0, 0.0) }), INDEX_NONE);

		// A figure-eight. The triangulator's contract is a simple polygon and it produces
		// overlapping triangles rather than an error, so this must be refused here.
		TestEqual(TEXT("a self-crossing outline is refused"),
			Actor->AddApron({ FVector2D(0.0, 0.0), FVector2D(4000.0, 4000.0),
				FVector2D(4000.0, 0.0), FVector2D(0.0, 4000.0) }), INDEX_NONE);

		// Clockwise in, counter-clockwise stored: corrected rather than refused.
		const int32 Reversed = Actor->AddApron({ FVector2D(0.0, 0.0), FVector2D(0.0, 5000.0),
			FVector2D(5000.0, 5000.0), FVector2D(5000.0, 0.0) });
		if (TestTrue(TEXT("a clockwise outline is accepted"), Reversed != INDEX_NONE))
		{
			TestTrue(TEXT("and stored counter-clockwise"),
				RoadGeom::PolygonArea(Actor->Network->GetAprons()[Reversed].Outline) > 0.0);
		}

		// Picking, which is what Ctrl+click removes by.
		TestEqual(TEXT("a point inside is found"),
			Actor->FindApronAt(FVector2D(2500.0, 2500.0)), Reversed);
		TestEqual(TEXT("a point outside is not"),
			Actor->FindApronAt(FVector2D(-2500.0, 2500.0)), INDEX_NONE);
	}

	// --- The load-bearing assertion: which way the pavement faces ---------------------
	//
	// Asserted on the ENGINE-COMPUTED NORMALS, never on the 2D signed area. This project
	// has already shipped a mesh where every road faced the ground, and a signed-area check
	// passes on exactly that mesh: Unreal's winding is left-handed, so counter-clockwise in
	// plan view is the DOWN face. Only asking the engine what it computed can catch it.
	{
		FApronSurface Square;
		Square.Outline = { FVector2D(0.0, 0.0), FVector2D(4000.0, 0.0),
			FVector2D(4000.0, 4000.0), FVector2D(0.0, 4000.0) };

		FRoadMeshBuilder Builder(0.0, 512.0);
		Builder.AddApron(Square);

		const FRoadMeshBuffers& Buffers = Builder.GetBuffers();
		TestEqual(TEXT("a square triangulates to two triangles"), Buffers.Indices.Num() / 3, 2);
		TestEqual(TEXT("and welds to four vertices"), Buffers.Positions.Num(), 4);

		UE::Geometry::FDynamicMesh3 Mesh;
		for (const FVector3d& Position : Buffers.Positions)
		{
			Mesh.AppendVertex(Position);
		}
		int32 Rejected = 0;
		for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
		{
			if (Mesh.AppendTriangle(Buffers.Indices[Slot], Buffers.Indices[Slot + 1],
					Buffers.Indices[Slot + 2]) < 0)
			{
				++Rejected;
			}
		}
		TestEqual(TEXT("every apron triangle survives AppendTriangle"), Rejected, 0);

		int32 FacingUp = 0;
		for (const int32 TriangleId : Mesh.TriangleIndicesItr())
		{
			if (Mesh.GetTriNormal(TriangleId).Z > 0.9)
			{
				++FacingUp;
			}
		}
		TestEqual(TEXT("every apron triangle faces the sky"), FacingUp, Mesh.TriangleCount());
	}

	// --- Does the geometry actually REACH the component? ------------------------------
	//
	// Instrumenting the boundary rather than reasoning at it. An apron that is built
	// correctly and never seen, and an apron that is never built, look identical from the
	// outside - and the last time this project guessed between them it got seven wrong
	// diagnoses in a row. This asserts which side of the boundary the geometry is on.
	{
		Actor->ClearNetwork();
		const int32 Added = Actor->AddApron({ FVector2D(0.0, 0.0), FVector2D(6000.0, 0.0),
			FVector2D(6000.0, 4000.0), FVector2D(0.0, 4000.0) });
		if (!TestTrue(TEXT("the apron is accepted"), Added != INDEX_NONE))
		{
			return false;
		}

		Actor->RebuildMesh();

		if (!TestNotNull(TEXT("the actor has an apron component"), Actor->ApronComponent.Get()))
		{
			return false;
		}

		const UE::Geometry::FDynamicMesh3& Live =
			Actor->ApronComponent->GetDynamicMesh()->GetMeshRef();

		TestTrue(TEXT("the component holds apron triangles"), Live.TriangleCount() > 0);
		TestTrue(TEXT("and their vertices"), Live.VertexCount() > 0);

		// A UDynamicMeshComponent has NO surface-material fallback: GetNumMaterials() is
		// literally BaseMaterials.Num(), so a component nobody called SetMaterial on has
		// zero slots and the renderer has no section to draw. Correct geometry, nothing on
		// screen, and not one line of log.
		TestTrue(TEXT("and a material slot to draw them in"),
			Actor->ApronComponent->GetNumMaterials() > 0);

		TestTrue(TEXT("the component is visible"), Actor->ApronComponent->GetVisibleFlag());

		// Facing the sky at the component, not merely in the buffers.
		int32 FacingUp = 0;
		for (const int32 TriangleId : Live.TriangleIndicesItr())
		{
			if (Live.GetTriNormal(TriangleId).Z > 0.9)
			{
				++FacingUp;
			}
		}
		TestEqual(TEXT("every triangle the component holds faces the sky"),
			FacingUp, Live.TriangleCount());
	}

	// --- An apron must never be built underground -------------------------------------
	//
	// Reported as an apron that vanished the moment it was created. Everything about it was
	// right - normals up, material bound, triangles in the component - and it was three
	// units under the ground plane, because a fixed drop below the road assumed the road
	// had headroom. With SurfaceZ at 1 and a 4 uu drop it did not.
	{
		Actor->ClearNetwork();

		Actor->SurfaceZ = 1.0;
		Actor->ApronZOffset = 4.0;

		TestTrue(TEXT("a drop deeper than the road is high stays above the ground plane"),
			Actor->GetApronSurfaceZ() > 0.0);
		TestTrue(TEXT("and gives away at most half the height"),
			FMath::IsNearlyEqual(Actor->GetApronSurfaceZ(), 0.5, 1e-9));

		// With headroom the offset is honoured in full - the cap is a floor, not a policy.
		Actor->SurfaceZ = 20.0;
		TestTrue(TEXT("with headroom the full offset applies"),
			FMath::IsNearlyEqual(Actor->GetApronSurfaceZ(), 16.0, 1e-9));

		// Below the road, always: the taxiway has to win the depth test where they overlap.
		TestTrue(TEXT("and the apron is always below the road"),
			Actor->GetApronSurfaceZ() < Actor->SurfaceZ);

		// The mesh is built at that height, not at some second opinion of it.
		Actor->SurfaceZ = 1.0;
		const int32 Low = Actor->AddApron({ FVector2D(0.0, 0.0), FVector2D(3000.0, 0.0),
			FVector2D(3000.0, 2000.0), FVector2D(0.0, 2000.0) });
		if (TestTrue(TEXT("the low-surface apron is accepted"), Low != INDEX_NONE))
		{
			Actor->RebuildMesh();

			const UE::Geometry::FDynamicMesh3& Buried =
				Actor->ApronComponent->GetDynamicMesh()->GetMeshRef();
			if (TestTrue(TEXT("it reaches the component"), Buried.VertexCount() > 0))
			{
				double Lowest = TNumericLimits<double>::Max();
				for (const int32 VertexId : Buried.VertexIndicesItr())
				{
					Lowest = FMath::Min(Lowest, Buried.GetVertex(VertexId).Z);
				}
				TestTrue(TEXT("and every vertex of it is above the ground plane"), Lowest > 0.0);
			}
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
