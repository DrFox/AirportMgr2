#include "Model/TaxiwayRestriction.h"

#include "AirsideLog.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"

namespace TaxiwayRestriction
{
	namespace
	{
		double PavementWidthOf(const URoadNetwork& Network, FRoadSegmentId Id)
		{
			const FRoadSegment* Segment = Network.GetSegment(Id);
			const URoadProfile* Profile = Segment != nullptr ? Network.ProfileFor(*Segment) : nullptr;
			return Profile != nullptr ? Profile->GetTotalWidth() : 0.0;
		}
	}

	TOptional<EIcaoCode> RestrictionOf(const URoadNetwork& Network, FRoadSegmentId Taxiway, FObstruction* OutWorst)
	{
		if (!TaxiwayStrip::HasStrip(Network, Taxiway))
		{
			return TOptional<EIcaoCode>();
		}
		const double Pavement = PavementWidthOf(Network, Taxiway);
		const EIcaoCode Own = IcaoCode::TaxiwayLetterForWidth(Pavement);

		// DOWN FROM THE PAVEMENT'S LETTER, first clear wins. Six letters at most, and the common
		// case - a clear strip - is answered by the first. Stands are not counted (ruling 3).
		for (int32 Letter = static_cast<int32>(Own); Letter >= static_cast<int32>(EIcaoCode::A); --Letter)
		{
			const EIcaoCode Code = static_cast<EIcaoCode>(Letter);
			const TOptional<FObstruction> Hit = TaxiwayStrip::StripSwallows(Network, Taxiway,
				IcaoCode::TaxiwayStripFor(Code, Pavement), false);
			if (!Hit.IsSet())
			{
				return Code == Own ? TOptional<EIcaoCode>() : TOptional<EIcaoCode>(Code);
			}
			if (OutWorst != nullptr)
			{
				*OutWorst = Hit.GetValue();   // the last to block is the one just above the answer
			}
		}
		// NOT EVEN A's STRIP IS CLEAR - something is at the pavement's edge. A is the least a
		// taxiway can operate at; the inspector's reason says what is in the way.
		return EIcaoCode::A;
	}

	TOptional<EIcaoCode> EffectiveLetterOf(const URoadNetwork& Network, FRoadSegmentId Taxiway)
	{
		if (!TaxiwayStrip::HasStrip(Network, Taxiway))
		{
			return TOptional<EIcaoCode>();
		}
		const EIcaoCode Own = IcaoCode::TaxiwayLetterForWidth(PavementWidthOf(Network, Taxiway));
		const uint8 Stored = Network.GetSegment(Taxiway)->RestrictedLetter;
		// LOWERED, NEVER RAISED: a stored letter above the pavement's (a segment re-profiled
		// narrower since the last pass) cannot widen what the pavement admits.
		return Stored <= static_cast<uint8>(EIcaoCode::F) && Stored < static_cast<uint8>(Own)
			? static_cast<EIcaoCode>(Stored) : Own;
	}

	FString Describe(const FObstruction& Obstruction)
	{
		switch (Obstruction.Kind)
		{
		case FObstruction::EKind::Stand:   return FString::Printf(TEXT("stand %d"), Obstruction.Index);
		case FObstruction::EKind::Depot:   return TEXT("a fuel depot");
		case FObstruction::EKind::Taxiway: return TEXT("a taxiway");
		case FObstruction::EKind::Road:    return TEXT("a service road");
		}
		return TEXT("something");
	}

	int32 Apply(URoadNetwork& Network, bool bLog)
	{
		// EVERY LIVE SEGMENT, LINEARLY: RestrictionOf is up to six StripSwallows each, whose own
		// comment gives the cost and N (34 segments on M_Test, 2026-09-28). Topology rebuilds only.
		int32 Restricted = 0;
		const int32 Count = Network.GetSegments().Num();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FRoadSegmentId Id = Network.SegmentIdAt(Index);
			if (!Id.IsSet())
			{
				continue;
			}
			FObstruction Worst;
			const TOptional<EIcaoCode> Letter = RestrictionOf(Network, Id, &Worst);
			const uint8 Now = Letter.IsSet() ? static_cast<uint8>(Letter.GetValue()) : Unrestricted;
			const uint8 Was = Network.GetSegment(Id)->RestrictedLetter;
			if (Letter.IsSet())
			{
				++Restricted;
			}
			if (Now == Was)
			{
				continue;
			}
			Network.WriteSegmentRestriction(Id, Now);
			if (!bLog)
			{
				continue;
			}

			// ONCE PER CHANGE, the line to grep when "the A380 won't taxi there any more".
			const auto Name = [](uint8 Code) -> FString
			{
				return Code == Unrestricted ? FString(TEXT("unrestricted"))
					: FString::Printf(TEXT("Code %s"), IcaoCode::ToLetter(static_cast<EIcaoCode>(Code)));
			};
			if (Letter.IsSet())
			{
				UE_LOG(LogAirside, Log, TEXT("Restriction: taxiway %d -> %s (was %s), by %s %d"),
					Index, *Name(Now), *Name(Was), *Describe(Worst), Worst.Index);
			}
			else
			{
				UE_LOG(LogAirside, Log, TEXT("Restriction: taxiway %d -> %s (was %s)"), Index, *Name(Now), *Name(Was));
			}
		}
		return Restricted;
	}
}
