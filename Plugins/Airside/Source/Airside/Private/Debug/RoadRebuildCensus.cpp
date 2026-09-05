#include "Debug/RoadRebuildCensus.h"

#include "AirsideLog.h"
#include "Build/RoadMeshSink.h"
#include "Build/RoadNetworkSolver.h"
#include "Components/DynamicMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadMaterialSet.h"
#include "Profiles/RoadProfile.h"

void RoadRebuildCensus::Log(const URoadNetwork& Network, const FRoadMeshBuffers& Buffers,
	const UDynamicMeshComponent& MeshComponent, const FRoadSolveResult& Solved,
	const UMaterialInterface* Surface, const URoadMaterialSet* Set)
{
	// SEGMENTS AND APRONS ARE REPORTED, not just nodes, because without them "0 triangles"
	// is ambiguous in the one way that matters: an empty network and a broken builder read
	// identically. That ambiguity cost a whole diagnosis - a level with five nodes and no
	// segments SHOULD produce no road surface, and there was no way to tell that from a
	// builder that had stopped emitting bands.
	// WHAT THE SEGMENTS THEMSELVES USED, not just what the default was. "The roads changed
	// width when I clicked a tool" is unanswerable from the default alone: a segment with its
	// own profile ignores it, and the interesting case is precisely when some segments have
	// one and some do not. Reported as a census so the next reproduction says which.
	int32 OwnProfile = 0;
	int32 Fallback = 0;
	double NarrowestUsed = TNumericLimits<double>::Max();
	double WidestUsed = 0.0;
	for (const FRoadSegment& Segment : Network.GetSegments())
	{
		if (!Segment.bAlive)
		{
			continue;
		}
		Segment.Profile != nullptr ? ++OwnProfile : ++Fallback;
		if (const URoadProfile* Used_ = Network.ProfileFor(Segment))
		{
			NarrowestUsed = FMath::Min(NarrowestUsed, Used_->GetTotalWidth());
			WidestUsed = FMath::Max(WidestUsed, Used_->GetTotalWidth());
		}
	}

	FString Slots;
	for (int32 Slot = 0; Slot < MeshComponent.GetNumMaterials(); ++Slot)
	{
		const UMaterialInterface* Applied = MeshComponent.GetMaterial(Slot);
		Slots += FString::Printf(TEXT("[%d]=%s "), Slot,
			Applied != nullptr ? *Applied->GetName() : TEXT("null"));
	}
	// The distinct material ids the BUILDER produced, against the slots the COMPONENT holds.
	// A road that changes appearance with an unchanged model has to differ in one of these
	// two, and comparing them is the only way to see which - an id with no slot behind it
	// draws as nothing or as the default, and reports neither.
	TSet<int32> DistinctIDs;
	for (const int32 Id : Buffers.MaterialIDs)
	{
		DistinctIDs.Add(Id);
	}
	FString IDList;
	for (const int32 Id : DistinctIDs)
	{
		IDList += FString::Printf(TEXT("%d "), Id);
	}

	// And which profile OBJECT each segment resolved to. Segments drawn before a save come
	// back with a null profile and fall to the network default; ones drawn since carry their
	// own. If the two ever resolve to different objects, they render differently while the
	// widths agree - which is a road changing material for no reason the model can show.
	TSet<FString> ProfileNames;
	for (const FRoadSegment& Seg : Network.GetSegments())
	{
		if (!Seg.bAlive) { continue; }
		const URoadProfile* Used_ = Network.ProfileFor(Seg);
		ProfileNames.Add(Used_ != nullptr ? Used_->GetName() : TEXT("null"));
	}
	FString ProfileList;
	for (const FString& Name : ProfileNames)
	{
		ProfileList += Name + TEXT(" ");
	}

	UE_LOG(LogRoadMesh, Log, TEXT("Surface slots: %s| material ids: %s| profiles in use: %s"),
		*Slots, *IDList, *ProfileList);

	UE_LOG(LogRoadMesh, Log,
		TEXT("Profiles: %d segments own theirs, %d fall back; widths used %.0f..%.0f uu. "
			 "SurfaceMaterial=%s MaterialSet=%s"),
		OwnProfile, Fallback,
		OwnProfile + Fallback > 0 ? NarrowestUsed : 0.0, WidestUsed,
		Surface != nullptr ? *Surface->GetName() : TEXT("none"),
		Set != nullptr ? *Set->GetName() : TEXT("none"));

	const URoadProfile* Used = Network.DefaultProfile;
	UE_LOG(LogRoadMesh, Log,
		TEXT("Rebuilt: %d nodes (%d failed), %d segments, %d aprons, %d vertices, %d triangles, "
			 "default profile '%s' %.0f uu wide"),
		Solved.SolvedNodes, Solved.FailedNodes,
		Network.GetSegments().Num(),
		Network.GetAprons().Num(),
		Buffers.Positions.Num(), Buffers.Indices.Num() / 3,
		Used != nullptr ? *Used->GetName() : TEXT("none"),
		Used != nullptr ? Used->GetTotalWidth() : 0.0);
}
