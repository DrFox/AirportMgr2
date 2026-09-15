#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Model/RoadTraffic.h"
#include "RoadProfile.generated.h"

/**
 * Which authored cross-section a build gesture lays.
 *
 * A KIND, NOT A URoadProfile*, wherever a tool is involved: a tool has no business naming an
 * asset, and resolving which profile a kind MEANS is the facade's job
 * (ARoadNetworkActor::ResolveProfile / ::ResolveServiceRoadProfile) - in one place, where a
 * missing one can be refused once. See Tool/RoadEditTarget.h, whose ConnectNodes and
 * UpdateGhost take this.
 *
 * HERE RATHER THAN ON THE TOOL SEAM, where it was first written. Tool/RoadEditTarget.h has no
 * .generated.h, so UHT never parses it and could not resolve the type when it appeared in
 * ARoadNetworkActor's declarations - "Unable to find 'class', 'delegate', 'enum', or 'struct'
 * with name 'ERoadKind'", before the compiler is reached. A forward declaration does not
 * satisfy UHT either. This header is already parsed, and the enum names a kind of
 * cross-section, which is what this file is about - so the constraint and the right home
 * happen to agree.
 *
 * NOT ON FToolContext. The kind is a fact about the TOOL the player selected, not about the
 * gesture, and a context field would let two tools disagree about it - the same distinction
 * FRoadDrawTool draws between a drawing STATE and a drag.
 */
UENUM()
enum class ERoadKind : uint8
{
	Taxiway,
	ServiceRoad
};

UENUM(BlueprintType)
enum class ERoadBandType : uint8
{
	Shoulder,
	Lane,
	Curb
};

/** One lateral band of the cross-section, ordered left to right. */
USTRUCT(BlueprintType)
struct AIRSIDE_API FProfileBand
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere) double Width = 0.0;
	UPROPERTY(EditAnywhere) ERoadBandType Type = ERoadBandType::Lane;
	UPROPERTY(EditAnywhere) FName MaterialSlot;
};

/**
 * One guideline this cross-section generates.
 *
 * This replaces FProfileLane, which modelled a road lane and had no reader. A taxiway
 * declares exactly one of these; a two-lane road declares two with mirrored offsets and
 * opposing directions, which is the case that recovers what "lane" used to mean.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FProfileGuideline
{
	GENERATED_BODY()

	/** Lateral offset from the centreline in uu: positive left, negative right. */
	UPROPERTY(EditAnywhere) double CentreOffset = 0.0;

	UPROPERTY(EditAnywhere) ETraversalClass Class = ETraversalClass::Aircraft;
	UPROPERTY(EditAnywhere) EGuidelineDir Direction = EGuidelineDir::Bidirectional;

	/** Physical extent for marking and clearance. NOT a capacity - see FGuidelineEdge. */
	UPROPERTY(EditAnywhere) double Width = 0.0;

	/** 0 means unlimited. */
	UPROPERTY(EditAnywhere) double MaxWingspan = 0.0;
};

/**
 * Shared, immutable cross-section description (Flyweight).
 * A taxiway, runway and service road differ only by their profile asset.
 */
UCLASS(BlueprintType)
class AIRSIDE_API URoadProfile : public UDataAsset
{
	GENERATED_BODY()

public:
	/**
	 * A real taxiway's width, uu - 23 m. The one figure ARoadNetworkActor::FallbackWidth's
	 * own default and the holding-position marking's width-with-no-profile-to-ask fallback
	 * both typed independently as a bare 2300.0 (#103); this is that number, named once.
	 */
	static constexpr double StandardTaxiwayWidth = 2300.0;

	/**
	 * What a metre of this profile costs to lay, and what a day of owning it costs.
	 *
	 * ON THE PROFILE rather than in a cost table beside it, so a new taxiway width cannot be
	 * added without a price. A central table keyed by asset was the alternative and was
	 * rejected: forget a row there and the profile builds free, with nothing anywhere to say
	 * so - the "lists that must agree" failure this codebase has shipped three times.
	 *
	 * A FIGURE ONLY AirportOps EVER READS, and that is deliberate. Airside owns the geometry,
	 * so Airside is the only layer that can say how much of it there is; putting the rate
	 * anywhere else would mean something outside this plugin had to know what a profile is
	 * made of. See FBuildQuote, and BuildCost, which is the only reader in this plugin.
	 *
	 * Zero by default, so a profile nobody has priced builds free rather than at some invented
	 * figure - visible in the ghost as a build that costs nothing, which is the right way for
	 * an un-authored asset to fail.
	 */
	UPROPERTY(EditAnywhere, Category = "Cost", meta = (ClampMin = "0.0")) double CostPerMetre = 0.0;
	UPROPERTY(EditAnywhere, Category = "Cost", meta = (ClampMin = "0.0")) double UpkeepPerMetrePerDay = 0.0;

	UPROPERTY(EditAnywhere) TArray<FProfileBand> Bands;
	UPROPERTY(EditAnywhere) TArray<FProfileGuideline> Guidelines;

	/** Distance from the leftmost band edge to the centreline. Defaults to half the total width. */
	UPROPERTY(EditAnywhere) double CentrelineOffset = -1.0;

	/**
	 * Preferred corner radius in uu, clamped by geometry at solve time. ZERO MEANS DERIVE -
	 * see ResolvedFilletRadius, and never read this field directly.
	 *
	 * 1500 is a TAXIWAY's, authored, and stays authored: a taxiway's corner is swept for the
	 * largest AIRCRAFT admitted, and that figure comes from IcaoCode rather than any vehicle.
	 */
	UPROPERTY(EditAnywhere) double PreferredFilletRadius = 1500.0;

	/**
	 * Headroom over the bare steering limit, because RoadNetworkSolver scales a preferred
	 * radius DOWN to fit a junction's arms - which is how a 500 uu fillet became 418 on the
	 * route that reported this in 2026-09-14, under a lock that needed 510. A fillet that
	 * merely clears the lock on paper does not clear it on a real junction.
	 *
	 * 1.25 covers the scaling seen in play with room to spare, without demanding a motorway
	 * sweep on an apron road. It is the figure Airside.Model.ServiceRoadFilletClearsTheTruckLock
	 * already argued for; this moves that decision from the test into the code it judges.
	 */
	static constexpr double JunctionScalingMargin = 1.25;

	/**
	 * The radius a junction on this profile actually turns on: the authored one, or - when
	 * that is zero - one derived from the largest vehicle admitted.
	 *
	 * THE ONLY LEGAL READER OF PreferredFilletRadius. Read the field directly and a service
	 * road turns on nothing at all.
	 *
	 * The sentinel exists so the ASSET CARRIES NO NUMBER. A stored radius is stale the moment
	 * a larger vehicle joins the fleet, and the four places this figure used to be typed -
	 * this header, build_road_profiles.py, DA_RoadProfile_ServiceRoad, and the test holding
	 * two of them together - are exactly the arrangement that shipped a ten-centimetre
	 * shortfall nobody could see.
	 */
	double ResolvedFilletRadius() const;

	/**
	 * Segments with this profile PASS THROUGH a node rather than ending at it, so they are
	 * never trimmed and no junction polygon is paved over them. True for a runway.
	 *
	 * ON THE PROFILE, not on the segment, because continuity is a fact about the KIND of
	 * pavement: a runway edge runs unbroken from threshold to threshold and a taxiway meeting
	 * it fillets into that edge, while two taxiways meeting each other both give way to a
	 * paved junction. Putting it here also means splitting a runway to add an exit cannot
	 * lose it - both halves keep the profile, and so keep the property.
	 *
	 * It is also what makes "a runway must be straight" cost nothing in the model: the halves
	 * of a split segment are collinear by construction, and an uncut node between two
	 * collinear arms produces no polygon at all.
	 *
	 * See FJunctionArm::bContinuous for what the solver does with it, and
	 * Airside.Solve.RunwayContinuity for the property it buys.
	 */
	UPROPERTY(EditAnywhere) bool bContinuousThroughJunctions = false;

	/**
	 * How far before a junction an exit from THIS profile begins, uu, measured along the
	 * centreline - and the same distance back along the taxiway that meets it. Read only
	 * from a continuous profile: the runway decides its own exits, per profile, so an 18 m
	 * strip and a 45 m one can differ from the details panel.
	 *
	 * A LENGTH, NOT A RADIUS, deliberately. Equal tangent lengths L either side of an angle
	 * theta make a near-circular arc of radius L / tan(theta / 2): at 60 m a 30 degree exit
	 * gets 224 m, 45 degrees gets 145 m, 90 degrees gets 60 m. That is how real exits are
	 * graded - rapid exits shallow and wide, right-angle exits tight - from one number the
	 * player can read off the ground. A fixed radius would hand a 90 degree exit a 150 m
	 * sweep that eats the taxiway.
	 *
	 * Per-aircraft figures are never consulted: taxi lines are infrastructure, sized for the
	 * largest aircraft admitted, not for the one taxiing. See the runway exit arcs spec.
	 */
	UPROPERTY(EditAnywhere) double ExitLength = 6000.0;

	double GetTotalWidth() const;
	double GetHalfWidthLeft() const;
	double GetHalfWidthRight() const;

	/**
	 * The wider of the two sides - what a corner-fit reach, a ghost material's edge
	 * distance, or a placement limit's half-width cares about, never which side is which.
	 * `FMath::Max(GetHalfWidthLeft(), GetHalfWidthRight())` was typed at five call sites
	 * (RoadPlacement.cpp x3, RoadNetworkActor.cpp, RoadSurfacePresenter.cpp) - one of them
	 * drifting from the others was how a corner check and its ghost's colour could disagree
	 * about the same road's half-width.
	 */
	double GetMaxHalfWidth() const { return FMath::Max(GetHalfWidthLeft(), GetHalfWidthRight()); }

	/**
	 * Symmetric profile for tests and the debug gallery.
	 *
	 * ShoulderWidth > 0 produces shoulder | lane | shoulder, which is what the ground
	 * blend needs: a profile of one Lane band has no outer shoulder, so there is nothing
	 * to fade and the road ends in a knife edge. Defaults to 0 so every existing caller
	 * keeps the single-band profile it already had.
	 */
	static URoadProfile* MakeTransient(double TotalWidth, double FilletRadius, double ShoulderWidth = 0.0);

	/**
	 * Fills Profile with the standard taxiway cross-section, replacing whatever it held.
	 *
	 * Exposed to script for the same reason UEntityDefinition::BuildCodeCStand is: the
	 * authoring commandlet that writes DA_RoadProfile_Taxiway must lay down the SAME bands
	 * the tests exercise, rather than a second transcription of them that is free to drift.
	 *
	 * MakeTransient is this plus a NewObject, so there is one description of a taxiway.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	static void Fill(URoadProfile* Profile, double TotalWidth, double FilletRadius,
		double ShoulderWidth = 0.0);

	/**
	 * Fills Profile with the SERVICE ROAD cross-section: kerb | lane | kerb, and one
	 * guideline of class GroundVehicle.
	 *
	 * A SECOND FILL RATHER THAN A PARAMETER ON THE FIRST, deliberately. Fill's taxiway is a
	 * concrete lane between asphalt run-offs carrying ONE AIRCRAFT guideline; this is a
	 * narrow kerbed lane carrying ONE VEHICLE guideline, and the two differ in band type,
	 * band count, guideline class and exit length. A shared function taking five flags would
	 * be a switch on "which road is this" spelled as parameters, and every caller would
	 * still have to know which combination meant a road.
	 *
	 * NOT CONTINUOUS and NO EXIT LENGTH - see bContinuousThroughJunctions and ExitLength. A
	 * road gives way to a paved junction; only a runway runs unbroken through one, and only
	 * a runway grades its own exits.
	 *
	 * Exposed to script for the same reason Fill is: the authoring commandlet that writes
	 * DA_RoadProfile_ServiceRoad must lay down the SAME bands the tests exercise, rather
	 * than a second transcription of them that is free to drift.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	static void FillServiceRoad(URoadProfile* Profile, double LaneWidth, double KerbWidth,
		double FilletRadius);

	/**
	 * FillServiceRoad plus a NewObject, so there is one description of a service road.
	 *
	 * The defaults are a 6 m lane with 0.6 m kerbs: wide enough for two vans to pass, tight
	 * enough that a road reads as a road beside a 23 m taxiway - whose own fillet is 15.3 m,
	 * so this is still visibly the smaller junction.
	 *
	 * THE CORNER IS NOT A NUMBER ANY MORE. It was 500 uu, then 750, typed in four places -
	 * here, build_road_profiles.py, DA_RoadProfile_ServiceRoad, and the test pinning two of
	 * them together - and a rigid vehicle cannot follow an arc tighter than Wheelbase /
	 * sin(lock) at any speed. In 2026-09-14 the authored 500 was ten centimetres under what
	 * the truck's lock needed, and the fix went the WRONG WAY: the lock was widened to fit
	 * the road. A zero here means "derive it from the largest vehicle admitted", which is the
	 * rule aircraft geometry already follows, and leaves no second figure to drift.
	 * See URoadProfile::ResolvedFilletRadius.
	 */
	static URoadProfile* MakeServiceRoadTransient(double LaneWidth = 600.0,
		double KerbWidth = 60.0, double FilletRadius = 0.0);
};
