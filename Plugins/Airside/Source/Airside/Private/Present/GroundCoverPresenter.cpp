#include "Present/GroundCoverPresenter.h"

#include "AirsideLog.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"

void UGroundCoverPresenter::Initialise(USceneComponent* InAttachTo)
{
	AttachTo = InAttachTo;
}

void UGroundCoverPresenter::SetKit(const FGroundCoverKit& InKit)
{
	Clear();
	TuftMeshes.Reset();
	for (UStaticMesh* Mesh : InKit.Tufts)
	{
		if (Mesh != nullptr)
		{
			TuftMeshes.Add(Mesh);
		}
	}
	Layers = InKit.Layers;
	CellSizeUu = InKit.CellSizeUu;
}

void UGroundCoverPresenter::SetMask(FGroundCoverMask&& InMask)
{
	Mask = MoveTemp(InMask);
	TArray<FIntVector> Keys;
	Live.GetKeys(Keys);
	for (const FIntVector& Key : Keys)
	{
		ReleaseCell(Key);
	}
	for (const FIntVector& Key : Keys)
	{
		BuildCell(Key);
	}
}

void UGroundCoverPresenter::SetMaxLayers(int32 InMaxLayers)
{
	if (InMaxLayers == MaxLayers)
	{
		return;
	}
	MaxLayers = FMath::Max(InMaxLayers, 0);
	TArray<FIntVector> Keys;
	Live.GetKeys(Keys);
	for (const FIntVector& Key : Keys)
	{
		if (Key.Z >= ActiveLayerCount())
		{
			ReleaseCell(Key);
		}
	}
}

int32 UGroundCoverPresenter::ActiveLayerCount() const
{
	return FMath::Min(MaxLayers, Layers.Num());
}

void UGroundCoverPresenter::StreamAround(const FVector& Viewer)
{
	if (!HasKit())
	{
		return;
	}

	TSet<FIntVector> Wanted;
	TArray<FIntPoint> Cells;
	for (int32 Layer = 0; Layer < ActiveLayerCount(); ++Layer)
	{
		const GroundCover::FLayerSpec& Spec = Layers[Layer];
		if (Spec.TuftsPerSquareMetre <= 0.0 || Spec.ShowWithinUu <= 0.0)
		{
			continue;
		}
		GroundCover::CellsWithin(Viewer, Spec.ShowWithinUu, CellSizeUu, Cells);
		for (const FIntPoint& Cell : Cells)
		{
			Wanted.Add(FIntVector(Cell.X, Cell.Y, Layer));
		}
	}

	TArray<FIntVector> Gone;
	for (const TPair<FIntVector, TArray<int32>>& Pair : Live)
	{
		if (!Wanted.Contains(Pair.Key))
		{
			Gone.Add(Pair.Key);
		}
	}
	for (const FIntVector& Key : Gone)
	{
		ReleaseCell(Key);
	}

	// NEAREST FIRST: under the per-call budget, the ground at the viewer's feet fills before
	// the far edge of the 40 m ring does.
	TArray<FIntVector> Missing;
	for (const FIntVector& Key : Wanted)
	{
		if (!Live.Contains(Key))
		{
			Missing.Add(Key);
		}
	}
	Missing.Sort([this, &Viewer](const FIntVector& A, const FIntVector& B)
	{
		return GroundCover::DistanceToCell(Viewer, FIntPoint(A.X, A.Y), CellSizeUu)
			< GroundCover::DistanceToCell(Viewer, FIntPoint(B.X, B.Y), CellSizeUu);
	});
	for (int32 Index = 0; Index < FMath::Min(CellBuildsPerStream, Missing.Num()); ++Index)
	{
		BuildCell(Missing[Index]);
	}

	const bool bStreaming = Live.Num() > 0;
	if (bStreaming != bWasStreaming)
	{
		bWasStreaming = bStreaming;
		UE_LOG(LogAirside, Log, TEXT("GroundCover: %s - viewer %.1f m above the ground, %d cell(s) live"),
			bStreaming ? TEXT("streaming") : TEXT("idle"), Viewer.Z / 100.0, Live.Num());
	}
}

void UGroundCoverPresenter::Clear()
{
	TArray<FIntVector> Keys;
	Live.GetKeys(Keys);
	for (const FIntVector& Key : Keys)
	{
		ReleaseCell(Key);
	}
}

void UGroundCoverPresenter::BuildCell(const FIntVector& Key)
{
	if (!Layers.IsValidIndex(Key.Z))
	{
		return;
	}
	const GroundCover::FLayerSpec& Spec = Layers[Key.Z];
	TArray<GroundCover::FTuft> Tufts;
	GroundCover::ScatterCell(FIntPoint(Key.X, Key.Y), Key.Z, CellSizeUu, Spec.TuftsPerSquareMetre,
		TuftMeshes.Num(), Tufts);

	TArray<TArray<FTransform>> PerMesh;
	PerMesh.SetNum(TuftMeshes.Num());
	for (const GroundCover::FTuft& Tuft : Tufts)
	{
		// THE CENTRE IS TESTED, NOT THE BLADES: a tuft on a surface's edge leans over the lip,
		// and that overhang is what hides it (spec 4.4).
		if (Mask.IsCovered(Tuft.Position))
		{
			continue;
		}
		// ON THE GROUND PLANE, Z = 0 - the landscape - not SurfaceZ, which is the slabs' top.
		PerMesh[Tuft.Variant].Add(FTransform(FRotator(0.0, Tuft.YawDegrees, 0.0),
			FVector(Tuft.Position.X, Tuft.Position.Y, 0.0), FVector(Tuft.Scale)));
	}

	// ADDED EVEN WHEN EMPTY: a cell wholly under a runway is live with no components, or it
	// would be "missing" and rebuilt every frame.
	TArray<int32>& Held = Live.Add(Key);
	for (int32 MeshIndex = 0; MeshIndex < PerMesh.Num(); ++MeshIndex)
	{
		if (PerMesh[MeshIndex].Num() == 0)
		{
			continue;
		}
		const int32 Component = AcquireComponent(TuftMeshes[MeshIndex], Spec.ShowWithinUu);
		if (Component == INDEX_NONE)
		{
			continue;
		}
		Components[Component]->AddInstances(PerMesh[MeshIndex], /*bShouldReturnIndices*/ false, /*bWorldSpace*/ true);
		Held.Add(Component);
	}
}

void UGroundCoverPresenter::ReleaseCell(const FIntVector& Key)
{
	TArray<int32> Held;
	if (!Live.RemoveAndCopyValue(Key, Held))
	{
		return;
	}
	for (const int32 Component : Held)
	{
		if (UInstancedStaticMeshComponent* Instances = Components[Component].Get())
		{
			Instances->ClearInstances();
		}
		FreeComponents.Add(Component);
	}
}

int32 UGroundCoverPresenter::AcquireComponent(UStaticMesh* Mesh, double CullUu)
{
	int32 Index = INDEX_NONE;
	if (FreeComponents.Num() > 0)
	{
		Index = FreeComponents.Pop(EAllowShrinking::No);
	}
	else
	{
		AActor* Owner = AttachTo != nullptr ? AttachTo->GetOwner() : nullptr;
		if (Owner == nullptr)
		{
			return INDEX_NONE;
		}
		// UPlotPresenter::PoolFor's shape: transient, attached, registered here, never saved.
		UInstancedStaticMeshComponent* Made = NewObject<UInstancedStaticMeshComponent>(Owner, NAME_None, RF_Transient);
		Made->SetMobility(EComponentMobility::Movable);
		Made->SetupAttachment(AttachTo);
		Made->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Made->SetGenerateOverlapEvents(false);
		Made->SetCanEverAffectNavigation(false);
		// NO SHADOW, NO DISTANCE FIELD, NO LUMEN CONTRIBUTION. The spike measured none of them
		// adding anything once M_Grass lights blades with the ground's own normal, and with
		// virtual shadow maps on, shadow-casting grass is the expensive case (spec 4.5).
		Made->SetCastShadow(false);
		Made->bAffectDistanceFieldLighting = false;
		Made->bAffectDynamicIndirectLighting = false;
		Made->RegisterComponent();
		Index = Components.Add(Made);
	}
	UInstancedStaticMeshComponent* Instances = Components[Index];
	Instances->SetStaticMesh(Mesh);
	Instances->SetCullDistances(0, FMath::CeilToInt32(CullUu));
	return Index;
}

int32 UGroundCoverPresenter::NumInstances(int32 Layer) const
{
	int32 Count = 0;
	for (const TPair<FIntVector, TArray<int32>>& Pair : Live)
	{
		if (Pair.Key.Z != Layer)
		{
			continue;
		}
		for (const int32 Component : Pair.Value)
		{
			Count += Components[Component]->GetInstanceCount();
		}
	}
	return Count;
}

void UGroundCoverPresenter::ForEachInstanceLocation(TFunctionRef<void(int32 Layer, const FVector& Location)> Visit) const
{
	for (const TPair<FIntVector, TArray<int32>>& Pair : Live)
	{
		for (const int32 Component : Pair.Value)
		{
			const UInstancedStaticMeshComponent* Instances = Components[Component];
			for (int32 Instance = 0; Instance < Instances->GetInstanceCount(); ++Instance)
			{
				FTransform Transform;
				Instances->GetInstanceTransform(Instance, Transform, /*bWorldSpace*/ true);
				Visit(Pair.Key.Z, Transform.GetLocation());
			}
		}
	}
}
