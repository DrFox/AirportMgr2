#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Model/RoadTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RouteSearch.h"
#include "Tool/RoadHeal.h"
#include "Tool/RoadSnap.h"

class URoadNetwork;
class URoadProfile;
class UEntityDefinition;

/**
 * The facade a tool edits through, seen only as the calls a tool makes.
 *
 * Pattern: Facade (ARoadNetworkActor) exposed to Strategy (the IBuildTool family) through
 * an interface, so Tool/ has no COMPILE-TIME dependency on Present/. Before this seam, six
 * Tool/*.cpp files included Present/RoadNetworkActor.h purely for FToolContext::Target's
 * concrete type - and Present/ already includes Tool/ headers (RoadEditHistory, RoadHeal,
 * RoadSnap) for the actor's own facade methods, so a tool header including Present/ back
 * would have closed a real cycle. This header is that seam: it names exactly the calls
 * FToolContext::Target makes (enumerated with
 * `grep -ho 'Context\.Target->[A-Za-z_]*' Tool/*.cpp`), nothing more, and
 * ARoadNetworkActor implements it alongside being an AActor.
 *
 * A plain abstract class rather than a UINTERFACE: the tools are plain C++ structs, not
 * UObjects, and nothing in Blueprint needs to see this seam - a UInterface would add
 * reflection generation for a consumer that does not exist.
 */
class AIRSIDE_API IRoadEditTarget
{
public:
	virtual ~IRoadEditTarget() = default;

	/**
	 * The graph this target owns, or null before one exists.
	 *
	 * One read accessor rather than one per query, because every Tool/*.cpp reader only
	 * ever reads it - GetNodes, GetSegments, GetAprons, GetEntities, GetGuidelineNode and
	 * the rest are URoadNetwork's own const interface. Const so that stays true at the type
	 * level: a tool cannot reach a mutator through this pointer even by accident, and every
	 * mutation instead goes through a named method below that the facade can make undoable.
	 */
	virtual const URoadNetwork* GetNetwork() const = 0;

	// --- Nodes and segments --------------------------------------------------------------

	virtual int32 PlaceNode(FVector2D Where) = 0;
	virtual bool ConnectNodes(int32 FromIndex, int32 ToIndex) = 0;
	virtual int32 ConnectGuidelines(int32 FromNodeIndex, int32 ToNodeIndex) = 0;
	virtual bool PlaceRunway(FVector2D From, FVector2D To, URoadProfile* RunwayProfile) = 0;

	/** MinimumRunwayLength, read-only: RunwayTool judges a drag against it but never sets it. */
	virtual double GetMinimumRunwayLength() const = 0;

	virtual bool DisconnectGuideline(int32 EdgeIndex) = 0;
	virtual int32 SplitSegment(int32 SegmentIndex, FVector2D At) = 0;
	virtual bool DeleteNode(int32 NodeIndex) = 0;
	virtual bool DeleteSegment(int32 SegmentIndex) = 0;
	virtual bool MoveNode(int32 NodeIndex, FVector2D To) = 0;
	virtual void BeginInteractiveEdit(const FString& Label) = 0;
	virtual void EndInteractiveEdit(bool bKeep) = 0;
	virtual FRoadDeletionPlan PlanNodeDeletion(int32 NodeIndex) const = 0;

	// --- Aprons ----------------------------------------------------------------------------

	virtual int32 AddApron(const TArray<FVector2D>& Outline) = 0;
	virtual bool DeleteApron(int32 ApronIndex) = 0;
	virtual int32 FindApronAt(FVector2D Where) const = 0;

	// --- Stands ------------------------------------------------------------------------

	virtual int32 PlaceStand(FVector2D Where, double Heading) = 0;
	virtual bool DeleteEntity(int32 EntityIndex) = 0;
	virtual int32 FindEntityAt(FVector2D Where, double Radius) const = 0;

	/** StandDefinition, read-only: a tool previews what would be placed, never authors it. */
	virtual const UEntityDefinition* GetStandDefinition() const = 0;

	// --- Ghost preview -------------------------------------------------------------------

	virtual void UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& Snap, bool bValid) = 0;
	virtual void HideGhost() = 0;
	virtual bool MakeLiveNodeId(int32 Index, FRoadNodeId& OutId) const = 0;

	// --- Routing and agents ------------------------------------------------------------

	virtual FRoutePlan FindRoute(FGuidelineNodeId Start, FGuidelineNodeId Goal,
		ETraversalClass Class, double Wingspan) const = 0;

	/**
	 * One struct, not four - see FAirframe. Ground, Climb and Engine used to be separate
	 * parameters, which is how issue #27 happened: a caller could pass one and default
	 * another, so the taxi and a later handover were never guaranteed to read the SAME
	 * aeroplane. Issue #30 finished the collapse begun there.
	 */
	virtual bool DispatchAgent(const FRoutePlan& Plan, const FAirframe& Airframe) = 0;

	virtual void RebuildMesh() = 0;
};
