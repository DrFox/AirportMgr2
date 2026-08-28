#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
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

/** A navigable lane. Populated in Slice 1, consumed in Slice 4. */
USTRUCT()
struct ROADNET_API FProfileLane
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere) double CentreOffset = 0.0;
	UPROPERTY(EditAnywhere) double Width = 0.0;
	UPROPERTY(EditAnywhere) ERoadLaneDirection Direction = ERoadLaneDirection::Bidirectional;
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
	UPROPERTY(EditAnywhere) TArray<FProfileLane> Lanes;

	/** Distance from the leftmost band edge to the centreline. Defaults to half the total width. */
	UPROPERTY(EditAnywhere) double CentrelineOffset = -1.0;

	/** Preferred corner radius in uu. Clamped by geometry at solve time. */
	UPROPERTY(EditAnywhere) double PreferredFilletRadius = 1500.0;

	double GetTotalWidth() const;
	double GetHalfWidthLeft() const;
	double GetHalfWidthRight() const;

	/** Symmetric single-band profile for tests and the debug gallery. */
	static URoadProfile* MakeTransient(double TotalWidth, double FilletRadius);
};
