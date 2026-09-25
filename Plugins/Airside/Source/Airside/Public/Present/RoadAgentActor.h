#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Misc/Optional.h"
#include "Model/AgentMotion.h"
#include "RoadAgentActor.generated.h"

class UAnimInstance;
class USkeletalMesh;
class USkeletalMeshComponent;
class UStaticMeshComponent;

/**
 * One drawn link of a tow: the mesh standing on a BODY-carrying FTowLink, and what its anim
 * instance needs to read that link rather than the cab. See ARoadAgentActor::SetVehicleTrailer.
 *
 * ONE STRUCT, NOT PARALLEL ARRAYS of components and indices: the pair must never drift, and
 * the anim finds its own entry by component (ARoadAgentActor::FindTowLinkView).
 */
USTRUCT()
struct AIRSIDE_API FTowLinkView
{
	GENERATED_BODY()

	/**
	 * The trailer mesh. A UPROPERTY and NOT Transient, so a duplicated actor's copy points at
	 * the duplicate's own component rather than the source's or the CDO's - memory note
	 * "transient subobject pointers reset on duplication". Airside.Present.RigActor.
	 * TrailerSurvivesDuplication.
	 */
	UPROPERTY() TObjectPtr<USkeletalMeshComponent> Mesh = nullptr;

	/** The FAgentMotion::Tow index this mesh stands on. */
	UPROPERTY() int32 Link = INDEX_NONE;

	/**
	 * The BAR link that swings this body's front axle, or INDEX_NONE - a semi-trailer couples
	 * straight onto the cab. Decided by the dresser from FTowLink::IsBar, which the view cannot
	 * see: "the link before has no mesh" would also be true of a body whose asset failed to load.
	 */
	UPROPERTY() int32 TowbarLink = INDEX_NONE;

	/**
	 * How far this link's axle has rolled along its own heading, uu, signed; the trailer's
	 * wheel angle is this over the radius (UAirsideAgentAnim::WheelAngleFromTravel).
	 *
	 * THE AXLE'S OWN TRAVEL, NOT THE CAB'S SPEED: round a bend the trailer axle cuts in and
	 * covers less ground, and at a standstill turn it can barely move while the cab rolls on.
	 * Summed from the model's axle positions because FTowPose carries no speed - the positions
	 * ARE the model's answer, and differencing them is arithmetic, not a second trailer model.
	 * Not a UPROPERTY: view state rebuilt from the next SetMotion, never saved.
	 */
	double RolledUu = 0.0;

	/** Where the axle was at the last SetMotion; unset until the first. */
	TOptional<FVector2D> LastAxle;
};

/**
 * The aircraft that stands where an agent is. A VIEW, and nothing else.
 *
 * It holds no route, no speed and no follower: ARoadNetworkActor advances the model and
 * pushes a pose in. That split is what keeps the whole of "does it go the right way"
 * testable without a world - see FRouteFollower - and it is why this class has one method.
 *
 * It was a placeholder cube, and the airframe replaced the mesh here with nothing else
 * changing, exactly as that comment promised - because nothing else knew it was a cube.
 * The cube survives as the fallback when the airframe asset is missing.
 */
UCLASS()
class AIRSIDE_API ARoadAgentActor : public AActor
{
	GENERATED_BODY()

public:
	ARoadAgentActor();

	/**
	 * Everything about where this agent is and what it is doing, in one call.
	 *
	 * SurfaceZ stays a separate argument because it belongs to the AIRPORT rather than to the
	 * agent - every aircraft on a flat airfield shares it, and folding it into the motion
	 * would have each agent carrying its own copy of one number.
	 */
	void SetMotion(const FAgentMotion& Motion, double SurfaceZ);

	/**
	 * What the model last said this agent was doing. Read by UAirsideAgentAnim.
	 *
	 * The view keeps it and the animation reads it, rather than the animation reaching into
	 * the model: an AnimInstance that knew about followers and departures would be a second
	 * consumer of the simulation, free to disagree with the one that draws the aircraft.
	 */
	const FAgentMotion& GetMotion() const { return LastMotion; }

	void SetAirframe(USkeletalMesh* InAirframe, UClass* AnimClass = nullptr);

	/**
	 * Dress this view as a GROUND VEHICLE: Mesh if one was configured, else a box of
	 * BoxSizeUu.
	 *
	 * THE SIBLING OF SetAirframe, on this actor rather than in a second class, because
	 * everything else about showing an agent - the pose, the motion, the no-collision rule,
	 * the outliner sprite - is identical, and a second AActor would be a copy of all of it
	 * that must agree for ever.
	 *
	 * BoxSizeUu is the FULL size, X forward, so the caller passes the footprint the traffic
	 * arbiter actually reserves rather than a scale factor this class would have to know how
	 * to interpret. See UAirsideTraffic::SpawnView, which reads it off FTrafficRules.
	 */
	void SetVehicleBody(UStaticMesh* Mesh, const FVector& BoxSizeUu);

	/**
	 * Dress this view as a RIGGED ground vehicle, so its wheels turn and steer. Null Mesh
	 * leaves the box, exactly as SetVehicleBody does.
	 *
	 * SEPARATE FROM SetVehicleBody rather than an overload on it, because the two take
	 * unrelated mesh types and an overload pair distinguished only by UStaticMesh* against
	 * USkeletalMesh* resolves on whichever the caller happens to hold - including, for a
	 * nullptr literal, ambiguously.
	 *
	 * BoxSizeUu means what it means on SetVehicleBody: the footprint the arbiter reserves,
	 * kept so the fallback box is right if Mesh is null.
	 */
	void SetVehicleAirframe(USkeletalMesh* Mesh, UClass* AnimClass, const FVector& BoxSizeUu);

	/**
	 * Dress tow link Link with a trailer mesh: creates that link's skeletal component, or
	 * re-dresses it if it exists. Null Mesh draws nothing for the link. TowbarLink is the bar
	 * link that swings this body's front axle, INDEX_NONE for none - see FTowLinkView.
	 *
	 * SetMotion then stands it on Motion.Tow[Link]'s AXLE at that link's heading, every
	 * frame, because every trailer mesh here has its origin on its link's axle (tankTrailer1 at
	 * the tandem centre, fuelTrailer1 at its rear axle - measured off the imported skeletons,
	 * 2026-09-24). A mesh whose origin sat elsewhere would need an offset HERE, not a second
	 * pose from the model.
	 *
	 * TAKES A LINK, unlike the brief's (Mesh, AnimClass): the tow is a chain (spec 2026-09-24
	 * §4) and the utility's body is link 1, not 0 - so which pose a mesh stands on is part of
	 * dressing it. The component is created at run time, never in the constructor, so a rigid
	 * vehicle and every aircraft carry none.
	 */
	void SetVehicleTrailer(int32 Link, USkeletalMesh* Mesh, UClass* AnimClass, int32 TowbarLink = INDEX_NONE);

	/**
	 * The drawn link whose mesh is Component, or null - the cab's own component, an aircraft's.
	 * Read by UAirsideAgentAnim, which is how a trailer's anim instance learns it is one.
	 *
	 * LOOKED UP, NOT PUSHED onto the anim instance: an anim instance is re-created whenever its
	 * component re-initialises (a mesh swap, a re-register), and a link index set on the old one
	 * would be silently lost. The component is the stable key.
	 */
	const FTowLinkView* FindTowLinkView(const USkeletalMeshComponent* Component) const;

	/** Link's trailer component, or null. For Airside.Present.RigActor.*. */
	USkeletalMeshComponent* TrailerForTest(int32 Link) const;

	/** How many trailer meshes this view draws. For Airside.Present.RigActor.*. */
	int32 TrailerCountForTest() const { return TowViews.Num(); }

	/**
	 * True once either vehicle path has dressed this view. Read by UAirsideAgentAnim, which
	 * measures a vehicle's wheel radius off its skeleton and keeps an aircraft's authored one.
	 */
	bool IsVehicle() const { return bIsVehicle; }

	/** True once either vehicle path has dressed this view. For Airside.Present.VehicleAgentView. */
	bool HasVehicleBodyForTest() const { return bIsVehicle; }

	/** What the placeholder was sized to, uu. For the same test, which checks it against
	 *  FTrafficRules rather than against a literal. */
	FVector PlaceholderSizeForTest() const { return PlaceholderSizeUu; }

private:
	/**
	 * The aircraft. SKELETAL, so the propeller and wheels can turn - see UAirsideAgentAnim.
	 *
	 * The root, so the placeholder can hang off it and be hidden rather than juggled.
	 */
	UPROPERTY() TObjectPtr<USkeletalMeshComponent> Airframe;

	/**
	 * The box that stands in when no airframe was assigned.
	 *
	 * A SEPARATE COMPONENT now, because a static mesh cannot live in a skeletal one. Kept
	 * rather than dropped for the reason it always was: a missing airframe should look like
	 * the box this used to be rather than like an agent that failed to spawn - one of those
	 * reads as a content problem and the other as a routing bug.
	 */
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Placeholder;

	/**
	 * One entry per BODY-carrying tow link, in tow order; empty for anything rigid. See
	 * FTowLinkView for why it is a UPROPERTY and not Transient.
	 */
	UPROPERTY() TArray<FTowLinkView> TowViews;

	/**
	 * False when the airframe asset was missing and the cube stood in.
	 *
	 * SetPose needs it: the cube's pivot is at its centre and must be lifted, the airframe's
	 * is on the ground and must not be.
	 *
	 * NOT a UPROPERTY. It is decided by the constructor from what loaded, so it is a fact
	 * about construction rather than authored or saved state - and agents are transient and
	 * never serialised anyway.
	 */
	bool bHasAirframe = false;

	/**
	 * The placeholder's world size in uu, so SetMotion's lift follows whatever the box was
	 * scaled to.
	 *
	 * A MEMBER RATHER THAN A CONSTANT, because the box is no longer one size: an aircraft's
	 * stand-in is 4 m x 4 m x 2 m and a van's is its own footprint. The lift is half the
	 * HEIGHT, and reading it off a constant after the scale changed would sink a van into
	 * the road by exactly the difference.
	 *
	 * NOT a UPROPERTY, for the reason bHasAirframe is not: it is decided by construction or
	 * by a dressing call, and agents are transient and never serialised.
	 */
	FVector PlaceholderSizeUu = FVector(400.0, 400.0, 200.0);

	/** False for an aircraft view. Only records WHAT dressed this actor - the pose maths is
	 *  the same either way, and reads PlaceholderSizeUu rather than this. */
	bool bIsVehicle = false;

	/**
	 * What the model last said this agent was doing.
	 *
	 * Not read by anything yet: the mesh is still a static one and cannot animate. It is here
	 * because it is what the AnimInstance will read, and keeping it means the model half of
	 * the animation is complete and can be got right before the rigged asset arrives.
	 */
	FAgentMotion LastMotion;
};
