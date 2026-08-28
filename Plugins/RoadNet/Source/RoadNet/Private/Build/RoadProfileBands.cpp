#include "Build/RoadProfileBands.h"

#include "Profiles/RoadProfile.h"

FRoadProfileBands FRoadProfileBands::FromProfile(const URoadProfile* Profile)
{
	FRoadProfileBands Out;

	const double HalfLeft  = Profile ? FMath::Max(Profile->GetHalfWidthLeft(),  0.0) : 0.0;
	const double HalfRight = Profile ? FMath::Max(Profile->GetHalfWidthRight(), 0.0) : 0.0;
	const double Total = HalfLeft + HalfRight;

	// No width, or no profile: the two outer boundaries coincide. The builder drops the
	// resulting degenerate triangles, so this produces nothing rather than misbehaving.
	if (Total <= 0.0 || Profile == nullptr || Profile->Bands.Num() == 0)
	{
		Out.Alphas = { 0.0, 1.0 };
		Out.Laterals = { static_cast<float>(-HalfRight), static_cast<float>(HalfLeft) };
		Out.GroundBlend = { 1.0f, 1.0f };
		return Out;
	}

	// Bands are ordered left to right; boundaries are walked right to left so the alphas
	// ascend the way Lerp(RightCut, LeftCut, Alpha) does.
	const int32 BandCount = Profile->Bands.Num();
	const bool bLeftShoulder  = Profile->Bands[0].Type == ERoadBandType::Shoulder;
	const bool bRightShoulder = Profile->Bands[BandCount - 1].Type == ERoadBandType::Shoulder;

	Out.Alphas.Reserve(BandCount + 1);
	Out.Laterals.Reserve(BandCount + 1);
	Out.GroundBlend.Reserve(BandCount + 1);

	double Lateral = -HalfRight;
	for (int32 Boundary = 0; Boundary <= BandCount; ++Boundary)
	{
		// Exactly 0 and exactly 1 at the ends, not (Lateral + HalfRight) / Total, so the
		// outermost band points reproduce the stored cut vertices bit for bit.
		const double Alpha =
			(Boundary == 0)         ? 0.0 :
			(Boundary == BandCount) ? 1.0 :
			(Lateral + HalfRight) / Total;

		Out.Alphas.Add(Alpha);
		Out.Laterals.Add(static_cast<float>(Lateral));

		const bool bOuterRight = (Boundary == 0) && bRightShoulder;
		const bool bOuterLeft  = (Boundary == BandCount) && bLeftShoulder;
		Out.GroundBlend.Add((bOuterRight || bOuterLeft) ? 0.0f : 1.0f);

		if (Boundary < BandCount)
		{
			// Walking right to left consumes the bands in reverse order.
			Lateral += FMath::Max(Profile->Bands[BandCount - 1 - Boundary].Width, 0.0);
		}
	}

	return Out;
}
