#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Model/RoadTraffic.h"
#include "RoadProfile.generated.h"

UENUM()
enum class ERoadBandType : uint8
{
	Shoulder,
	Lane,
	Curb
};

UENUM()
enum class ERoadLaneDirection : uint8
{
	Forward,
	Backward,
	Bidirectional
};

/** One lateral band of the cross-section, ordered left to right. */
USTRUCT()
struct ROADNET_API FProfileBand
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
USTRUCT()
struct ROADNET_API FProfileGuideline
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
UCLASS()
class ROADNET_API URoadProfile : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere) TArray<FProfileBand> Bands;
	UPROPERTY(EditAnywhere) TArray<FProfileGuideline> Guidelines;

	/** Distance from the leftmost band edge to the centreline. Defaults to half the total width. */
	UPROPERTY(EditAnywhere) double CentrelineOffset = -1.0;

	/** Preferred corner radius in uu. Clamped by geometry at solve time. */
	UPROPERTY(EditAnywhere) double PreferredFilletRadius = 1500.0;

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
};
