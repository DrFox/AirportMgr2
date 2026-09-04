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
};
