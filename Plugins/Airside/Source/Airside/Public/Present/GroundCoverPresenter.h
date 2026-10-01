#pragma once

#include "CoreMinimal.h"
#include "Build/GroundCoverMask.h"
#include "Content/GroundCoverKit.h"
#include "UObject/Object.h"
#include "GroundCoverPresenter.generated.h"

class UInstancedStaticMeshComponent;
class USceneComponent;
class UStaticMesh;

/**
 * The grass near the camera, as instanced tufts streamed cell by cell (spec
 * 2026-10-01-ground-cover-grass-design.md, 4.2-4.3). Pattern: Presenter (a Humble Object) -
 * WHERE tufts go is GroundCover::ScatterCell and WHETHER they may is FGroundCoverMask, both
 * world-free and tested there; this class only turns their answers into components.
 *
 * ONE COMPONENT PER (CELL, LAYER, TUFT MESH), each culled per instance at its layer's distance
 * on the GPU - so the camera moving inside a cell costs the CPU nothing, and a nearer layer's
 * tufts appear around the camera without a rebuild. Components are POOLED: a cell leaving
 * the live set clears its instances and hands the component back, so a pan churns no objects.
 *
 * NOTHING HERE TICKS. AAirsideGroundCoverActor calls StreamAround each frame with the view.
 */
UCLASS()
class AIRSIDE_API UGroundCoverPresenter : public UObject
{
	GENERATED_BODY()

public:
	/** The component tufts attach under; its owner owns the pooled components. */
	void Initialise(USceneComponent* InAttachTo);

	/** What to draw. Clears every live cell: a new kit may change meshes, layers or the grid. */
	void SetKit(const FGroundCoverKit& InKit);

	/**
	 * The built surfaces to keep off. REFILLS EVERY LIVE CELL AT ONCE rather than streaming the
	 * refill over frames: a road the player has just drawn must not show grass through it for
	 * even a frame. A few tens of thousands of point tests - milliseconds, once per edit.
	 */
	void SetMask(FGroundCoverMask&& InMask);

	/** At most this many density layers (graphics quality - spec 4.6). Fewer clears the rest. */
	void SetMaxLayers(int32 InMaxLayers);

	/**
	 * Bring the live cells in line with a viewer: release those no layer wants, build the
	 * nearest missing ones first, at most CellBuildsPerStream per call so a fast pan streams in
	 * over a few frames instead of hitching.
	 */
	void StreamAround(const FVector& Viewer);

	/** Release every live cell. The pool is kept. */
	void Clear();

	/** A cell fully built in one call - see StreamAround. */
	static constexpr int32 CellBuildsPerStream = 4;

	bool HasKit() const { return TuftMeshes.Num() > 0 && Layers.Num() > 0 && CellSizeUu > 0.0; }
	int32 NumLiveCells() const { return Live.Num(); }
	int32 NumInstances(int32 Layer) const;
	int32 NumComponents() const { return Components.Num(); }
	const FGroundCoverMask& GetMask() const { return Mask; }

	/** Every live tuft's world location, with its layer. */
	void ForEachInstanceLocation(TFunctionRef<void(int32 Layer, const FVector& Location)> Visit) const;

private:
	int32 ActiveLayerCount() const;
	void BuildCell(const FIntVector& Key);
	void ReleaseCell(const FIntVector& Key);

	/** A pooled component dressed for Mesh and culled at CullUu, or INDEX_NONE with no owner to make one in. */
	int32 AcquireComponent(UStaticMesh* Mesh, double CullUu);

	UPROPERTY(Transient) TObjectPtr<USceneComponent> AttachTo;

	/** The kit's meshes, held here because FGroundCoverKit's raw pointers keep nothing alive. */
	UPROPERTY(Transient) TArray<TObjectPtr<UStaticMesh>> TuftMeshes;

	/** Every component ever made, live or pooled; Live and FreeComponents index into it. */
	UPROPERTY(Transient) TArray<TObjectPtr<UInstancedStaticMeshComponent>> Components;

	TArray<int32> FreeComponents;

	/** (cell x, cell y, layer) -> the components holding that cell-layer's tufts, one per mesh used. */
	TMap<FIntVector, TArray<int32>> Live;

	TArray<GroundCover::FLayerSpec> Layers;
	double CellSizeUu = 0.0;
	FGroundCoverMask Mask;
	int32 MaxLayers = TNumericLimits<int32>::Max();

	/** For the streaming/idle log line, which fires on the change only. */
	bool bWasStreaming = false;
};
