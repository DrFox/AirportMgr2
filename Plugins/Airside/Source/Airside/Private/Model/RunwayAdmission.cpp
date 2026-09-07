#include "Model/RunwayAdmission.h"

#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

const TCHAR* RunwaySurfaceName(ERunwaySurface Surface)
{
	switch (Surface)
	{
	case ERunwaySurface::Grass:      return TEXT("grass");
	case ERunwaySurface::Tarmac:     return TEXT("tarmac");
	case ERunwaySurface::Concrete:   return TEXT("concrete");
	case ERunwaySurface::Reinforced: return TEXT("reinforced");
	}
	return TEXT("unknown");
}

const TCHAR* RunwayApproachName(ERunwayApproach Approach)
{
	switch (Approach)
	{
	case ERunwayApproach::Visual:       return TEXT("visual");
	case ERunwayApproach::NonPrecision: return TEXT("non-precision");
	case ERunwayApproach::Precision:    return TEXT("precision");
	}
	return TEXT("unknown");
}

namespace RunwayAdmission
{
	double MaxWingspanForWidth(double TotalWidth)
	{
		// ICAO Annex 14 Table 1-1, the code letter each runway width is built for, in uu.
		// A table rather than a formula because the relation is a standard, not a curve:
		// a 45 m runway serves both D (52 m) and E (65 m) and the wider figure is the one
		// the width was chosen for.
		struct FCode { double Width; double Wingspan; };
		static const FCode Codes[] = {
			{ 1800.0, 1500.0 },   // A
			{ 2300.0, 2400.0 },   // B
			{ 3000.0, 3600.0 },   // C
			{ 4500.0, 6500.0 },   // D/E
			{ 6000.0, 8000.0 },   // F
		};
		const FCode* Nearest = &Codes[0];
		for (const FCode& Code : Codes)
		{
			if (FMath::Abs(Code.Width - TotalWidth) < FMath::Abs(Nearest->Width - TotalWidth))
			{
				Nearest = &Code;
			}
		}
		return Nearest->Wingspan;
	}

	FRunwayAdmission Judge(const FRunwayFacts& Facts, double RunwayLength, double MaxWingspan,
		const FAirframe& Airframe, bool bLanding)
	{
		FRunwayAdmission Out;
		Out.Facts = Facts;
		Out.Required = Airframe.Requirements;
		Out.RunwayLength = RunwayLength;
		Out.FieldLength = bLanding ? Airframe.Requirements.LandingFieldLength
			: Airframe.Requirements.TakeoffFieldLength;
		Out.Wingspan = Airframe.Wingspan;
		Out.MaxWingspan = MaxWingspan;

		// Both scales are ORDERED enums (see RunwayFacts.h), so "weaker than" is <.
		if (Facts.Surface < Airframe.Requirements.MinimumSurface)
		{
			Out.Why = ERunwayRefusal::Surface;
		}
		else if (Facts.Approach < Airframe.Requirements.ApproachNeeded)
		{
			Out.Why = ERunwayRefusal::Approach;
		}
		// A field length of 0 is "no claim" - an aircraft type authored before the
		// requirements existed - and makes no length refusal; the planners' own
		// RunwayTooShort backstop still measures the physics for it.
		else if (Out.FieldLength > 0.0 && RunwayLength < Out.FieldLength)
		{
			Out.Why = ERunwayRefusal::TooShort;
		}
		else if (MaxWingspan > 0.0 && Airframe.Wingspan > MaxWingspan)
		{
			Out.Why = ERunwayRefusal::TooNarrow;
		}
		return Out;
	}

	FRunwayAdmission Check(const URoadNetwork& Network, FRoadSegmentId Seed, const FAirframe& Airframe,
		bool bLanding)
	{
		const FRoadSegment* Segment = Network.GetSegment(Seed);
		if (Segment == nullptr || !Network.IsRunwaySegment(Seed))
		{
			// Admitted, deliberately: "not a runway" is the planners' NoRunway and they
			// have already asked it. Refusing here would make a taxiway read as a runway
			// with the wrong surface.
			return FRunwayAdmission();
		}

		// The chain's length, not the seed's: the same walk every runway query makes,
		// asked from the seed's own A node so it is certainly on the strip.
		double Length = 0.0;
		FVector2D Threshold, Direction;
		if (const FRoadNode* A = Network.GetNode(Segment->A))
		{
			Network.RunwayExtentAt(A->Position, Threshold, Direction, Length);
		}

		double MaxWingspan = 0.0;
		if (const URoadProfile* Profile = Network.ProfileFor(*Segment))
		{
			// The profile's own declared limit first - it is the same figure the route
			// search refuses a taxi turn by - and the code-letter table only where the
			// profile makes no claim, which is every runway profile authored so far.
			if (Profile->Guidelines.Num() > 0 && Profile->Guidelines[0].MaxWingspan > 0.0)
			{
				MaxWingspan = Profile->Guidelines[0].MaxWingspan;
			}
			else
			{
				MaxWingspan = MaxWingspanForWidth(Profile->GetTotalWidth());
			}
		}

		return Judge(Network.RunwayFactsFor(Seed), Length, MaxWingspan, Airframe, bLanding);
	}

	FString Describe(const FRunwayAdmission& Admission)
	{
		switch (Admission.Why)
		{
		case ERunwayRefusal::Surface:
			return FString::Printf(TEXT("the surface is %s; this aircraft needs %s"),
				RunwaySurfaceName(Admission.Facts.Surface),
				RunwaySurfaceName(Admission.Required.MinimumSurface));

		case ERunwayRefusal::Approach:
			return FString::Printf(TEXT("the approach is %s; this aircraft needs %s"),
				RunwayApproachName(Admission.Facts.Approach),
				RunwayApproachName(Admission.Required.ApproachNeeded));

		case ERunwayRefusal::TooShort:
			return FString::Printf(TEXT("the runway is %.0f uu; this aircraft's field length is %.0f"),
				Admission.RunwayLength, Admission.FieldLength);

		case ERunwayRefusal::TooNarrow:
			return FString::Printf(TEXT("the runway admits a %.0f uu wingspan; this aircraft's is %.0f"),
				Admission.MaxWingspan, Admission.Wingspan);

		case ERunwayRefusal::None:
		default:
			return FString();
		}
	}
}
