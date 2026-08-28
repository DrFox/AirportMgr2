#include "Profiles/RoadProfile.h"

double URoadProfile::GetTotalWidth() const
{
	double Total = 0.0;
	for (const FProfileBand& Band : Bands)
	{
		Total += Band.Width;
	}
	return Total;
}

double URoadProfile::GetHalfWidthLeft() const
{
	const double Total = GetTotalWidth();
	if (CentrelineOffset < 0.0)
	{
		return Total * 0.5;   // sentinel: symmetric
	}

	// Clamped so GetHalfWidthRight() can never go negative. The junction solver
	// offsets edge rays by these half-widths; a negative one mirrors an edge to
	// the wrong side of the centreline and inverts the junction polygon.
	return FMath::Min(CentrelineOffset, Total);
}

double URoadProfile::GetHalfWidthRight() const
{
	return GetTotalWidth() - GetHalfWidthLeft();
}

URoadProfile* URoadProfile::MakeTransient(double TotalWidth, double FilletRadius)
{
	URoadProfile* Profile = NewObject<URoadProfile>(GetTransientPackage());

	FProfileBand Band;
	Band.Width = TotalWidth;
	Band.Type = ERoadBandType::Lane;
	Profile->Bands.Add(Band);

	FProfileLane Lane;
	Lane.CentreOffset = 0.0;
	Lane.Width = TotalWidth;
	Lane.Direction = ERoadLaneDirection::Bidirectional;
	Profile->Lanes.Add(Lane);

	Profile->CentrelineOffset = -1.0;
	Profile->PreferredFilletRadius = FilletRadius;
	return Profile;
}
