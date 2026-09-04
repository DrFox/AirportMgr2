#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Build/RoadProfileBands.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBandWeldTest,
	"Airside.Build.BandWeld",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBandWeldTest::RunTest(const FString& Parameters)
{
	constexpr double Width = 800.0;
	constexpr double Shoulder = 120.0;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(Width, 200.0, Shoulder);

	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId East   = Net->AddNode(FVector2D(12000.0, 0.0));
	const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 12000.0));
	const FRoadSegmentId ToEast  = Net->AddStraightSegment(Centre, East,  Profile);
	Net->AddStraightSegment(Centre, North, Profile);

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	TestEqual(TEXT("every node solved"), Solved.FailedNodes, 0);

	FRoadMeshBuilder Builder(10.0);
	Builder.Build(*Net, Solved, 3);
	const FRoadMeshBuffers& Buffers = Builder.GetBuffers();

	// THE CONTRACT, extended to bands. A band vertex is not stored anywhere: the ribbon
	// and the junction rim each derive it from the same two cut vertices through
	// CutLinePoint. If they ever stop agreeing bitwise, the shoulder tears open along
	// every cut line - the same seam this project exists to make unrepresentable, one
	// step inboard of where slice 2a proved it closed.
	{
		const FRoadSegment* Seg = Net->GetSegment(ToEast);
		if (!TestNotNull(TEXT("east segment resolves"), Seg))
		{
			return false;
		}

		const FRoadProfileBands Bands = FRoadProfileBands::FromProfile(Profile);
		TestEqual(TEXT("shouldered profile gives four boundaries"), Bands.Alphas.Num(), 4);

		for (int32 Boundary = 0; Boundary < Bands.Alphas.Num(); ++Boundary)
		{
			const FVector2D Expected = FRoadMeshBuilder::CutLinePoint(
				Seg->RightCutA, Seg->LeftCutA, Bands.Alphas[Boundary]);

			int32 Matches = 0;
			for (const FVector3d& P : Buffers.Positions)
			{
				if (P.X == Expected.X && P.Y == Expected.Y)
				{
					++Matches;
				}
			}

			// Exactly one: the ribbon and the rim both produced it and it welded.
			TestEqual(
				FString::Printf(TEXT("band boundary %d is present exactly once"), Boundary),
				Matches, 1);
		}
	}

	// The surface is opaque everywhere. UV2.Y once carried a ground blend that faded the
	// shoulder into the terrain; edge treatment is a per-band material choice now, so
	// nothing may be transparent and this asserts the channel stays neutral.
	{
		for (const FVector2f& Masks : Buffers.UV2)
		{
			if (Masks.Y < 1.0f)
			{
				AddError(TEXT("a vertex carries a ground blend below 1 - the fade is back"));
				break;
			}
		}
	}

	// Facing is unchanged by subdivision. Unreal's front face is the opposite winding to
	// the maths convention, so front-facing means NEGATIVE 2D signed area.
	{
		int32 Backfacing = 0;
		for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
		{
			const FVector3d& A = Buffers.Positions[Buffers.Indices[Slot]];
			const FVector3d& B = Buffers.Positions[Buffers.Indices[Slot + 1]];
			const FVector3d& C = Buffers.Positions[Buffers.Indices[Slot + 2]];
			if (0.5 * ((B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X)) >= 0.0)
			{
				++Backfacing;
			}
		}
		TestEqual(TEXT("no backfacing triangle after subdivision"), Backfacing, 0);
	}

	// The junction's inset ring. Built for a shoulder fade that no longer exists, kept
	// because it is exactly the boundary between a junction's outer shoulder band and its
	// interior - the geometry per-band materials need. Asserted so it is not quietly lost
	// in the meantime, when nothing renders differently either way.
	//
	// Written to be FALSE without the ring. The obvious phrasing - "count vertices near the
	// apex" - is satisfied by the segments' own band vertices, which sit well inside this
	// radius, so it would pass whether or not a ring was ever built and prove nothing.
	{
		const FJunctionResult* CentreResult = Solved.NodeResults.Find(Centre.Index);
		if (!TestNotNull(TEXT("centre node solved"), CentreResult))
		{
			return false;
		}

		const int32 ApexSlot = CentreResult->Boundary.Num() - 1;
		const FVector2D Apex = CentreResult->Boundary[ApexSlot];

		// Before the ring exists the apex is the ONLY mesh vertex inboard of the rim, so a
		// vertex nearer the apex than every rim vertex, and not the apex, cannot exist. A
		// fold - the ring overshooting the apex - would show up as a backfacing triangle,
		// which the check above already forbids across the whole buffer.
		{
			double NearestRim = TNumericLimits<double>::Max();
			for (int32 Slot = 0; Slot < ApexSlot; ++Slot)
			{
				NearestRim = FMath::Min(
					NearestRim, FVector2D::Distance(CentreResult->Boundary[Slot], Apex));
			}

			int32 Inboard = 0;
			for (int32 Index = 0; Index < Buffers.Positions.Num(); ++Index)
			{
				const FVector2D Flat(Buffers.Positions[Index].X, Buffers.Positions[Index].Y);
				const double ToApex = FVector2D::Distance(Flat, Apex);

				// Strictly between the apex and the innermost rim vertex.
				if (ToApex > 0.0 && ToApex < NearestRim)
				{
					++Inboard;
				}
			}

			TestTrue(TEXT("the junction has a ring of vertices inboard of its whole rim"),
				Inboard > 0);
		}
	}

	// A dead end must reach its NODE, carrying its full cross-section, not stop at its
	// trimmed cut.
	//
	// The cap closing that gap used to be built from the two outer rails alone. The shoulder
	// fade exposed it - every corner of such a cap was transparent, so the road visibly
	// stopped a half-width short of where it was drawn and only reached its node once a
	// second click turned it into a junction whose fan filled the hole. The fade is gone;
	// the cap defect it exposed is not, it is merely invisible again until per-band
	// materials would paint the whole cap in the outermost band's surface.
	//
	// So this asserts on the cap's LATERAL SUBDIVISION rather than on a blend. The blend is
	// uniformly 1 now and an assertion on it would hold either way.
	{
		const FRoadSegment* EastSeg = Net->GetSegment(ToEast);
		const FRoadNode* EastNode = Net->GetNode(East);
		if (TestNotNull(TEXT("east dead-end node resolves"), EastNode) &&
			TestNotNull(TEXT("east segment resolves for the cap check"), EastSeg))
		{
			// The segment runs +X from the centre, so its B-end cut line is the LAST thing
			// the ribbon reaches. Anything beyond it in X belongs to the cap and nothing
			// else - which is what makes this discriminating. Counting vertices "near the
			// node" instead would sweep in the ribbon's own rails, which are subdivided
			// regardless, and the assertion would hold whether or not a cap was built.
			const double CutX = FMath::Max(EastSeg->LeftCutB.X, EastSeg->RightCutB.X);
			const float HalfWidth = static_cast<float>(Width * 0.5);

			int32 BeyondCut = 0;
			int32 InteriorBandsBeyondCut = 0;
			for (int32 Index = 0; Index < Buffers.Positions.Num(); ++Index)
			{
				if (Buffers.Positions[Index].X > CutX)
				{
					++BeyondCut;

					// UV1.X is the lateral offset. An unsubdivided cap has only its two
					// outer corners, both at exactly +/- the half width; an interior band
					// boundary is the one thing that cannot be there without subdivision.
					if (FMath::Abs(Buffers.UV1[Index].X) < HalfWidth)
					{
						++InteriorBandsBeyondCut;
					}
				}
			}

			TestTrue(TEXT("the dead end has surface beyond its trimmed cut"), BeyondCut > 0);
			TestTrue(TEXT("the road reaches its own node"), EastNode->Position.X > CutX);

			// The load-bearing one: the cap carries the ribbon's interior bands, not just
			// its two outer corners.
			TestTrue(TEXT("the dead end's cap is subdivided by band"),
				InteriorBandsBeyondCut > 0);
		}
	}

	// K3: a pass-through node that is very nearly, but not exactly, collinear emits slivers -
	// measured here at 2.6e-07 uu², the same defect slice 2a recorded at 1.6e-10. They pass
	// every winding check and are geometrically harmless, but they reach the renderer and
	// the normal computation. Slice 2a could not calibrate a threshold; a millionth of a
	// square unit is a square 0.001 uu on a side, far below anything a pixel can cover at
	// any sane texel density, and still above the slivers.
	auto CountSlivers = [](const FRoadMeshBuffers& In)
	{
		int32 Slivers = 0;
		for (int32 Slot = 0; Slot + 2 < In.Indices.Num(); Slot += 3)
		{
			const FVector3d& A = In.Positions[In.Indices[Slot]];
			const FVector3d& B = In.Positions[In.Indices[Slot + 1]];
			const FVector3d& C = In.Positions[In.Indices[Slot + 2]];
			const double Area = FMath::Abs(
				0.5 * ((B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X)));
			if (Area < 1e-6)
			{
				++Slivers;
			}
		}
		return Slivers;
	};

	TestEqual(TEXT("no zero-area slivers reach the bend's buffers"), CountSlivers(Buffers), 0);

	// The bend above has no near-collinear node, so it produces no slivers whether or not
	// they are dropped - it cannot fail, and a test that cannot fail is worth less than no
	// test. A pass-through node a HAIR off collinear is the case that emits them, and the
	// offset has to be chosen rather than guessed. Measured across a sweep:
	//
	//   offset 0        rim 3  triangles 0   - exactly collinear: no apex sees the rim, so
	//                                          the solver vetoes the fan and emits nothing
	//   offset 1e-6     rim 3  triangles 9   - 6 slivers, smallest 2.6e-07 uu²  <- K3
	//   offset 1e-3     rim 4  triangles 0   - vetoed again
	//   offset 1        rim 28 triangles 84  - healthy, smallest 0.14 uu²
	//
	// So "collinear" is the wrong word for the failing case and a straight line is the
	// wrong test: it produces no junction geometry at all. Only the narrow band where the
	// fan is still emitted but its corner has collapsed produces slivers.
	{
		constexpr double HairOffCollinear = 1e-6;

		URoadNetwork* Straight = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId West   = Straight->AddNode(FVector2D(-10000.0, 0.0));
		const FRoadNodeId Middle = Straight->AddNode(FVector2D(0.0, HairOffCollinear));
		const FRoadNodeId Far    = Straight->AddNode(FVector2D(10000.0, 0.0));
		Straight->AddStraightSegment(West, Middle, Profile);
		Straight->AddStraightSegment(Middle, Far, Profile);

		const FRoadSolveResult StraightSolved = FRoadNetworkSolver::SolveAll(*Straight);
		TestEqual(TEXT("the near-collinear network solved"), StraightSolved.FailedNodes, 0);

		FRoadMeshBuilder StraightBuilder(10.0);
		StraightBuilder.Build(*Straight, StraightSolved, 3);

		// The construction must still be producing a fan, or this asserts nothing.
		const FJunctionResult* MiddleResult = StraightSolved.NodeResults.Find(Middle.Index);
		if (TestNotNull(TEXT("the near-collinear node solved"), MiddleResult))
		{
			TestTrue(TEXT("the near-collinear node still emits a fan"),
				MiddleResult->Triangles.Num() > 0);
		}

		TestEqual(TEXT("no zero-area slivers reach a near-collinear node's buffers"),
			CountSlivers(StraightBuilder.GetBuffers()), 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
