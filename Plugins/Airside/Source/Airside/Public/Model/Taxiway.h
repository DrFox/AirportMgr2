#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Taxiway.generated.h"

/**
 * One taxiway's NAME (spec docs/superpowers/specs/2026-10-02-taxiway-naming-design.md). Stored and stable - never
 * re-derived from geometry, for the reason stand numbers are not: re-deriving reshuffles letters on every edit. Owned by
 * URoadNetwork::Taxiways; a segment names it by FRoadSegment::TaxiwayId.
 *
 * APPEND-ONLY, Id == index, never reused (a deviation from the spec's slot list - see the plan's D5): a dead entry stays
 * bAlive == false, so an int32 is a stable handle with no generation, and a card holding one cannot come to name another taxiway.
 */
USTRUCT()
struct AIRSIDE_API FTaxiway
{
	GENERATED_BODY()

	UPROPERTY() int32 Id = INDEX_NONE;

	/**
	 * "A", "AB", or the player's override ("K7"). EMPTY for a connector that has not been renamed: its display name is
	 * derived, Parent's + ConnectorNumber, so renaming A to K makes A1..A4 read K1..K4 (URoadNetwork::TaxiwayDisplayName).
	 */
	UPROPERTY() FString Name;

	/** INDEX_NONE for a lettered taxiway; the parent (always a LETTERED one) for a connector. */
	UPROPERTY() int32 ParentId = INDEX_NONE;

	/** 1.. for a connector, 0 otherwise. */
	UPROPERTY() int32 ConnectorNumber = 0;

	/** The next number this taxiway gives a connector. Only advances: a deleted A1 is never reissued while A lives. */
	UPROPERTY() int32 NextConnectorNumber = 1;

	/**
	 * The player renamed it (PR 3's card). A named-by-the-player taxiway is never re-judged while it is being drawn
	 * (URoadNetwork::RejudgeTaxiway, the plan's D3): the player's word outranks "as drawn so far".
	 */
	UPROPERTY() bool bPlayerNamed = false;

	UPROPERTY() bool bAlive = false;

	bool IsConnector() const { return ParentId != INDEX_NONE; }
};

/** The naming knobs - an actor UPROPERTY (ARoadNetworkActor::TaxiwayNaming), defaulted for a model test. */
USTRUCT()
struct AIRSIDE_API FTaxiwayNamingRules
{
	GENERATED_BODY()

	/** A new chain shorter than this, anchored at both ends, is a CONNECTOR (A1) rather than a letter. 300 m (spec). */
	UPROPERTY(EditAnywhere, Category = "Airside|Taxiway names", meta = (ClampMin = "0.0"))
	double ConnectorMaxLength = 30000.0;

	/**
	 * At a node carrying exactly two roads - a BEND - how far off straight the road may turn and still be the same
	 * taxiway. NOT RoadGeom::InLineDegrees (10), which is the window at a JUNCTION: the draw tool commits one segment
	 * per click, so every bend of a curved taxiway is a node, and 10 would mint a letter per click (plan D1). 45 keeps
	 * a 90 degree corner a name boundary.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Taxiway names", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	double BendDegrees = 45.0;

	/** A long taxiway's name is repeated this far apart along it. 500 m (spec). */
	UPROPERTY(EditAnywhere, Category = "Airside|Taxiway names", meta = (ClampMin = "1000.0"))
	double LabelRepeatDistance = 50000.0;
};

/** A run of road segments in chain order and the two nodes it ends at (First == Last for a loop). Plain. */
struct FTaxiwayChain
{
	TArray<FRoadSegmentId> Segments;
	FRoadNodeId First;
	FRoadNodeId Last;
	double Length = 0.0;
	int32 LowestIndex = MAX_int32;
};

/** One "C split off from A" - URoadNetwork::NormaliseTaxiways' report, one per split. Plain. */
struct FTaxiwayRename
{
	FString SplitOff;
	FString From;
};
