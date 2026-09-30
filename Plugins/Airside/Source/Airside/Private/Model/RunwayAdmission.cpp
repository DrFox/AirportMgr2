#include "Model/RunwayAdmission.h"

#include "Model/Airframe.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/RunwayQuery.h"
#include "Profiles/RoadProfile.h"
#include "Solve/IcaoCode.h"

// RunwayApproachName moved to RunwayFacts.cpp (#103) - declared in RunwayFacts.h beside the
// enum it names, which is where its definition belongs too. Pavement::Name and
// Pavement::MaterialSlot made the same move again, to Pavement.h/.cpp (2026-09-27; were
// RunwaySurfaceName in RunwayFacts.cpp and RunwayMaterialSlot per issue #105 item 4), when
// EPavement became the one scale for every buildable rather than a runway-only enum.

namespace RunwayAdmission
{
	double MaxWingspanForWidth(double TotalWidth)
	{
		// The table itself is Solve/IcaoCode.h now - shared with InspectFacts (wingspan ->
		// letter) and AnchorLink (letter -> stand radius), so the three no longer risk
		// typing the same ICAO Annex 14 rows in three different orderings. See #85.
		return IcaoCode::MaxWingspanForWidth(TotalWidth);
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
		Out.Pavement = Pavement::Judge(Facts.Surface, Airframe.MinimumPavement);

		// Both scales are ORDERED enums (see RunwayFacts.h), so "weaker than" is checked with
		// FPavementCheck::Passes for the surface and < for the approach.
		if (!Out.Pavement.Passes())
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
		FRunwayEnd End;
		if (const FRoadNode* A = Network.GetNode(Segment->A))
		{
			Network.RunwayExtentAt(A->Position, End);
		}
		const double Length = End.Length;

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

	FRunwayAdmission CheckArrival(const URoadNetwork& Network, FRoadSegmentId LandingSeed,
		const FAirframe& Airframe)
	{
		const FRunwayAdmission Landing = Check(Network, LandingSeed, Airframe, /*bLanding=*/true);
		if (!Landing.IsAdmitted())
		{
			return Landing;
		}

		// Every runway that TAKES DEPARTURES, not just the landing one and not every runway - see the header.
		// The list is RunwayQuery::DepartureRunways, the enumeration DeparturePlanner::PlanAny walks and the one
		// place CheckArrival and the planners read a use setting (the inspector, the card and the build bar read
		// it for display and editing, which is not a planning decision): this loop used to walk every strip
		// AirsideCapability found, an arrivals-only one included, so a departure it counted on such a strip was
		// one nothing would ever plan and the aircraft landed and stayed on its stand (#433). A copy of that
		// filter here would be the same bug the next time a rule is added to one side only.
		// ENFORCED BY: Check-Architecture.ps1 rule 39 (this file may not read a use setting or walk the runways
		// itself); Airside.Model.RunwayUse.EveryModePairLandsOnlyWhatCanLeave (every mode pair, against PlanAny).
		int32 RunwayCount = 0;
		const TArray<FRunwayEnd> Leaving = RunwayQuery::DepartureRunways(Network, &RunwayCount);
		FRunwayAdmission Longest;
		bool bAny = false;
		for (const FRunwayEnd& Runway : Leaving)
		{
			const FRunwayAdmission Departure = Check(Network, Runway.Seed, Airframe, /*bLanding=*/false);
			if (Departure.IsAdmitted())
			{
				return Landing;
			}
			if (!bAny || Departure.RunwayLength > Longest.RunwayLength)
			{
				Longest = Departure;
				bAny = true;
			}
		}
		if (!bAny)
		{
			// RUNWAYS, BUT NONE TAKES A DEPARTURE: every one is set to arrivals only - the player's setting,
			// said as such (Describe), with no strip's figures because no strip was judged.
			if (RunwayCount > 0)
			{
				FRunwayAdmission NoDeparture;
				NoDeparture.Why = ERunwayRefusal::NoDepartureRunway;
				NoDeparture.bForDeparture = true;
				return NoDeparture;
			}
			// No runway at all is unreachable with a runway to land on, which is itself a runway - kept
			// rather than asserted, so a network the summary cannot see still answers with the landing.
			return Landing;
		}
		Longest.bForDeparture = true;
		return Longest;
	}

	FString Describe(const FRunwayAdmission& Admission)
	{
		// SAID AS THE DEPARTURE when it is one: "the runway is 40366 uu" alone would read as a
		// landing refusal, and the landing fitted.
		if (Admission.bForDeparture && !Admission.IsAdmitted())
		{
			FRunwayAdmission AsDeparture = Admission;
			AsDeparture.bForDeparture = false;
			return FString::Printf(TEXT("it could not take off again - %s"), *Describe(AsDeparture));
		}

		switch (Admission.Why)
		{
		case ERunwayRefusal::Surface:
			return Pavement::Describe(Admission.Pavement);

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

		case ERunwayRefusal::NoDepartureRunway:
			return TEXT("every runway is set to arrivals only - set one to departures or mixed");

		case ERunwayRefusal::None:
		default:
			return FString();
		}
	}
}
