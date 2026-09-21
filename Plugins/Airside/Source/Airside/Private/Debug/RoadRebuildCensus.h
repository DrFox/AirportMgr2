#pragma once

#include "CoreMinimal.h"

class URoadNetwork;
class UDynamicMeshComponent;
class UMaterialInterface;
class URoadMaterialSet;
struct FRoadMeshBuffers;
struct FRoadSolveResult;

/**
 * The diagnostic census URoadSurfacePresenter::Rebuild logs after every rebuild.
 *
 * Pulled out of RebuildMesh because the census - profile counts, the material slot list,
 * the distinct material ids the builder produced, the profile names in use, and the final
 * "Rebuilt:" line - was ~90 of RebuildMesh's ~160 lines and had nothing to do with driving
 * the rebuild itself; it exists purely to answer "why does the mesh look like that" without
 * a repro. Free function taking exactly what it reads (the network, the builder's buffers,
 * the live component, the solve result, and the two resolved material knobs) rather than
 * the whole actor, so it stays testable in isolation from ARoadNetworkActor and cannot reach
 * for actor state nobody handed it.
 */
namespace RoadRebuildCensus
{
	/**
	 * bQuiet (issue #178) skips the whole census - not just its three UE_LOG lines, but the
	 * FStrings and TSets built to fill them - on a Geometry (drag-frame) rebuild, which calls
	 * this at up to 60fps while a node is held. The caller (URoadSurfacePresenter::
	 * RebuildInternal) passes FSurfaceSettings::bQuiet, so this fires normally on every
	 * Topology rebuild, including the one a drag ends with.
	 */
	AIRSIDE_API void Log(const URoadNetwork& Network, const FRoadMeshBuffers& Buffers,
		const UDynamicMeshComponent& MeshComponent, const FRoadSolveResult& Solved,
		const UMaterialInterface* Surface, const URoadMaterialSet* Set, bool bQuiet);
}
