#pragma once

#include "CoreMinimal.h"
#include "Build/RoadMeshSink.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadApron.h"
#include "Model/RoadHandles.h"
#include "Solve/JunctionSolver.h"

class URoadNetwork;
class URoadMaterialSet;
struct FEntityInstance;

/**
 * Accumulates junction fans and segment ribbons into one welded triangle soup.
 *
 * Vertices are welded through a map keyed on the EXACT FVector2D value. A junction
 * boundary vertex and a segment end vertex that hold the same bits therefore resolve
 * to the same vertex index, which is what makes a seam unrepresentable rather than
 * merely small. The solver guarantees those bits match by storing the cut vertices
 * rather than letting anyone recompute them.
 */
class AIRSIDE_API FRoadMeshBuilder
{
public:
	/**
	 * A null InMaterials is the supported single-material state: every triangle takes id 0
	 * and the result is byte-for-byte the mesh this builder produced before per-band
	 * materials existed.
	 */
	explicit FRoadMeshBuilder(double InZHeight, double InTexelsPerUnit = 512.0,
		const URoadMaterialSet* InMaterials = nullptr);

	/**
	 * A point on a cut line, parameterised from the right cut to the left.
	 *
	 * The ONLY way a band vertex is ever produced. The ribbon and the junction rim both
	 * call this with the same two stored cut vertices and the same alpha, so their results
	 * are bitwise identical and weld to one vertex - the same property slice 2a
	 * established for the outer pair, extended inboard. Never inline this or "simplify"
	 * one caller: two expressions that are algebraically equal are not bitwise equal.
	 */
	static FVector2D CutLinePoint(const FVector2D& RightCut, const FVector2D& LeftCut, double Alpha)
	{
		return FMath::Lerp(RightCut, LeftCut, Alpha);
	}

	/**
	 * Append a solved junction's fan, subdivided to match each arm's profile bands.
	 *
	 * ArmSegments is FRoadSolveResult::NodeArmSegments for this node - parallel to
	 * Junction.Arms. It is passed in rather than re-derived because re-walking
	 * Node.Incident re-applies a skip rule that can put the two out of step, which writes
	 * one arm's bands onto another arm's cut line.
	 */
	void AddJunction(const URoadNetwork& Network, int32 NodeIndex, const FJunctionResult& Junction,
		const TArray<FRoadSegmentId>& ArmSegments);

	/**
	 * The junction pavement when the solver could find no fan apex that sees the whole rim
	 * (FJunctionResult::Triangles empty with a rim present). Ear-clips the rim instead; no
	 * shoulder ring. See the call site in AddJunction for why this is here and not in Solve/.
	 */
	void AddJunctionByEarClipping(const URoadNetwork& Network, int32 NodeIndex, const FJunctionResult& Junction,
		const TArray<FRoadSegmentId>& ArmSegments);

	/**
	 * Append a segment's ribbon between its two stored cut lines.
	 * RibbonSegments is the number of quads along the segment; 1 is correct for a
	 * straight segment, more for a curve.
	 */
	void AddSegment(const URoadNetwork& Network, FRoadSegmentId SegmentId, int32 RibbonSegments = 8);

	/**
	 * Build a whole solved network: every live segment, then every junction.
	 *
	 * Prefer this to calling AddSegment and AddJunction by hand. The order is a contract,
	 * not a preference - a cut vertex is one welded vertex holding one UV1, WeldVertex is
	 * first-writer-wins, and a segment measures `along` from its A end while the junction
	 * at its B end would write 0. Getting it backwards makes every segment's markings jump
	 * at one end, and nothing fails when it happens.
	 *
	 * Enforcing that here rather than in a comment on each caller is the same move as the
	 * weld map itself: make the wrong result unrepresentable instead of documented. The
	 * two element functions stay public because tests need to build partial meshes.
	 */
	void Build(const URoadNetwork& Network, const FRoadSolveResult& Solved, int32 RibbonSegments = 1);

	/**
	 * Append an apron polygon.
	 *
	 * An apron never enters the junction solve - it has no arms to trim, no fillets and no
	 * cut vertices to share - so this takes the outline and nothing else. It goes through
	 * the SAME AddTriangle as everything here, deliberately: that function is the one place
	 * that knows Unreal's winding is left-handed, and "every road faced the ground" is a
	 * bug this project has already shipped once. A separate apron builder that re-derived
	 * winding is how it comes back.
	 *
	 * Aprons belong on their own builder INSTANCE, though, so their vertices cannot weld to
	 * a road's. The two surfaces meet; they are not one surface.
	 *
	 * UV1.Y is zero throughout: distance along a centreline is meaningless for a polygon.
	 * UV1.X is the paint tag (EApronPaint), Concrete for an FApronSurface.
	 */
	void AddApron(const FApronSurface& Apron);

	/**
	 * Which paint an apron triangle wears - a MEANING, written to UV1.X, never a colour.
	 * M_ApronConcrete maps it to a palette row (build_apron_material.py), so C++ says what a
	 * surface IS and the material alone says how it looks.
	 *
	 * VALUES ARE THE TAG - append only, and the material's thresholds sit at the midpoints
	 * (0.5, 1.5). Concrete is 0 so every apron built before tags existed means concrete.
	 */
	enum class EApronPaint : uint8
	{
		Concrete = 0,
		FuelSlab = 1,
		HazardBand = 2,
	};

	/**
	 * How wide a fuel depot's hazard band is, in uu (2026-09-27). 1 m: about 4 px at max zoom
	 * on the build camera, measured from samples/fuel-built.png - the width the mock-up read
	 * at. Real bund paint is narrower; this is option 1's exaggeration, not a survey figure.
	 */
	static constexpr double HazardBandUu = 100.0;

	/**
	 * The same pavement from a bare polygon, for a surface that is not an FApronSurface.
	 *
	 * A PLOTTED INSTALLATION'S PAD IS PAVEMENT and goes through here, not through a second
	 * triangulator: two evaluators of the same polygon would drift on exactly the concave
	 * shapes a freeform gesture produces, and the pad would disagree with the plot the
	 * player drew. The overload above forwards to this one.
	 *
	 * Slot TAGS EVERY TRIANGLE, the way SurfaceSlotFor's slot tags a band: NAME_None is id 0,
	 * the apron layer's own material (URoadSurfacePresenter::ApronMaterialSet puts it there);
	 * a name resolves through this builder's Materials, and one it does not declare falls back
	 * to 0 and is counted (GetUnresolvedApronSlots) rather than drawn with a guess. NO DEFAULT,
	 * for AddTriangle's own reason: a forgotten slot must not compile into tarmac.
	 */
	void AddApron(const TArray<FVector2D>& Outline, FName Slot, EApronPaint Paint = EApronPaint::Concrete);

	/**
	 * The ring between Outer and Inner - a painted band inside an outline, two triangles per
	 * edge, wound as Outer is (PolygonInset::Inset keeps winding and vertex order, which is
	 * what lets edge i pair with edge i). Slot as AddApron's: stated, resolved once.
	 *
	 * A DIFFERENT BUILDER INSTANCE from the slab it surrounds: the two share Inner's
	 * positions, and a welded vertex would carry one paint's tag into the other's triangles.
	 * Nothing if the two do not have the same corner count.
	 */
	void AddApronRing(TConstArrayView<FVector2D> Outer, TConstArrayView<FVector2D> Inner, FName Slot,
		EApronPaint Paint);

	/**
	 * Every live apron and every plotted entity's pad on Network, into this builder - the
	 * apron layer's whole content, returned as a surface count. HERE, not in the presenter's
	 * lambda it moved out of (shared-pavement Task 8), so Airside.Build.StandPadSlots measures
	 * the loop the presenter actually runs rather than a copy of it.
	 *
	 * A depot's painted slab and band (#358) are built here too, on two builders of their own
	 * (see the body for why) appended to this one's buffers - so call this LAST on a builder:
	 * appended vertices are not in the weld map, and a later AddApron would not weld to them.
	 */
	int32 AddNetworkAprons(const URoadNetwork& Network);

	/**
	 * The slot a plotted entity's pad is drawn with: the grass runway's slot for a grass pad
	 * (any non-tarmac pavement's runway slot), NAME_None for tarmac. THE SAME RULE SurfaceSlotFor
	 * gives a road, so a grass stand beside a grass taxiway is one field. Read off the entity
	 * whatever its kind: an entity left at FEntityInstance::Pavement's Tarmac default keeps
	 * the apron material.
	 * ENFORCED BY: Airside.Build.StandPadSlots
	 */
	static FName PadSlotFor(const FEntityInstance& Entity);

	/** How many AddApron slots named something Materials does not declare - see AddApron. */
	int32 GetUnresolvedApronSlots() const { return UnresolvedApronSlots; }

	/** Triangles AddNetworkAprons painted as depot slab and hazard band - the apron log line's figures. */
	int32 GetDepotSlabTriangles() const { return DepotSlabTriangles; }
	int32 GetDepotBandTriangles() const { return DepotBandTriangles; }

	void Emit(IRoadMeshSink& Sink) const;

	const FRoadMeshBuffers& GetBuffers() const { return Buffers; }
	int32 VertexCount() const { return Buffers.Positions.Num(); }

private:
	/**
	 * Returns the index of Point, appending it only if this exact value is new.
	 *
	 * FIRST WRITER WINS. When the point is already present the incoming UV1 and masks
	 * are discarded, because a welded vertex can only hold one of each. That is why
	 * callers add segments BEFORE junctions: a segment measures `along` from its A end,
	 * so its B-end cut vertices carry along = the ribbon's length, while the junction
	 * standing at that node would write along = 0. Segments must therefore write first
	 * and own the shared attributes; junctions then supply values only for the vertices
	 * they alone introduce - arc samples and the fan apex.
	 */
	int32 WeldVertex(const FVector2D& Point, const FVector2f& InUV1, const FVector2f& InUV2);

	/**
	 * DELIBERATELY NO DEFAULT ARGUMENT ON MaterialID.
	 *
	 * A default is exactly the mechanism by which a band silently becomes slot 0: the
	 * caller that forgets compiles, runs, and renders a plausible road in the wrong
	 * surface. Every caller must state which surface it is emitting, for the same reason
	 * this one function owns winding - it is the single place that can be got wrong once
	 * instead of everywhere.
	 */
	void AddTriangle(int32 A, int32 B, int32 C, int32 MaterialID);

	/**
	 * The junction's slots, taken from its WIDEST PAVED arm: any arm not on grass beats every
	 * grass arm, then greatest total width, ties broken by lowest segment id so the choice is
	 * deterministic. An all-grass junction takes its widest grass arm.
	 *
	 * A junction is one continuous annulus plus one fan, and its arms may carry different
	 * profiles, so one arm has to win. The dominant road paves the junction; a junction
	 * paved unlike every road entering it is the wrong answer, and per-arm strips would
	 * need the inset ring subdivided per arm, which it is not.
	 */
	void JunctionSlots(const URoadNetwork& Network, const TArray<FRoadSegmentId>& ArmSegments,
		int32& OutStripSlot, int32& OutFanSlot) const;

	/**
	 * The material slot a segment's whole width takes when a FACT on the segment decides it -
	 * URoadNetwork::PavementOf, for a runway or any road not on tarmac - or NAME_None for a tarmac road or taxiway, whose
	 * bands name their own. The one place the builder asks; the ribbon, the junction rim and
	 * the junction fan all go through it. Was RunwaySlotFor until grass roads existed.
	 */
	static FName SurfaceSlotFor(const URoadNetwork& Network, FRoadSegmentId Segment);

	/**
	 * Junction.Boundary[0..RimCount) with each arm's INTERIOR band points inserted along its
	 * own cut line - the rim AddJunction's fan path builds, factored out (issue #193) so
	 * AddJunctionByEarClipping can build the IDENTICAL rim rather than a plainer one.
	 *
	 * Before this existed, AddJunctionByEarClipping took Junction.Boundary as its rim
	 * verbatim - no insertion at all - so a bent junction (the one case that reaches
	 * ear-clipping instead of the fan) welded its rim ONLY at each arm's RightCut/LeftCut and
	 * silently dropped every interior band vertex the segment ribbon on the other side of that
	 * same cut line still emits (AddSegment, via the same CutLinePoint/Bands.Alphas pairing).
	 * The two sides of the seam then disagreed on how many vertices it holds: a T-vertex, not
	 * merely a coincident one, invisible while every band shares one material and open the
	 * moment it does not.
	 *
	 * SAME BOUNDS AS THE ORIGINAL LOOP, deliberately, including no wraparound check between
	 * the last rim slot and the first: whatever AddJunction's insertion already does is what
	 * this must reproduce, bitwise, not a corrected version of it - see CutLinePoint's own
	 * comment on why two algebraically-equal expressions are not this contract's business.
	 */
	TArray<FVector2D> BuildRimWithBandPoints(const URoadNetwork& Network,
		const FJunctionResult& Junction, const TArray<FRoadSegmentId>& ArmSegments,
		int32 RimCount) const;

	/**
	 * AddApron's slot resolution, shared with AddApronRing: NAME_None is id 0 without a
	 * lookup; an undeclared name is counted in UnresolvedApronSlots and falls back to 0.
	 */
	int32 ResolveApronSlot(FName Slot);

	double ZHeight;
	double TexelsPerUnit;
	const URoadMaterialSet* Materials = nullptr;
	FRoadMeshBuffers Buffers;
	TMap<FVector2D, int32> WeldMap;

	/** See GetUnresolvedApronSlots. */
	int32 UnresolvedApronSlots = 0;

	/** See GetDepotSlabTriangles. */
	int32 DepotSlabTriangles = 0;
	int32 DepotBandTriangles = 0;
};
