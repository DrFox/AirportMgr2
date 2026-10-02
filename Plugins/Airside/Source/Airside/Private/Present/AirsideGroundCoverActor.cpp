#include "Present/AirsideGroundCoverActor.h"

#include "AirsideLog.h"
#include "Build/GroundCoverMask.h"
#include "Components/SceneComponent.h"
#include "Content/GroundCoverKit.h"
#include "DynamicRHI.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Model/ExhaustiveSwitch.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideOwnedLandActor.h"
#include "Present/GroundCoverPresenter.h"
#include "Present/RoadNetworkActor.h"
#include "Present/RoadSurfacePresenter.h"
#include "Tool/RoadEditTarget.h"

namespace
{
	TAutoConsoleVariable<int32> CVarGroundCover(
		TEXT("airside.GroundCover"), 1,
		TEXT("1 draws the grass near the camera, 0 hides it - the A/B for its frame cost (spec 7)."));

	TAutoConsoleVariable<int32> CVarGroundCoverLogGpu(
		TEXT("airside.GroundCover.LogGpu"), 0,
		TEXT("1 logs the average GPU frame time and grass instance counts every 2 s (GroundCover: frame ...)."));
}

AAirsideGroundCoverActor::AAirsideGroundCoverActor()
{
	PrimaryActorTick.bCanEverTick = true;
	// AFTER THE CAMERA HAS MOVED this frame - though the view read is last frame's either way.
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
	// A PAUSED GAME STILL PANS: the build camera moves while the clock is stopped.
	SetTickableWhenPaused(true);

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Presenter = CreateDefaultSubobject<UGroundCoverPresenter>(TEXT("Presenter"));
	Presenter->Initialise(RootComponent);
}

void AAirsideGroundCoverActor::SetKit(const FGroundCoverKit& Kit)
{
	Presenter->SetKit(Kit);
	UE_LOG(LogAirside, Log, TEXT("GroundCover: kit - %d tuft mesh(es), %d layer(s), %.0f m cells%s"),
		Kit.Tufts.Num(), Kit.Layers.Num(), Kit.CellSizeUu / 100.0,
		Kit.IsUsable() ? TEXT("") : TEXT(" - NOT USABLE, no grass will be drawn"));
}

void AAirsideGroundCoverActor::BindTo(ARoadNetworkActor* Road)
{
	Unbind();
	if (Road == nullptr)
	{
		// Nothing to keep off, and nothing to stand beside: grass left from an airport that has
		// gone would grow through whatever replaces it.
		Presenter->Clear();
		return;
	}
	Bound = Road;
	BoundHandle = Road->OnNetworkChanged.AddUObject(this, &AAirsideGroundCoverActor::OnNetworkChanged);
	UE_LOG(LogAirside, Log, TEXT("GroundCover: %s growing grass around %s"), *GetName(), *Road->GetName());
	// THE CATCH-UP, for AAirsideBuildingsActor::BindTo's reason: the road's own first rebuild
	// may have fired before this bound.
	RebuildMask();
}

void AAirsideGroundCoverActor::Unbind()
{
	if (ARoadNetworkActor* Road = Bound.Get())
	{
		Road->OnNetworkChanged.Remove(BoundHandle);
	}
	Bound.Reset();
	BoundHandle.Reset();
}

void AAirsideGroundCoverActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Unbind();
	Super::EndPlay(EndPlayReason);
}

// A MISSING CASE BELOW IS A BUILD ERROR - see ExhaustiveSwitch.h: a fifth EChangeKind must say whether it moves a footprint.
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
void AAirsideGroundCoverActor::OnNetworkChanged(EChangeKind Kind, const URoadNetwork& Network)
{
	switch (Kind)
	{
	case EChangeKind::Topology:
	case EChangeKind::Facts:
		// A SURFACE OR PLOT DRAWN, REMOVED OR RESIZED: at once, so no frame shows grass through it.
		RebuildMask();
		return;
	case EChangeKind::Geometry:
		// EVERY DRAG FRAME: wait for the drag to settle - see GeometrySettleSeconds.
		bMaskDirty = true;
		DirtySinceSeconds = FPlatformTime::Seconds();
		return;
	case EChangeKind::Markings:
		// Paint lies on surfaces that are already there; no footprint moved.
		return;
	}
}
AIRSIDE_EXHAUSTIVE_SWITCH_END

void AAirsideGroundCoverActor::RebuildMask()
{
	bMaskDirty = false;
	ARoadNetworkActor* Road = Bound.Get();
	if (Road == nullptr)
	{
		return;
	}
	const double Start = FPlatformTime::Seconds();
	FGroundCoverMask Mask;
	if (const URoadSurfacePresenter* Surfaces = Road->GetPresenter())
	{
		Surfaces->ForEachSurfaceTriangle([&Mask](const FVector2D& A, const FVector2D& B, const FVector2D& C)
		{
			Mask.AddTriangle(A, B, C);
		});
	}
	// PLOTS TOO: a yard's ground is the plot's, and a depot's pad may not cover all of it.
	// KIND-NEUTRAL ON PURPOSE - a drawn stand's plot is kept bare like a depot's - which is why
	// this file is on Check-Architecture rule 17's allow-list rather than naming IsDepot().
	if (Road->Network != nullptr)
	{
		for (const FEntityInstance& Entity : Road->Network->GetEntities())
		{
			if (Entity.IsPlotted())
			{
				Mask.AddPolygon(Entity.Outline);
			}
		}
	}
	// THE OWNED LAND, read at every rebuild rather than cached: one actor lookup beside a rebuild
	// that already reads every surface triangle. No land actor owns everything.
	if (const AAirsideOwnedLandActor* Land = AAirsideOwnedLandActor::Find(GetWorld()))
	{
		Mask.SetLand(Land->GetOwnedLand());
	}
	const int32 Triangles = Mask.NumTriangles();
	const int32 Outlines = Mask.NumPolygons();
	const int32 Buckets = Mask.NumBuckets();
	Presenter->SetMask(MoveTemp(Mask));
	UE_LOG(LogAirside, Log, TEXT("GroundCover: mask rebuilt - %d triangle(s), %d outline(s), %d bucket(s), %d cell(s) refilled in %.1f ms"),
		Triangles, Outlines, Buckets, Presenter->NumLiveCells(), (FPlatformTime::Seconds() - Start) * 1000.0);
}

int32 AAirsideGroundCoverActor::LayersForQuality()
{
	// sg.FoliageQuality IS SET BY SetOverallScalabilityLevel, which the Settings panel's
	// Graphics dropdown calls (FPlayerSettings::Apply) - so no setting of our own (spec 4.6).
	const IConsoleVariable* Quality = IConsoleManager::Get().FindConsoleVariable(TEXT("sg.FoliageQuality"));
	return FMath::Clamp(Quality != nullptr ? Quality->GetInt() : 3, 0, 3);
}

void AAirsideGroundCoverActor::StreamAround(const FVector& Viewer)
{
	Presenter->StreamAround(Viewer);
}

void AAirsideGroundCoverActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (CVarGroundCover.GetValueOnGameThread() == 0)
	{
		if (!bHiddenByConsole)
		{
			bHiddenByConsole = true;
			Presenter->Clear();
			UE_LOG(LogAirside, Log, TEXT("GroundCover: hidden by airside.GroundCover 0"));
		}
		LogGpuSample();
		return;
	}
	bHiddenByConsole = false;

	Presenter->SetMaxLayers(LayersForQuality());
	if (bMaskDirty && FPlatformTime::Seconds() - DirtySinceSeconds >= GeometrySettleSeconds)
	{
		RebuildMask();
	}

	// THE VIEW THAT RENDERED THIS WORLD LAST FRAME, not the player's camera manager: the world
	// resets the list at the end of its tick (LevelTick.cpp) and the renderer refills it
	// (UnrealClient.cpp, AddStreamingViewInfo), so this is the camera in PIE, in the packaged
	// game, AND the editor viewport during Simulate - which has no player controller at all.
	const UWorld* World = GetWorld();
	if (World != nullptr && World->ViewLocationsRenderedLastFrame.Num() > 0)
	{
		bReportedNoView = false;
		StreamAround(World->ViewLocationsRenderedLastFrame[0]);
	}
	else if (!bReportedNoView)
	{
		bReportedNoView = true;
		UE_LOG(LogAirside, Log, TEXT("GroundCover: no view rendered last frame - holding the grass where it is"));
	}
	LogGpuSample();
}

void AAirsideGroundCoverActor::LogGpuSample()
{
	if (CVarGroundCoverLogGpu.GetValueOnGameThread() == 0)
	{
		GpuSamples = 0;
		GpuMsSum = 0.0;
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (GpuSamples == 0)
	{
		GpuSampleStartSeconds = Now;
	}
	GpuMsSum += FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles());
	++GpuSamples;
	if (Now - GpuSampleStartSeconds >= 2.0)
	{
		UE_LOG(LogAirside, Log, TEXT("GroundCover: frame GPU %.2f ms (mean of %d), grass %s, cells %d, instances L0/L1/L2 = %d/%d/%d"),
			GpuMsSum / GpuSamples, GpuSamples, bHiddenByConsole ? TEXT("OFF") : TEXT("ON"), Presenter->NumLiveCells(),
			Presenter->NumInstances(0), Presenter->NumInstances(1), Presenter->NumInstances(2));
		GpuSamples = 0;
		GpuMsSum = 0.0;
	}
}
