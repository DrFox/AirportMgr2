#include "Model/StandAdmission.h"

#include "Model/Airframe.h"
#include "Solve/IcaoCode.h"

namespace StandAdmission
{
	bool PavementAdmitsRole(EPavement P, EServiceRole Role)
	{
		// See the declaration's own comment: every role works on every pavement until a
		// ruling says otherwise, and this is the one place that changes when one does.
		return true;
	}

	FStandAdmission Judge(const FEntityInstance& Stand, const FAirframe& Airframe)
	{
		FStandAdmission Out;
		Out.Pavement = Pavement::Judge(Stand.Pavement, Airframe.MinimumPavement);
		Out.StandDesignSpan = Stand.DesignWingspan;
		Out.Wingspan = Airframe.Wingspan;

		// SURFACE, SIZE, SERVICE - first wins. Surface first for RunwayAdmission's reason: it is
		// the fact a player cannot fix by drawing the stand bigger.
		if (!Out.Pavement.Passes())
		{
			Out.Why = EStandRefusal::Surface;
		}
		// IcaoCode::StandAdmits CALLED, not re-implemented - its "unknown admits anything" and
		// "wider than F is never admitted" live there. ENFORCED BY: Check-Architecture rule 4 row
		// 'IcaoCode::StandAdmits'
		else if (!IcaoCode::StandAdmits(Stand.DesignWingspan, Airframe.Wingspan))
		{
			Out.Why = EStandRefusal::TooSmall;
		}
		else
		{
			// THE SERVICE HOOK (user, 2026-09-27): consumed here so restricting a role on grass
			// later is one function body, not a new call site. Never refuses today.
			for (const FResolvedAnchor& Anchor : Stand.ResolvedAnchors)
			{
				if (!PavementAdmitsRole(Stand.Pavement, Anchor.Role))
				{
					Out.Why = EStandRefusal::Service;
					Out.RefusedRole = Anchor.Role;
					break;
				}
			}
		}
		return Out;
	}

	FString Describe(const FStandAdmission& Admission)
	{
		switch (Admission.Why)
		{
		case EStandRefusal::Surface:
			return Pavement::Describe(Admission.Pavement);

		case EStandRefusal::TooSmall:
			return FString::Printf(TEXT("the stand is Code %s; this aircraft needs Code %s"),
				*IcaoCode::LetterForWingspan(Admission.StandDesignSpan),
				*IcaoCode::LetterForWingspan(Admission.Wingspan));

		case EStandRefusal::Service:
			return FString::Printf(TEXT("%s cannot work on %s"),
				*UEnum::GetDisplayValueAsText(Admission.RefusedRole).ToString(),
				Pavement::Name(Admission.Pavement.Have));

		case EStandRefusal::None:
		default:
			return FString();
		}
	}
}
