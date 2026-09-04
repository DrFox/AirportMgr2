#include "Build/RoadProfileBands.h"

#include "Profiles/RoadMaterialSet.h"
#include "Profiles/RoadProfile.h"

int32 FRoadProfileBands::BandAt(double Alpha) const
{
	const int32 BandCount = Alphas.Num() - 1;
	if (BandCount <= 0)
	{
		return INDEX_NONE;
	}

	// Clamped rather than refused at the ends. Alpha comes from CentrelineAlpha, which is
	// exactly 0 or 1 for a profile whose centreline sits on an outer edge - a legitimate
	// asymmetric profile, not an error.
	for (int32 Band = 0; Band < BandCount; ++Band)
	{
		if (Alpha < Alphas[Band + 1])
		{
			return Band;
		}
	}

	return BandCount - 1;
}

FRoadProfileBands FRoadProfileBands::FromProfile(const URoadProfile* Profile,
	const URoadMaterialSet* Materials)
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

		// One notional band, so SlotForBand answers the same way here as anywhere else.
		Out.SlotIndices = { 0 };
		Out.CentrelineAlpha = Total > 0.0 ? HalfRight / Total : 0.5;
		return Out;
	}

	// Bands are ordered left to right; boundaries are walked right to left so the alphas
	// ascend the way Lerp(RightCut, LeftCut, Alpha) does.
	const int32 BandCount = Profile->Bands.Num();

	Out.Alphas.Reserve(BandCount + 1);
	Out.Laterals.Reserve(BandCount + 1);

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

		if (Boundary < BandCount)
		{
			// Walking right to left consumes the bands in reverse order.
			Lateral += FMath::Max(Profile->Bands[BandCount - 1 - Boundary].Width, 0.0);
		}
	}

	// The centreline sits at lateral 0, which is HalfRight along a cut line that starts at
	// -HalfRight. Derived here from the same two half-widths the laterals use, so no caller
	// can arrive at a different answer for where the middle of the road is.
	Out.CentrelineAlpha = HalfRight / Total;

	// Slots, in the SAME right-to-left order as the boundaries above - hence the reversed
	// index. Getting this backwards mirrors every profile's materials and nothing else
	// changes, which is why the test that covers it uses an asymmetric profile.
	Out.SlotIndices.Reserve(BandCount);
	for (int32 Band = 0; Band < BandCount; ++Band)
	{
		const FProfileBand& Source = Profile->Bands[BandCount - 1 - Band];

		int32 Slot = 0;
		if (Materials != nullptr && !Source.MaterialSlot.IsNone())
		{
			Slot = Materials->IndexOf(Source.MaterialSlot);
			if (Slot == INDEX_NONE)
			{
				// Fall back, and SAY SO. An id the set cannot address would be discarded by
				// the scene proxy without a word, so the id is always made valid here and
				// the mistake is reported instead.
				//
				// A band that names NO slot is not counted: an unfilled field is not a
				// misspelling, and every profile authored before this slice has one.
				Slot = 0;
				++Out.UnresolvedSlots;
			}
		}

		Out.SlotIndices.Add(Slot);
	}

	return Out;
}
