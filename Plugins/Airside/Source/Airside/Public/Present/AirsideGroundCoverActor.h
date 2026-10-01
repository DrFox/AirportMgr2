#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AirsideGroundCoverActor.generated.h"

class ARoadNetworkActor;
class UGroundCoverPresenter;
class URoadNetwork;
struct FGroundCoverKit;
// OPAQUE, NOT INCLUDED, for AAirsideBuildingsActor's reason: one enum, a dozen Model/ headers.
enum class EChangeKind : uint8;

/**
 * The grass near the camera (spec 2026-10-01-ground-cover-grass-design.md). A composition
 * root ONLY, like AAirsideBuildingsActor, which this follows: it hears the road network
 * change, turns the drawn surfaces into a mask, and hands the view to UGroundCoverPresenter,
 * which does the drawing.
 *
 * ITS OWN ACTOR, NOT ARoadNetworkActor's: that actor grows by forwarding only (CLAUDE.md), and
 * grass is presentation that stands beside the surface rather than being part of it.
 *
 * TRANSIENT AND RUNTIME-ONLY. UGroundCoverSubsystem spawns it in game and PIE worlds; it is
 * never saved with a level, and the editor mode has no grass (spec 1, out of scope).
 *
 * LISTENS, NEVER POLLS (lint rule 51): OnNetworkChanged rebuilds the mask - Topology and Facts
 * at once, Geometry (every drag frame) once the drag has been still for GeometrySettleSeconds.
 */
UCLASS(Transient, NotPlaceable)
class AIRSIDE_API AAirsideGroundCoverActor : public AActor
{
	GENERATED_BODY()

public:
	AAirsideGroundCoverActor();

	/** Listen to Road, dropping any previous binding, and build the mask once to catch up. Null unbinds and clears. */
	void BindTo(ARoadNetworkActor* Road);

	ARoadNetworkActor* GetRoadNetwork() const { return Bound.Get(); }

	/** What to draw - UAirsideSettings::ResolveGroundCover's answer, or a test's. */
	void SetKit(const FGroundCoverKit& Kit);

	/** Tick's work for one viewer location - the seam a test drives without rendering. */
	void StreamAround(const FVector& Viewer);

	UGroundCoverPresenter* GetPresenter() const { return Presenter; }

	/**
	 * How long a run of Geometry changes must stop before the mask follows. A node drag
	 * announces Geometry every frame and a mask rebuild reads every surface triangle, so the
	 * grass under a dragged road lags this much - and never stays under a surface after.
	 */
	static constexpr double GeometrySettleSeconds = 0.25;

	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void OnNetworkChanged(EChangeKind Kind, const URoadNetwork& Network);
	void RebuildMask();
	void Unbind();

	/** The graphics dropdown's foliage group as a layer count: Low 0 ... Epic 3 (spec 4.6). */
	static int32 LayersForQuality();

	/** Every two seconds, when airside.GroundCover.LogGpu is 1: the frame's GPU time and the instance counts (spec 7). */
	void LogGpuSample();

	UPROPERTY(Transient) TObjectPtr<UGroundCoverPresenter> Presenter;

	TWeakObjectPtr<ARoadNetworkActor> Bound;
	FDelegateHandle BoundHandle;

	bool bMaskDirty = false;
	double DirtySinceSeconds = 0.0;
	bool bReportedNoView = false;
	bool bHiddenByConsole = false;

	double GpuSampleStartSeconds = 0.0;
	double GpuMsSum = 0.0;
	int32 GpuSamples = 0;
};
