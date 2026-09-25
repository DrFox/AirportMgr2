#include "Profiles/RoadProfile.h"

#include "Content/AirsideSettings.h"
#include "Model/Chassis.h"

double URoadProfile::ResolvedFilletRadius() const
{
	// ONE PLACE, which is the whole point - Content/ resolves every content default in exactly
	// one function, and this is that function for a vehicle-sized corner. A road that admits
	// the largest service vehicle can be turned on by every smaller one; sizing it for the
	// truck that happens to be driving now is the mistake the aircraft geometry rule already
	// names.
	//
	// SELF-RESOLVING, for a caller that asks once and is not inside a hot loop - see the
	// other overload for the one that is (issue #190).
	// PER TIER since 2026-09-25: this profile's own design vehicle - the rig for the Wide
	// service road, the largest rigid vehicle otherwise (UAirsideSettings::ResolveTierDesignVehicles).
	// Looked up in the cached tier map, never by building an FRoadDesignVehicles per arm (a map
	// copy) - see ResolveTierDesignVehicles' comment on who calls this how often.
	return ResolvedFilletRadius(ResolvedDesignVehicle());
}

FChassis URoadProfile::ResolvedDesignVehicle() const
{
	// THE ONE TIER LOOKUP AND FALLBACK both self-resolving answers read (review of 5660420c):
	// this profile's tier vehicle, else the largest rigid service vehicle.
	const FChassis* Tier = UAirsideSettings::ResolveTierDesignVehicles().Find(TObjectKey<URoadProfile>(this));
	return Tier != nullptr ? *Tier : UAirsideSettings::ResolveLargestServiceVehicle();
}

double URoadProfile::ResolvedDesignRadius() const
{
	// The same vehicle as ResolvedFilletRadius() above, answering with the lock rather than a fillet.
	return ResolvedDesignVehicle().TightestFollowableRadius();
}

double URoadProfile::ResolvedFilletRadius(const FChassis& DesignVehicle) const
{
	if (PreferredFilletRadius > 0.0)
	{
		return PreferredFilletRadius;
	}

	return DesignVehicle.TightestFollowableRadius() * JunctionScalingMargin;
}

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
	Profile->bMaterialCentreline = true;
}

URoadProfile* URoadProfile::MakeServiceRoadTransient(double LaneWidth, double KerbWidth,
	double FilletRadius)
{
	URoadProfile* Profile = NewObject<URoadProfile>(GetTransientPackage());
	FillTwoWayRoad(Profile, LaneWidth, KerbWidth, FilletRadius);
	return Profile;
}

void URoadProfile::FillTwoWayRoad(URoadProfile* Profile, double LaneWidth, double KerbWidth,
	double FilletRadius)
{
	if (Profile == nullptr)
	{
		return;
	}

	Profile->Bands.Reset();
	Profile->Guidelines.Reset();

	// Clamped for the reason Fill clamps its shoulder: a band of zero or negative width
	// would put the band boundaries out of order and invert the ribbon.
	const double Lane = FMath::Max(LaneWidth, 0.0);
	const double Kerb = FMath::Clamp(KerbWidth, 0.0, Lane * 0.45);

	auto AddBand = [Profile](double Width, ERoadBandType Type, const TCHAR* Slot)
	{
		if (Width <= 0.0)
		{
			return;
		}
		FProfileBand Band;
		Band.Width = Width;
		Band.Type = Type;
		Band.MaterialSlot = Slot;
		Profile->Bands.Add(Band);
	};

	// KERBS, NOT RUN-OFFS, and that is the whole visible difference from a taxiway. A
	// taxiway is graded so an aircraft that leaves the pavement survives leaving it; a road
	// has a raised edge whose job is to keep a van on it. Slot names match DA_RoadMaterials
	// (Asphalt, Concrete, Kerb) so a material set assigned on the actor skins this with no
	// further authoring; with no set they are inert and every band is slot 0, as before.
	AddBand(Kerb, ERoadBandType::Curb, TEXT("Kerb"));
	// TWO LANE BANDS rather than one of twice the width: the ribbon is the same, but the
	// cross-section now says how many lanes it carries, which is what the lanes are.
	AddBand(Lane, ERoadBandType::Lane, TEXT("Asphalt"));
	AddBand(Lane, ERoadBandType::Lane, TEXT("Asphalt"));
	AddBand(Kerb, ERoadBandType::Curb, TEXT("Kerb"));

	// TWO one-way lanes, one each way (spec 2026-09-23). Until then this was ONE centred
	// bidirectional line, "because a 6 m lane IS one line both ways" - which left nothing to
	// route on a side of, and trucks meeting head-on on a single line.
	//
	// AUTHORED FOR RIGHT-HAND TRAFFIC. The A->B lane is at the POSITIVE offset: the model's
	// left is PerpCCW of the tangent, and UE is left-handed, so seen from above that is
	// screen-RIGHT of travel. Left-hand traffic is FProfileGuideline::OffsetFor's mirror.
	// ENFORCED BY: Airside.Build.TwoWay.LanesDerived (asserts world Y, not this sign)
	for (const EGuidelineDir Dir : { EGuidelineDir::AToB, EGuidelineDir::BToA })
	{
		FProfileGuideline Line;
		Line.CentreOffset = (Dir == EGuidelineDir::AToB ? 0.5 : -0.5) * Lane;
		Line.Class = ETraversalClass::GroundVehicle;
		Line.Direction = Dir;
		Line.Width = Lane;

		// 0 IS UNLIMITED (see FProfileGuideline::MaxWingspan), which is right rather than
		// lax: nothing with a wing is admitted here at all - the CLASS refuses aircraft - so
		// a span limit would be a second, weaker statement of a rule already made exactly.
		Line.MaxWingspan = 0.0;
		Profile->Guidelines.Add(Line);
	}

	Profile->CentrelineOffset = -1.0;
	Profile->PreferredFilletRadius = FilletRadius;
	Profile->bContinuousThroughJunctions = false;
	// Its centre is white dashes (FRoadLaneMarkingBuilder), not the material's taxiway line.
	Profile->bMaterialCentreline = false;

	// A ROAD HAS NO EXITS TO GRADE. ExitLength is read only from a continuous profile (see
	// its own comment), so this is belt and braces - but a non-zero value here would be an
	// authored number that nothing reads, which is the failure this codebase has shipped
	// three times over.
	Profile->ExitLength = 0.0;
}
