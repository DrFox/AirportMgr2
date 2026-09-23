#pragma once

// ENTITY PLACEMENT RECORDS ONLY, since #176. This header used to be three headers in one -
// these records, the airframe physics bundle (Model/Airframe.h since the split) and
// FAgentMotion, "everything the VIEW needs" (Model/AgentMotion.h) - so touching a taxi turn
// rate or a wheel radius recompiled every one of the ~40 files that only wanted to know what
// EServiceRole an anchor was.
//
// BOTH OF THE OTHER TWO ARE STILL INCLUDED HERE, so every existing includer of this one
// header keeps compiling unchanged. Only the handful that need just one of the three -
// RouteFollower, SpeedProfile, ArrivalPlanner, the *Run.h files, the view and anim classes -
// now name the narrow header directly; see those headers for the split.

#include "CoreMinimal.h"
#include "Model/AgentMotion.h"
#include "Model/Airframe.h"
#include "Model/RoadHandles.h"
#include "Model/RoadTraffic.h"
#include "RoadEntity.generated.h"

class UEntityDefinition;

/**
 * What a thing DOES at an anchor - not how it moves.
 *
 * Deliberately separate from ETraversalClass. A fuel truck and a baggage cart obey
 * identical movement rules and are both GroundVehicle to the network; they differ only in
 * the job they come to do, which is this. Keeping the two apart is what lets this list
 * grow with every vehicle type in the game without pathfinding ever consulting it.
 */
UENUM(BlueprintType)
enum class EServiceRole : uint8
{
	Aircraft,
	Fuel,
	Baggage,
	Tug,
	GPU,
	Passenger,
	Crew
};

/**
 * Plan-view extent of the thing a definition describes, in its own local space.
 *
 * Here rather than in the overlay because it is a fact about an A320, not a drawing
 * choice: an overlay that carried its own dimensions would be a second opinion about how
 * big the aircraft is, and the two could disagree without anything reporting it. It is
 * also what a stand's painted boundary is derived from.
 *
 * All distances are local X or spans in uu, on the same axes as the anchors: +X forward,
 * +Y starboard, origin at whatever the definition measures from.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FEntityFootprint
{
	GENERATED_BODY()

	/** Nose, forward of the origin. */
	UPROPERTY(EditAnywhere) double NoseX = 0.0;

	/** Tail, normally behind the origin and so negative. */
	UPROPERTY(EditAnywhere) double TailX = 0.0;

	/** Wingtip to wingtip. */
	UPROPERTY(EditAnywhere) double Wingspan = 0.0;

	/**
	 * Side to side, uu, so the fuselage is a BOX and not an axis. Zero keeps the old line.
	 *
	 * WHY IT DID NOT MATTER UNTIL NOW. Every route that had an opinion about an aeroplane ran
	 * OUTSIDE it - the service ring was outboard of the wingtips - so "does this line cross the
	 * centreline" was the whole of the question and a zero-width segment answered it. The stand
	 * lane now runs INSIDE the wingtip, alongside the fuselage, where a zero-width line permits
	 * a route straight down the aircraft's skin.
	 *
	 * WINGS AND TAILPLANE STAY PASSABLE, and that is unchanged rather than overlooked: driving
	 * under a wing is normal, and HydrantPit is under the starboard wing root because that is
	 * where a hydrant pit is.
	 *
	 * A BUILD-TIME ASSERTION, NOT A RUNTIME KEEP-OUT, and that is the whole of what it is
	 * today. Nothing in RouteSearch, NodeReach or the follower reads this field: no route is
	 * refused for entering the box and no agent steers round it. Its ONE consumer is
	 * Airside.Entities.StandLaneClearsTheAircraft (StandLaneTest.cpp), which inflates it to a
	 * rectangle and asserts no point of the laid lane falls inside - so what it actually buys
	 * is that an AUTHORED lane cannot be laid down the aircraft's skin. Written down because a
	 * field that reads as a rule invites a caller to rely on one that is not there; a keep-out
	 * at route time would be a second evaluator of the same geometry, and wants a decision
	 * rather than an assumption.
	 *
	 * ZERO IS THE DEFAULT so a definition authored before this field keeps its old meaning
	 * rather than silently gaining a keep-out it was never laid out around.
	 */
	UPROPERTY(EditAnywhere) double FuselageWidth = 0.0;

	/** Where the wing crosses the centreline. */
	UPROPERTY(EditAnywhere) double WingX = 0.0;

	UPROPERTY(EditAnywhere) double TailplaneSpan = 0.0;

	UPROPERTY(EditAnywhere) double TailplaneX = 0.0;

	/** False when nothing has been authored, so callers can skip drawing rather than draw a dot. */
	bool IsSet() const { return Wingspan > 0.0 && NoseX > TailX; }
};

/** A connection point between an entity and the guideline graph, in the entity's local space. */
USTRUCT(BlueprintType)
struct AIRSIDE_API FEntityAnchor
{
	GENERATED_BODY()

	/**
	 * Stable identity, and the ONLY way an anchor should be addressed.
	 *
	 * Not the array index, which shifts the moment a definition gains an anchor and leaves
	 * every instance placed before it addressing the wrong thing. Not the role either: a
	 * stand has two belt loaders, so Baggage names a category, not a position.
	 *
	 * An instance that predates a new anchor simply does not carry this id, and a lookup
	 * for it misses - which is a correct answer, where an index read the same information
	 * out of bounds.
	 */
	UPROPERTY(EditAnywhere) FName Id;

	UPROPERTY(EditAnywhere) FVector2D LocalPosition = FVector2D::ZeroVector;

	/** Radians, relative to the entity's own heading. */
	UPROPERTY(EditAnywhere) double LocalHeading = 0.0;

	UPROPERTY(EditAnywhere) EServiceRole Role = EServiceRole::Aircraft;
};

/**
 * One anchor of a placed entity, resolved to a node in the guideline graph.
 *
 * Carries the id it resolved FROM, so nothing has to be parallel to anything. This
 * replaces a bare TArray<FGuidelineNodeId> indexed in lockstep with the definition's
 * anchors - an invariant nothing enforced, and which the natural iterate-the-definition,
 * index-the-instance pattern broke by reading out of bounds the moment a saved definition
 * gained an anchor. Ordinary designer work, not an edge case.
 *
 * LocalHeading and Role are a second widening of the same idea, for the same reason:
 * URoadNetwork::PlaceEntity is given FEntityAnchor values (Model/ must not depend on the
 * Entities layer - see the top of this file), and once placement is over there is no
 * UEntityDefinition left to read them back from without breaking that rule. So the two
 * fields placement actually needs afterwards - GetAnchorWorldHeading's heading,
 * GetAnchorIdsForRole's role - are captured here rather than looked up live. The trade is
 * the one FEntityInstance's own header already accepts for Node: an anchor edited on the
 * asset after a stand is placed is not picked up by instances already placed from it.
 */
USTRUCT()
struct AIRSIDE_API FResolvedAnchor
{
	GENERATED_BODY()

	UPROPERTY() FName Id;

	UPROPERTY() FGuidelineNodeId Node;

	/** Radians, relative to the entity's own heading. Copied from FEntityAnchor::LocalHeading. */
	UPROPERTY() double LocalHeading = 0.0;

	/** Copied from FEntityAnchor::Role. */
	UPROPERTY() EServiceRole Role = EServiceRole::Aircraft;
};

/**
 * One module in a depot's plot: what occupies a bay.
 *
 * A UENUM in this UHT-parsed header rather than a plain enum beside the tool, for the reason
 * EPlaceableEntity records at its own declaration: UHT cannot resolve a type declared in a
 * header with no .generated.h, and a forward declaration does not satisfy it either.
 *
 * IN Model/ AND NOT Entities/, deliberately. UFuelService lives in another plugin's Model/
 * layer and counts a depot's pumps for itself; had this been an Entities/ type it could not
 * have, and the count would have needed a fifth captured fact to reach it. That it did not
 * is the test that this enum is in the right layer.
 *
 * WHAT EACH ONE DRIVES, and how real that is today:
 *   Shed - the truck count. LIVE: UFuelService gates dispatch on FEntityInstance::Trucks.
 *   Pump - the dwell. LIVE: UFuelService::DwellSecondsFor divides by these.
 *   Tank - storage. INERT: no fuel inventory exists anywhere yet. Counted, never read.
 * The asymmetry is deliberate and is recorded in the design doc §2: growing a consumable
 * economy inside a feel probe is how a probe stops being one.
 */
UENUM()
enum class EDepotModule : uint8
{
	Shed,
	Tank,
	Pump,
	/** Sentinel, never a real module - sizes DepotKitSpecs's walk instead of retyping Pump. */
	Count UMETA(Hidden),
};

/**
 * A placed entity. The Flyweight instance: pose plus a shared definition.
 *
 * ResolvedAnchors holds a guideline node per anchor the definition declared AT THE TIME IT
 * WAS PLACED, each tagged with the id it came from. Those nodes are created NON-DERIVED, so
 * FRoadGuidelineBuilder's orphan sweep never touches them and these handles stay valid
 * across every rebuild - which is what makes "drive to stand 12's cart position" an
 * ordinary path query rather than a lookup that goes stale the moment anyone edits a
 * taxiway.
 */
USTRUCT()
struct AIRSIDE_API FEntityInstance
{
	GENERATED_BODY()

	UPROPERTY() FVector2D Position = FVector2D::ZeroVector;

	/** Radians. */
	UPROPERTY() double Heading = 0.0;

	UPROPERTY() TObjectPtr<UEntityDefinition> Definition = nullptr;

	/** Keyed by anchor id, never by position in the definition's array. */
	UPROPERTY() TArray<FResolvedAnchor> ResolvedAnchors;

	/**
	 * Wingspan of the aircraft this stand was designed for, uu, captured at placement.
	 * CAPTURED, not read from Definition->DesignAircraft, for the same reason FResolvedAnchor
	 * copies Role: Model/ must not dereference the Entities layer. 0 means unknown.
	 * The capability summary (Model/AirsideCapability.h) classes the stand from this.
	 */
	UPROPERTY() double DesignWingspan = 0.0;

	/**
	 * The entity's OWN pose as a guideline node - for a stand, the nose gear stop.
	 *
	 * Not an anchor, and deliberately not in the array above. An anchor is a FIXTURE: a
	 * hydrant pit, a ground power point, a painted equipment box - something dug into or
	 * painted onto the concrete. There is nothing at the nose gear mark except paint, and
	 * a definition that declared one would be claiming a Code C stand has a piece of plant
	 * where the aircraft parks. RoadEntityTest asserts that invariant directly.
	 *
	 * But an aircraft routed to this stand has to be routed SOMEWHERE, and that somewhere
	 * is the stop position. So it gets a node of its own, named for what it is, rather than
	 * being smuggled into the fixture list to make pathfinding easier.
	 *
	 * Non-derived, like the anchor nodes, so the rebuild sweep leaves it alone.
	 */
	UPROPERTY() FGuidelineNodeId PoseNode;

	/**
	 * What the pose node is FOR: an aircraft's stop mark, or a service vehicle's home bay.
	 *
	 * CAPTURED AT PLACEMENT, exactly like DesignWingspan above and FResolvedAnchor::Role,
	 * and for exactly the same reason: Model/ must not dereference the Entities layer (see
	 * the top of this file), so the one field placement needs afterwards is copied in rather
	 * than read live from UEntityDefinition::PoseRole.
	 *
	 * FAnchorLink reads it to decide which CLASS of guideline the pose lead-in may join. A
	 * depot's home bay is on a road; a stand's stop mark is on a taxiway. Before this field
	 * the lead-in was cast as Aircraft unconditionally, so a depot's pose found no aircraft
	 * guideline, joined nothing, and logged a warning on every rebuild for ever.
	 *
	 * An instance saved before this existed loads as Aircraft, which is correct for every
	 * entity that COULD have been saved then - they were all stands. See
	 * UEntityDefinition::RefreshResolvedAnchors for where a later edit is picked up.
	 */
	UPROPERTY() EServiceRole PoseRole = EServiceRole::Aircraft;

	/**
	 * How many service vehicles this installation may have out at once, captured from
	 * UEntityDefinition::Trucks at placement. 0 on a stand, where it means nothing.
	 *
	 * THE THIRD CAPTURED FACT, for the same reason as the two above - but note that it is
	 * read from ANOTHER MODULE's Model/ layer (AirportOps' UFuelService), which
	 * Check-Architecture.ps1 also forbids from including Entities/. So the snapshot is not
	 * merely convenient here, it is the only way the number reaches the thing that counts
	 * against it.
	 *
	 * If a FOURTH capture arrives, these become one struct passed by reference: three
	 * trailing defaulted parameters on PlaceEntity is the most a caller can still get right.
	 */
	UPROPERTY() int32 Trucks = 0;

	/**
	 * The plot the player drew, in WORLD space. Empty means an ordinary plop with no plot -
	 * which is every stand, and every depot placed before this field existed.
	 *
	 * CLOSED IMPLICITLY: the last point joins the first and the array does NOT repeat it,
	 * the same contract UEntityDefinition::ServiceLoop states and for the same reason - a
	 * repeated point is a value that must agree with another value in the same array, which
	 * is exactly the drift FResolvedAnchor exists to remove.
	 *
	 * WORLD AND NOT LOCAL, unlike ServiceLoop, and that is a deliberate difference rather
	 * than an inconsistency: a ServiceLoop is authored once on a shared definition and must
	 * therefore be relative to whatever pose it is stamped at, while this outline is drawn
	 * by the player at world coordinates and belongs to this instance alone. Storing it
	 * local would mean unrotating the player's own clicks and rotating them back to draw,
	 * which is arithmetic that can only lose.
	 */
	UPROPERTY() TArray<FVector2D> Outline;

	/**
	 * Has a drawn outline - a depot plot OR a drawn stand. The one test for "was this drawn
	 * rather than stamped", so a count is not re-spelled at each new call site.
	 *
	 * NEVER USE IT TO MEAN "DEPOT": a stand can be plotted too (2026-09-23's "drawn stands"
	 * work), so a site that fences, prices or labels by outline alone now catches a stand as
	 * well - ask IsDepot() (or IsStand()) for kind, and IsPlotted() only for "has ground to
	 * draw". Check-Architecture.ps1's IsPlotted rule is what stops this meaning drifting back.
	 */
	bool IsPlotted() const { return Outline.Num() >= 3; }

	/** A stand: its pose node is an aircraft's stop mark. PoseRole is the captured kind -
	 *  Model/ cannot ask the definition. */
	bool IsStand() const { return PoseRole == EServiceRole::Aircraft; }

	/** A fuel depot, plotted or pre-plot. */
	bool IsDepot() const { return PoseRole == EServiceRole::Fuel; }

	/**
	 * May an aircraft be sent here at all: alive, a stand, and with a stop mark to route to.
	 * Size is NOT asked here - that is IcaoCode::StandAdmits, once this has said yes.
	 *
	 * ONE PREDICATE, TWO CALLERS: ArrivalPlanner::ChooseStand (live dispatch) and
	 * UStandAllocator::Reserve (holding a stand for an accepted flight). They used to spell
	 * the filter each their own way, and the allocator's spelling had no IsStand() - harmless
	 * while it compared raw spans, and a fuel depot handed to an airliner the day it switched
	 * to StandAdmits, under which a depot's 0 span means "unknown, admits anything" (final
	 * review C1). A member rather than a free function beside ChooseStand because both inputs
	 * are this instance's own captured facts, exactly like IsStand() above, and AirportOps
	 * already includes this header and not ArrivalPlanner's.
	 * ENFORCED BY: AirportOps.Model.StandAllocator.NeverReservesADepot.
	 */
	bool IsStandCandidate() const { return bAlive && PoseNode.IsSet() && IsStand(); }

	/**
	 * What the player put in the bays, in bay order. Empty for a plotless entity.
	 *
	 * THE FOURTH CAPTURED FACT - see Trucks above, whose comment called for exactly this:
	 * the three trailing defaulted parameters became FEntityPlacement rather than growing
	 * a fourth that a caller could still get right only by luck.
	 */
	UPROPERTY() TArray<EDepotModule> Modules;

	UPROPERTY() int32 Generation = 0;
	UPROPERTY() bool  bAlive = false;
};

/**
 * Everything one placement needs, replacing the three trailing defaulted parameters that
 * FEntityInstance::Trucks' own comment warned would not survive a fourth.
 *
 * A PLAIN STRUCT, NOT A USTRUCT. It holds a TConstArrayView, which cannot be a UPROPERTY,
 * and it is never saved, never reflected and never seen by Blueprint - it exists for the
 * length of one call. Marking it USTRUCT to match its neighbours would mean copying the
 * anchors into an owned array for no reason but decoration.
 *
 * TRUCKS IS STILL HERE, and is NOT what a plotted depot uses. A plot's truck count is
 * derived from the sheds in Modules, because the player's mix IS the fleet size and a
 * separately-stated number could only ever disagree with it. This field is what a PLOTLESS
 * caller states - every stand, and every pre-plot depot - and exactly one of the two paths
 * is taken, so the two can never both apply.
 */
struct AIRSIDE_API FEntityPlacement
{
	UEntityDefinition* Definition = nullptr;

	/** Handed in rather than read off Definition: Model/ must not dereference Entities/. */
	TConstArrayView<FEntityAnchor> Anchors;

	FVector2D Position = FVector2D::ZeroVector;

	/** Radians, as everywhere else a placement is involved. */
	double Heading = 0.0;

	double DesignWingspan = 0.0;
	EServiceRole PoseRole = EServiceRole::Aircraft;

	/** Only read when Modules is empty. See the struct comment. */
	int32 Trucks = 0;

	/** The drawn plot, world space, implicitly closed. Empty for an ordinary plop. */
	TArray<FVector2D> Outline;

	/** What fills the bays, in bay order. Empty for an ordinary plop. */
	TArray<EDepotModule> Modules;

	/**
	 * How far along Heading the pose NODE stands from Position, uu. Zero - on Position - for
	 * everything but a drawn depot.
	 *
	 * A DEPOT'S Position IS ITS GATE and stays its gate: the fence's gap, the frontage the
	 * presenter recovers and the yard's seed are all read off it. Its NODE is where its trucks
	 * live and leave from, and on the gate that was on the kerb - too close to the road for the
	 * truck to turn out onto it (FAnchorLink::PoseSetbackFor). So the two part company here, and
	 * only here.
	 */
	double PoseSetbackUu = 0.0;
};

/**
 * How a thing that comes to do this job MOVES.
 *
 * The two enums are deliberately separate - see EServiceRole - but a lead-in still has to
 * decide which traffic its guideline admits, and that decision has exactly one right
 * answer per role. Here rather than at each call site, so a fuel truck and a belt loader
 * cannot end up classified differently by two functions that both looked obvious.
 */
AIRSIDE_API ETraversalClass TraversalForRole(EServiceRole Role);
