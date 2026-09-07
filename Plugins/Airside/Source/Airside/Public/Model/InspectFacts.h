#pragma once

#include "CoreMinimal.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"

class UGroundTraffic;
class URoadNetwork;

/**
 * What the inspector shows for an aircraft. PLAIN STRUCTS, not USTRUCTs: built every frame
 * for one panel, never saved, never Blueprint-bound - the Blueprint restyle binds to text
 * blocks the C++ widget fills, not to these.
 *
 * THE SEAM TO M3: the panel reads this and never FRoadAgent, so when UFlight exists it fills
 * the same struct (airline, scheduled off-block) and the panel does not change shape.
 */
struct FAgentFacts
{
	int32 Id = 0;
	/** FAirframe::TypeCode, or the traversal class when none ("Aircraft", "Vehicle"). */
	FString TypeName;
	EAgentPhase Phase = EAgentPhase::Gone;
	/** Compass degrees, 0 north, clockwise. */
	double HeadingDegrees = 0.0;
	/** uu per second; the panel formats. */
	double GroundSpeed = 0.0;
	/** uu above the surface. */
	double Altitude = 0.0;
	/** "Stand 3", "Runway 09", or "Node 41". */
	FString Destination;
	/** See InspectFacts::StatusOf for the precedence. */
	FString Status;
	bool bEngineRunning = false;
	/** Phase == Parked - the one precondition DepartAgent checks. */
	bool bCanDepart = false;

	/**
	 * What fuelling is doing for this aircraft - "truck en route", "no fuel depot" - or
	 * empty when nothing is.
	 *
	 * FILLED BY AirportOps, NOT BY DescribeAgent, which leaves it empty. Airside must never
	 * learn what a truck is FOR (see UFuelService), so the FIELD is here - because the panel
	 * reads FAgentFacts and never FRoadAgent - and the SENTENCE comes from the layer that
	 * knows. The same seam M3's UFlight fills its airline and off-block time through.
	 */
	FString Fuel;
};

struct FStandFacts
{
	int32 Index = INDEX_NONE;
	/** ICAO code letter A-F from the design wingspan. */
	FString SizeClass;
	double DesignWingspan = 0.0;
	/** The agent holding this stand's pose node in the traffic occupancy table, 0 when free.
	 *  An inbound holder has RESERVED it; a parked one OCCUPIES it (bOccupantParked). Read
	 *  from the claim, never stored on the entity: an occupancy field there would be a second
	 *  source of truth traffic would have to keep in step. M3's allocator sits above this. */
	int32 OccupantAgent = 0;
	/** The occupant is Parked (else inbound: reserved). */
	bool bOccupantParked = false;
	int32 AnchorCount = 0;
	/** The pose node has at least one guideline edge - the entity's own traffic can be
	 *  routed here. For a stand that means a taxiway; for a depot, a service road. */
	bool bReachable = false;

	/**
	 * What this entity's pose is FOR - see FEntityInstance::PoseRole. Aircraft is a stand;
	 * anything else is a service installation, which the panel titles and describes
	 * differently.
	 *
	 * The ROLE rather than a bIsDepot flag: a flag would need a second one the day a second
	 * kind of installation arrives, and the enum already exists and already says it.
	 */
	EServiceRole PoseRole = EServiceRole::Aircraft;
};

namespace InspectFacts
{
	/** False for an unknown agent id; Out untouched. Network may be null (no destination names). */
	AIRSIDE_API bool DescribeAgent(const UGroundTraffic& Traffic, const URoadNetwork* Network, int32 AgentId, FAgentFacts& Out);

	/**
	 * False for a dead or out-of-range entity index. Traffic may be null (no occupant).
	 *
	 * DESCRIBES ANY ENTITY, not only a stand, since the fuel slice: PoseRole says which, and
	 * SizeClass / DesignWingspan / OccupantAgent are meaningless for one with no design
	 * aircraft. Not renamed, because the name is reached from four call sites and a rename
	 * would buy nothing that this sentence does not.
	 */
	AIRSIDE_API bool DescribeStand(const UGroundTraffic* Traffic, const URoadNetwork& Network, int32 EntityIndex, FStandFacts& Out);

	/**
	 * One line, first match wins: No stand - waiting; Departure armed; Holding for aircraft N; Crossing runway;
	 * Shutting down (Ns); Parked; On final / Landing roll; Rolling / Climbing; Taxiing.
	 * A STRING, not an enum: presentation of several orthogonal model facts, and nothing
	 * branches on it.
	 */
	AIRSIDE_API FString StatusOf(const FRoadAgent& Agent);

	/** ICAO aerodrome reference code letter for a wingspan in uu: A <15 m, B <24, C <36, D <52, E <65, F otherwise. */
	AIRSIDE_API FString IcaoCodeForWingspan(double WingspanUu);
}
