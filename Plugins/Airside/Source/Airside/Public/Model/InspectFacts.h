#pragma once

#include "CoreMinimal.h"
#include "Model/RoadAgent.h"

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
	/** The pose node has at least one guideline edge - an aircraft can be routed here. */
	bool bReachable = false;
};

namespace InspectFacts
{
	/** False for an unknown agent id; Out untouched. Network may be null (no destination names). */
	AIRSIDE_API bool DescribeAgent(const UGroundTraffic& Traffic, const URoadNetwork* Network, int32 AgentId, FAgentFacts& Out);

	/** False for a dead or out-of-range entity index. Traffic may be null (no occupant). */
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
