#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Model/RoadTraffic.h"
#include "RoadProfile.generated.h"

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
	UPROPERTY(EditAnywhere) TArray<FProfileBand> Bands;
	UPROPERTY(EditAnywhere) TArray<FProfileGuideline> Guidelines;

	/** Distance from the leftmost band edge to the centreline. Defaults to half the total width. */
	UPROPERTY(EditAnywhere) double CentrelineOffset = -1.0;

	/** Preferred corner radius in uu. Clamped by geometry at solve time. */
	UPROPERTY(EditAnywhere) double PreferredFilletRadius = 1500.0;

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
	 * The defaults are a 6 m lane with 0.6 m kerbs on a 5 m corner: wide enough for two vans
	 * to pass, tight enough that a road reads as a road beside a 23 m taxiway.
	 */
	static URoadProfile* MakeServiceRoadTransient(double LaneWidth = 600.0,
		double KerbWidth = 60.0, double FilletRadius = 500.0);
};
