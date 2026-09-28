#pragma once

#include "CoreMinimal.h"
#include "Model/Pavement.h"
#include "Model/RoadEntity.h"
#include "StandAdmission.generated.h"

struct FAirframe;
class URoadNetwork;

/** Why a stand may not admit an aircraft. None means it may. */
UENUM()
enum class EStandRefusal : uint8
{
	None,
	/** The pad is weaker than the aircraft's minimum. */
	Surface,
	/** IcaoCode::StandAdmits refuses it - too narrow a letter for the wingspan. */
	TooSmall,
	/** Surface and size both pass, but a service this aircraft needs cannot work on this
	 *  pavement. */
	Service,
	/** The stand sits inside a taxiway's clearance strip - drawn before the strip existed, or
	 *  its taxiway was upgraded since. Closed to NEW arrivals only; one already parked
	 *  finishes its turnaround (user, 2026-09-28). Appended, never inserted: a UENUM. */
	InsideStrip,
};

/**
 * One admission decision with the figures it was made from, so the sentence that explains
 * it can be written from the decision rather than by re-deriving it - FRunwayAdmission's own
 * rule, mirrored here because a stand's admission is the same three-step comparison over a
 * different fact set.
 *
 * A USTRUCT because FArrivalPlan already carries FRunwayAdmission the same way and is itself
 * a USTRUCT.
 */
USTRUCT()
struct AIRSIDE_API FStandAdmission
{
	GENERATED_BODY()

	UPROPERTY() EStandRefusal Why = EStandRefusal::None;

	/** The surface comparison - see FPavementCheck. Written by Judge whatever the verdict. */
	UPROPERTY() FPavementCheck Pavement;

	/** The stand's own design span and the aircraft's wingspan, uu - TooSmall's figures. */
	UPROPERTY() double StandDesignSpan = 0.0;
	UPROPERTY() double Wingspan = 0.0;

	/**
	 * Whether IcaoCode::StandAdmits alone would admit this stand, computed UNCONDITIONALLY -
	 * true whenever size alone admits, independent of Why. Lets an aggregate (ArrivalPlanner::
	 * WhyEveryStandRefused) ask "would paving fix this" without re-asking IcaoCode::StandAdmits
	 * itself, which the call-site rule (Check-Architecture rule 4 row 'IcaoCode::StandAdmits')
	 * confines to IcaoCode.cpp and this file.
	 */
	UPROPERTY() bool bPassesSize = false;

	/** Which service was refused, when Why == Service. Meaningless otherwise. */
	UPROPERTY() EServiceRole RefusedRole = EServiceRole::Aircraft;

	bool IsAdmitted() const { return Why == EStandRefusal::None; }
};

/**
 * May this aircraft park at this stand? Model/, no world - ChooseStand and UStandAllocator
 * both ask it, so the answer lives in one place rather than in two callers comparing
 * DesignWingspan and Wingspan by hand.
 *
 * Free functions over value types, RunwayAdmission's shape: there is no state to own between
 * calls, only a stand and an airframe to compare.
 *
 * Shares only FPavementCheck with RunwayAdmission (Task 2's Value Object comment on
 * FPavementCheck explains why that is a value object and not a Specification chain of rule
 * objects) - the rest of the comparison differs enough (letters, not lengths and widths;
 * a service role, not an approach category) that a shared base class would buy the two
 * nothing but a false resemblance.
 */
namespace StandAdmission
{
	/**
	 * The comparison itself, over a placed stand and an airframe.
	 *
	 * STRIP FIRST, THEN SURFACE, SIZE, SERVICE - the refusals are checked in that order, and the
	 * FIRST wins. A stand inside a taxiway's clearance strip refuses every aircraft, so no
	 * other reason may speak over it; its figures (bPassesSize, Pavement) are still written, so
	 * an aggregate can tell "redraw it back" from "it would not fit anyway".
	 *
	 * NETWORK because that strip belongs to the taxiway beside the stand, which the stand alone
	 * cannot know (strip spec 2026-09-28).
	 *
	 * Among the rest:
	 * a stand too small for a jet AND laid on grass is refused for its grass, because that is
	 * the fact a player cannot fix by drawing the stand bigger - RunwayAdmission's own reason,
	 * applied to a stand instead of a runway.
	 */
	AIRSIDE_API FStandAdmission Judge(const URoadNetwork& Network, const FEntityInstance& Stand, const FAirframe& Airframe);

	/** The sentence for a refusal: "the surface is grass; this aircraft needs concrete", or the
	 *  Code-letter or service equivalent. Empty when admitted. */
	AIRSIDE_API FString Describe(const FStandAdmission& Admission);

	/**
	 * May a service of Role work on a pad of P? Consumed by Judge, not by any caller directly,
	 * so restricting a role on grass later is one function body to change, not a new call
	 * site at every stand-admission caller.
	 * ENFORCED BY: Check-Architecture rule 4 row 'StandAdmission::PavementAdmitsRole'
	 *
	 * EVERY SERVICE WORKS ON EVERY PAVEMENT TODAY (ruling, user, 2026-09-27) - there is no
	 * rule yet that a fuel bowser needs concrete a baggage cart does not. PINNED by
	 * Airside.Model.StandAdmission.EveryRoleWorksOnEveryPavement, which goes red BY DESIGN
	 * the day a role is restricted - update THAT test with the ruling then, do not delete it.
	 */
	AIRSIDE_API bool PavementAdmitsRole(EPavement P, EServiceRole Role);
}
