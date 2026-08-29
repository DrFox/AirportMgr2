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

URoadProfile* URoadProfile::MakeTransient(double TotalWidth, double FilletRadius, double ShoulderWidth)
{
	URoadProfile* Profile = NewObject<URoadProfile>(GetTransientPackage());

	// Clamped so two shoulders can never exceed the road: a lane of zero or negative
	// width would put the band boundaries out of order and invert the ribbon.
	const double Shoulder = FMath::Clamp(ShoulderWidth, 0.0, TotalWidth * 0.45);

	if (Shoulder > 0.0)
	{
		FProfileBand Left;
		Left.Width = Shoulder;
		Left.Type = ERoadBandType::Shoulder;
		Profile->Bands.Add(Left);
	}

	FProfileBand Lane;
	Lane.Width = TotalWidth - 2.0 * Shoulder;
	Lane.Type = ERoadBandType::Lane;
	Profile->Bands.Add(Lane);

	if (Shoulder > 0.0)
	{
		FProfileBand Right;
		Right.Width = Shoulder;
		Right.Type = ERoadBandType::Shoulder;
		Profile->Bands.Add(Right);
	}

	FProfileLane DriveLane;
	DriveLane.CentreOffset = 0.0;
	DriveLane.Width = TotalWidth - 2.0 * Shoulder;
	DriveLane.Direction = ERoadLaneDirection::Bidirectional;
	Profile->Lanes.Add(DriveLane);

	Profile->CentrelineOffset = -1.0;
	Profile->PreferredFilletRadius = FilletRadius;
	return Profile;
}
