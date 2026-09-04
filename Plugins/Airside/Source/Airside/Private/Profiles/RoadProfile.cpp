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
	Fill(Profile, TotalWidth, FilletRadius, ShoulderWidth);
	return Profile;
}

void URoadProfile::Fill(URoadProfile* Profile, double TotalWidth, double FilletRadius,
	double ShoulderWidth)
{
	if (Profile == nullptr)
	{
		return;
	}

	Profile->Bands.Reset();
	Profile->Guidelines.Reset();

	// Clamped so two shoulders can never exceed the road: a lane of zero or negative
	// width would put the band boundaries out of order and invert the ribbon.
	const double Shoulder = FMath::Clamp(ShoulderWidth, 0.0, TotalWidth * 0.45);

	if (Shoulder > 0.0)
	{
		FProfileBand Left;
		Left.Width = Shoulder;
		Left.Type = ERoadBandType::Shoulder;
		Left.MaterialSlot = TEXT("Asphalt");
		Profile->Bands.Add(Left);
	}

	// Named so a transient profile shows per-band materials the moment a set is assigned.
	// A taxiway is concrete between asphalt run-offs; with no material set these names are
	// inert and every band is slot 0, exactly as before.
	FProfileBand Lane;
	Lane.Width = TotalWidth - 2.0 * Shoulder;
	Lane.Type = ERoadBandType::Lane;
	Lane.MaterialSlot = TEXT("Concrete");
	Profile->Bands.Add(Lane);

	if (Shoulder > 0.0)
	{
		FProfileBand Right;
		Right.Width = Shoulder;
		Right.Type = ERoadBandType::Shoulder;
		Right.MaterialSlot = TEXT("Asphalt");
		Profile->Bands.Add(Right);
	}

	// One guideline, centred, bidirectional, carrying aircraft: a taxiway. Every existing
	// caller of MakeTransient is a taxiway or a stand-in for one, and the lane it used to
	// declare was read by nothing.
	FProfileGuideline Centre;
	Centre.CentreOffset = 0.0;
	Centre.Class = ETraversalClass::Aircraft;
	Centre.Direction = EGuidelineDir::Bidirectional;
	Centre.Width = 0.0;
	Profile->Guidelines.Add(Centre);

	Profile->CentrelineOffset = -1.0;
	Profile->PreferredFilletRadius = FilletRadius;
}
