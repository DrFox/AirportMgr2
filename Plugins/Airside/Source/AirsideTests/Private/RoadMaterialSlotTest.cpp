#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Build/RoadProfileBands.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadMaterialSet.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** The slice's slot vocabulary, in the order DA_RoadMaterials declares it. */
	URoadMaterialSet* StandardSet()
	{
		return URoadMaterialSet::MakeTransient(
			{ TEXT("Asphalt"), TEXT("Concrete"), TEXT("Kerb") });
	}

	/**
	 * A profile with three DIFFERENT bands, deliberately asymmetric.
	 *
	 * Bands are declared left to right but boundaries are walked right to left, so a
	 * symmetric profile cannot tell a correct mapping from a reversed one. Kerb | Concrete
	 * | Asphalt reads back as 0, 1, 2 only if the walk order is right.
	 */
	URoadProfile* AsymmetricProfile()
	{
		URoadProfile* Profile = NewObject<URoadProfile>(GetTransientPackage());

		auto AddBand = [Profile](double Width, ERoadBandType Type, const TCHAR* Slot)
		{
			FProfileBand Band;
			Band.Width = Width;
			Band.Type = Type;
			Band.MaterialSlot = Slot;
			Profile->Bands.Add(Band);
		};

		AddBand(200.0,  ERoadBandType::Curb,     TEXT("Kerb"));
		AddBand(1600.0, ERoadBandType::Lane,     TEXT("Concrete"));
		AddBand(500.0,  ERoadBandType::Shoulder, TEXT("Asphalt"));

		Profile->CentrelineOffset = -1.0;
		Profile->PreferredFilletRadius = 1500.0;
		return Profile;
	}

	/** Every band on one slot, same widths - the control for the weld measurement. */
	URoadProfile* UniformProfile()
	{
		URoadProfile* Profile = AsymmetricProfile();
		for (FProfileBand& Band : Profile->Bands)
		{
			Band.MaterialSlot = TEXT("Asphalt");
		}
		return Profile;
	}

	/** A bend, so the mesh contains a real junction fan as well as ribbons. */
	URoadNetwork* BendNetwork(URoadProfile* Profile)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId East   = Net->AddNode(FVector2D(40000.0, 0.0));
		const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 40000.0));
		Net->AddStraightSegment(Centre, East,  Profile);
		Net->AddStraightSegment(Centre, North, Profile);
		return Net;
	}

	/** Solve and build a whole network through the production ordering. */
	void BuildAll(FRoadMeshBuilder& Builder, URoadNetwork& Net)
	{
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(Net);
		Builder.Build(Net, Solved, 1);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadMaterialSlotTest,
	"Airside.Build.MaterialSlots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadMaterialSlotTest::RunTest(const FString& Parameters)
{
	// 1. A band's slot NAME becomes the material set's INDEX, in the band walk's own order.
	//    The asymmetric profile is what makes this assertion able to fail: a reversed walk
	//    would read back 2, 1, 0 and every other property here would still hold.
	{
		const FRoadProfileBands Bands =
			FRoadProfileBands::FromProfile(AsymmetricProfile(), StandardSet());

		// Guarded, not indexed blind: a wrong count must FAIL this test, not take the
		// whole suite down with it. A crashed test used to be indistinguishable from a
		// suite that had legitimately shrunk.
		if (TestEqual(TEXT("one slot per band, not per boundary"),
			Bands.SlotIndices.Num(), Bands.Alphas.Num() - 1) && Bands.SlotIndices.Num() == 3)
		{
			// Laterals[0] is the RIGHT edge, so SlotIndices[0] is the rightmost band - the
			// Asphalt shoulder declared last.
			TestEqual(TEXT("rightmost band takes the slot its profile named"),
				Bands.SlotIndices[0], 0);
			TestEqual(TEXT("middle band takes Concrete"), Bands.SlotIndices[1], 1);
			TestEqual(TEXT("leftmost band takes Kerb"), Bands.SlotIndices[2], 2);
		}

		TestEqual(TEXT("every name resolved, so nothing fell back"), Bands.UnresolvedSlots, 0);
	}

	// 2. A name the set does not declare falls back to 0 and SAYS SO. The count exists so
	//    this can be asserted without parsing a log; an unreported fallback is how a road
	//    quietly renders in the wrong surface.
	{
		URoadProfile* Profile = AsymmetricProfile();
		Profile->Bands[1].MaterialSlot = TEXT("Grass");

		const FRoadProfileBands Bands = FRoadProfileBands::FromProfile(Profile, StandardSet());

		if (Bands.SlotIndices.IsValidIndex(1))
		{
			TestEqual(TEXT("an unresolved name falls back to slot 0"), Bands.SlotIndices[1], 0);
		}
		else
		{
			AddError(TEXT("no slot was resolved for the middle band"));
		}
		TestEqual(TEXT("and is counted"), Bands.UnresolvedSlots, 1);
	}

	// 3. A null material set is a supported state, not an error: every band is slot 0,
	//    which is the single-material mesh this builder produced before this slice.
	{
		const FRoadProfileBands Bands = FRoadProfileBands::FromProfile(AsymmetricProfile(), nullptr);

		TestEqual(TEXT("no set still gives one slot per band"),
			Bands.SlotIndices.Num(), Bands.Alphas.Num() - 1);
		for (int32 Band = 0; Band < Bands.SlotIndices.Num(); ++Band)
		{
			TestEqual(TEXT("every band is slot 0 without a set"), Bands.SlotIndices[Band], 0);
		}
		TestEqual(TEXT("absent set is not an unresolved name"), Bands.UnresolvedSlots, 0);
	}

	// 4. The mesh carries one id per TRIANGLE, and every one addresses a real slot.
	//    FDynamicMeshSceneProxy discards a triangle whose id is >= NumMaterials into no
	//    render buffer at all, silently, so out of range here is an invisible hole later.
	{
		URoadMaterialSet* Set = StandardSet();
		URoadNetwork* Net = BendNetwork(AsymmetricProfile());

		FRoadMeshBuilder Builder(10.0, 512.0, Set);
		BuildAll(Builder, *Net);
		const FRoadMeshBuffers& Buffers = Builder.GetBuffers();

		TestTrue(TEXT("the network built some geometry"), Buffers.Indices.Num() > 0);
		TestEqual(TEXT("exactly one material id per triangle"),
			Buffers.MaterialIDs.Num() * 3, Buffers.Indices.Num());

		int32 OutOfRange = 0;
		for (const int32 Id : Buffers.MaterialIDs)
		{
			if (Id < 0 || Id >= Set->Slots.Num())
			{
				++OutOfRange;
			}
		}
		TestEqual(TEXT("no id can address a slot the set does not have"), OutOfRange, 0);
	}

	// 5. THE WELD MEASUREMENT. Per-triangle ids must not have tempted anything to split a
	//    vertex in order to carry a material, so a profile whose bands differ produces the
	//    same vertex count as one whose bands agree.
	//
	//    This measures the contract rather than naming it: an assertion that merely counted
	//    ids, or checked for duplicate positions, would pass on a mesh that had split every
	//    band boundary - which is exactly the failure slice 2a's plan shipped twice.
	{
		URoadMaterialSet* Set = StandardSet();

		FRoadMeshBuilder Varied(10.0, 512.0, Set);
		URoadNetwork* VariedNet = BendNetwork(AsymmetricProfile());
		BuildAll(Varied, *VariedNet);

		FRoadMeshBuilder Uniform(10.0, 512.0, Set);
		URoadNetwork* UniformNet = BendNetwork(UniformProfile());
		BuildAll(Uniform, *UniformNet);

		TestEqual(TEXT("differing band materials add no vertices, because the id is on the face"),
			Varied.VertexCount(), Uniform.VertexCount());
		TestEqual(TEXT("and change no triangle count either"),
			Varied.GetBuffers().Indices.Num(), Uniform.GetBuffers().Indices.Num());

		// The ids themselves must actually differ, or the assertion above is vacuous - two
		// identical meshes would satisfy it.
		TSet<int32> Distinct;
		for (const int32 Id : Varied.GetBuffers().MaterialIDs)
		{
			Distinct.Add(Id);
		}
		TestTrue(TEXT("the varied mesh really does use more than one slot"), Distinct.Num() > 1);
	}

	// 6. A junction is skinned by its WIDEST arm: one continuous annulus and one fan cannot
	//    be per arm, so the dominant road paves the junction.
	{
		URoadMaterialSet* Set = StandardSet();

		URoadProfile* Wide = AsymmetricProfile();                 // 2300 uu, lane Concrete
		URoadProfile* Narrow = URoadProfile::MakeTransient(400.0, 1500.0, 60.0);
		for (FProfileBand& Band : Narrow->Bands)
		{
			Band.MaterialSlot = TEXT("Kerb");
		}

		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId East   = Net->AddNode(FVector2D(40000.0, 0.0));
		const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 40000.0));
		Net->AddStraightSegment(Centre, East,  Wide);
		Net->AddStraightSegment(Centre, North, Narrow);

		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
		const TArray<FRoadSegmentId>* Arms = Solved.NodeArmSegments.Find(Centre.Index);
		const FJunctionResult* Junction = Solved.NodeResults.Find(Centre.Index);

		if (Arms != nullptr && Junction != nullptr && Junction->Triangles.Num() > 0)
		{
			// Junction alone, so nothing but the fan and its strip is in the buffers.
			FRoadMeshBuilder Builder(10.0, 512.0, Set);
			Builder.AddJunction(*Net, Centre.Index, *Junction, *Arms);

			TSet<int32> Used;
			for (const int32 Id : Builder.GetBuffers().MaterialIDs)
			{
				Used.Add(Id);
			}

			// The wide arm is Kerb | Concrete | Asphalt: outer Asphalt (0) on the strip,
			// centreline Concrete (1) on the fan. The narrow arm would give Kerb (2) for
			// both, so its absence is what proves the widest arm won.
			TestTrue(TEXT("the junction strip takes the widest arm's outer band"),
				Used.Contains(0));
			TestTrue(TEXT("the junction fan takes the widest arm's centreline band"),
				Used.Contains(1));
			TestFalse(TEXT("the narrow arm does not skin the junction"), Used.Contains(2));
		}
		else
		{
			AddError(TEXT("the two-arm bend produced no junction fan to measure"));
		}
	}

	// 7. The material array handed to ConfigureMaterialSet is never short and never sparse.
	//    The engine matches purely by index and has no slot names, so a short array is what
	//    turns a valid id into a discarded triangle.
	{
		URoadMaterialSet* Set = StandardSet();
		TArray<UMaterialInterface*> Resolved;
		Set->ResolveMaterials(Resolved);

		TestEqual(TEXT("one material per declared slot"), Resolved.Num(), Set->Slots.Num());

		int32 Nulls = 0;
		for (UMaterialInterface* Material : Resolved)
		{
			if (Material == nullptr)
			{
				++Nulls;
			}
		}
		TestEqual(TEXT("a slot with no material still gets one, so no slot is empty"), Nulls, 0);
	}

	// 8. A name the set does not declare is INDEX_NONE at the set, not 0. The fallback is
	//    the band table's decision to make and to report - burying it here would leave
	//    nothing able to tell a genuine Asphalt band from a misspelled one.
	{
		URoadMaterialSet* Set = StandardSet();

		TestEqual(TEXT("a declared name gives its index"), Set->IndexOf(TEXT("Concrete")), 1);
		TestEqual(TEXT("an undeclared name is INDEX_NONE, never 0"),
			Set->IndexOf(TEXT("Grass")), (int32)INDEX_NONE);
	}

	// 9. The mesh a component receives carries the ids the buffers described - and keeps
	//    them lined up ACROSS A REJECTED TRIANGLE.
	//
	//    FDynamicMesh3::AppendTriangle refuses duplicates, so mesh triangle ids are not the
	//    buffer's triangle indices. A build that assumed they were would skin every triangle
	//    after the refusal with its neighbour's material and report nothing. The duplicate
	//    here is deliberate: it is the smallest thing that makes that assumption fail.
	{
		FRoadMeshBuffers Buffers;
		Buffers.Positions = {
			FVector3d(0.0, 0.0, 0.0), FVector3d(100.0, 0.0, 0.0),
			FVector3d(100.0, 100.0, 0.0), FVector3d(0.0, 100.0, 0.0) };
		for (int32 Vertex = 0; Vertex < Buffers.Positions.Num(); ++Vertex)
		{
			Buffers.UV0.Add(FVector2f::ZeroVector);
			Buffers.UV1.Add(FVector2f::ZeroVector);
			Buffers.UV2.Add(FVector2f(0.0f, 1.0f));
		}

		// Triangle 1 repeats triangle 0, so the mesh takes 0 and 2 and refuses 1.
		Buffers.Indices = { 0, 1, 2,   0, 1, 2,   0, 2, 3 };
		Buffers.MaterialIDs = { 0, 1, 2 };

		UE::Geometry::FDynamicMesh3 Mesh;
		const int32 Rejected = FDynamicMeshSink::BuildMesh(Mesh, Buffers);

		TestEqual(TEXT("the duplicate triangle is refused"), Rejected, 1);
		TestEqual(TEXT("two triangles survive"), Mesh.TriangleCount(), 2);
		const bool bHasIds = Mesh.HasAttributes() && Mesh.Attributes()->HasMaterialID();
		TestTrue(TEXT("the mesh carries material ids"), bHasIds);

		if (bHasIds)
		{
			// The surviving triangles are the buffer's 0 and 2, so their ids must be 0 and
			// 2 - never 0 and 1, which is what indexing by mesh triangle id would give.
			TArray<int32> Ids;
			for (const int32 Tid : Mesh.TriangleIndicesItr())
			{
				int32 Id = INDEX_NONE;
				Mesh.Attributes()->GetMaterialID()->GetValue(Tid, &Id);
				Ids.Add(Id);
			}
			Ids.Sort();

			TestEqual(TEXT("two ids, one per surviving triangle"), Ids.Num(), 2);
			if (Ids.Num() == 2)
			{
				TestEqual(TEXT("the first surviving triangle keeps its own id"), Ids[0], 0);
				TestEqual(TEXT("and the third keeps ITS id, not the refused one's"), Ids[1], 2);
			}
		}
	}

	return true;
}

#endif
