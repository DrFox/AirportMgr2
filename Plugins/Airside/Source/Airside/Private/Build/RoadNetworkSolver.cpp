#include "Build/RoadNetworkSolver.h"

#include "Build/BendWidening.h"
#include "Build/ExitGeometry.h"

#include "Model/Chassis.h"
#include "Model/Vehicle.h"
#include "Model/VehicleFit.h"
#include "Solve/VehicleSweep.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadDesignVehicles.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/JunctionSolver.h"
#include "Solve/RoadGeom.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoadSolve, Log, All);

namespace
{
	/** Straight-line distance between a segment's endpoints. */
	double SegmentChordLength(const URoadNetwork& Network, const FRoadSegment& Segment)
	{
		const FRoadNode* A = Network.GetNode(Segment.A);
		const FRoadNode* B = Network.GetNode(Segment.B);
		if (A == nullptr || B == nullptr)
		{
			return 0.0;
		}
		return FVector2D::Distance(A->Position, B->Position);
	}
}

namespace
{
	// SlackShare moved to FRoadNetworkSolver (public) with its comment, so the builder's
	// capped-taper warning can say how long a segment must be from the same figure.
	constexpr double SlackShare = FRoadNetworkSolver::SlackShare;

	/**
	 * A tenth of a percent over the design vehicle's lock, so the builder's re-derivation of the
	 * S from the cut lines (atan, sin, a lerp of cut vertices) can never land its radius a
	 * rounding under the lock VehicleFit compares it with. Not a design margin: the taper IS
	 * sized for the lock.
	 */
	constexpr double TaperRadiusMargin = 1.001;

	/**
	 * THE WIDTH TAPER'S LENGTH, uu, at a straight-through node of two arms (user ruling
	 * 2026-09-25: "inset nodes and a curve between the two widths"), or zero when the lanes do
	 * not move. OutShift is the lateral step of the lane that moves furthest.
	 *
	 * Where a Narrow road meets a Wide one head on, each lane's offset steps (a quarter of the
	 * width difference on a two-lane road: 75 uu Narrow -> Wide). Both cuts at the node, the
	 * turn path between the lane ends was that step laid across NOTHING - a 90 degree jog,
	 * MinRadius 0, crawled at steering speed. So both arms are cut back half this length and
	 * the lane crosses the gap on an S: GuidelineGeom's own lane-change construction
	 * (ShiftDeflectionFor, the one a stand's link to a parallel road uses), two quadratics of
	 * tangent s deflecting by b, so the transition runs L = 2 s (1 + cos b) along and 2 s sin b
	 * across. NOT the circular reverse curve's L = sqrt(4 R d - d^2): the lanes are quadratics,
	 * and sizing them from a circle would be a second evaluator of the curve that is driven.
	 * Worked, 2026-09-25: d 75 uu, R 576 (the rig, Wide's design vehicle) gives b 20.6 deg,
	 * s 107, L 412 uu - against sqrt(4 R d - d^2) = 409 for the circle.
	 *
	 * THE TRIGGER IS A LANE OFFSET, NOT A WIDTH DIFFERENCE - intended (review of 5660420c): a
	 * same-width profile change with its lanes at other offsets, and a one-lane bidirectional
	 * road meeting a two-lane one, step the line just the same and taper just the same.
	 *
	 * THE WIDER ARM'S DESIGN VEHICLE sizes it (the brief's ruling): the taper is part of the wide
	 * road, and a rig admitted to the Wide road must be able to follow onto it. A Narrow ->
	 * Standard taper is therefore the bowser's, and the rig is refused there on its lock - where
	 * before it was waved through the unmeasured jog and crawled.
	 *
	 * Lane positions from each profile's own offsets, in one frame across the node. Exact for a
	 * symmetric profile, which every road profile is (a lane's cut-line point is its offset
	 * along the arm's left); an asymmetric one would place its lanes a little off here and the
	 * S would be sized for that - not wrong, since the builder lays it from where the ends are.
	 */
	double WidthTaperLength(const URoadNetwork& Network, FRoadNodeId NodeId, const FJunctionInput& Input,
		const TArray<FRoadSegmentId>& ArmSegments, const FRoadDesignVehicles* DesignVehicles, double& OutShift)
	{
		OutShift = 0.0;
		if (Input.Arms.Num() != 2 || ArmSegments.Num() != 2)
		{
			return 0.0;
		}
		const FVector2D Across = RoadGeom::PerpCCW(Input.Arms[0].Tangent);
		TArray<double> Lanes[2];
		const URoadProfile* Profiles[2] = { nullptr, nullptr };
		for (int32 Arm = 0; Arm < 2; ++Arm)
		{
			const FRoadSegment* Segment = Network.GetSegment(ArmSegments[Arm]);
			Profiles[Arm] = Segment != nullptr ? Network.ProfileFor(*Segment) : nullptr;
			if (Profiles[Arm] == nullptr)
			{
				return 0.0;
			}
			// A guideline's offset is to the left of its segment's A->B, whichever end this is.
			const FVector2D AToB = Segment->A == NodeId ? Input.Arms[Arm].Tangent : -Input.Arms[Arm].Tangent;
			const double Sign = FVector2D::DotProduct(RoadGeom::PerpCCW(AToB), Across) >= 0.0 ? 1.0 : -1.0;
			for (const FProfileGuideline& Guideline : Profiles[Arm]->Guidelines)
			{
				Lanes[Arm].Add(Sign * Guideline.OffsetFor(Network.GetDriveSide()));
			}
		}
		// Each lane pairs with the nearest lane across the node - the one the builder's turn path
		// joins it to on a straight-through road.
		for (const double From : Lanes[0])
		{
			double Nearest = TNumericLimits<double>::Max();
			for (const double To : Lanes[1])
			{
				Nearest = FMath::Min(Nearest, FMath::Abs(From - To));
			}
			if (Nearest < TNumericLimits<double>::Max())
			{
				OutShift = FMath::Max(OutShift, Nearest);
			}
		}
		// Under a uu the lanes meet: the ordinary zero-length through-turn, no taper.
		if (OutShift <= 1.0)
		{
			OutShift = 0.0;
			return 0.0;
		}
		const URoadProfile* Wider = Profiles[1]->GetTotalWidth() > Profiles[0]->GetTotalWidth() ? Profiles[1] : Profiles[0];
		const double Radius = DesignVehicles != nullptr
			? DesignVehicles->For(Wider).TightestFollowableRadius()
			: Wider->ResolvedDesignRadius();
		return GuidelineGeom::LaneChangeLength(Radius * TaperRadiusMargin, OutShift);
	}

	/**
	 * One arm per live incident segment, in incidence order, with the profile's own widths
	 * and preferred radius. The ONE place a node's junction input is assembled, so the solve
	 * proper and the zero-radius floor query cannot describe the same node differently.
	 */
	bool BuildNodeInput(const URoadNetwork& Network, int32 NodeIndex, int32 ArcSegments,
		FJunctionInput& OutInput, TArray<FRoadSegmentId>& OutArmSegments,
		const FRoadDesignVehicles* DesignVehicles)
	{
		const TArray<FRoadNode>& Nodes = Network.GetNodes();
		if (!Nodes.IsValidIndex(NodeIndex))
		{
			return false;
		}
		const FRoadNode& Node = Nodes[NodeIndex];
		if (!Node.bAlive || Node.Incident.Num() == 0)
		{
			return false;
		}
		// Network.NodeIdAt, not a hand-built handle (#79, #173).
		const FRoadNodeId NodeId = Network.NodeIdAt(NodeIndex);

		OutInput.Position = Node.Position;
		OutInput.ArcSegments = ArcSegments;
		for (const FRoadSegmentId SegmentId : Node.Incident)
		{
			const FRoadSegment* Segment = Network.GetSegment(SegmentId);
			if (Segment == nullptr)
			{
				continue;
			}
			const URoadProfile* Profile = Network.ProfileFor(*Segment);

			FJunctionArm Arm;
			Arm.Tangent = Network.GetOutgoingTangent(SegmentId, NodeId);
			Arm.HalfWidthLeft  = Profile ? Profile->GetHalfWidthLeft()  : 0.0;
			Arm.HalfWidthRight = Profile ? Profile->GetHalfWidthRight() : 0.0;
			// ISSUE #190: the passed-down vehicle when the caller has one, the profile's own
			// self-resolving overload otherwise - see BuildNodeInput's caller-supplied
			// DesignVehicles and URoadProfile::ResolvedFilletRadius's two overloads.
			// PER ARM, PER TIER (2026-09-25): each arm's fillet from its own profile's design
			// vehicle, so a Wide road's corner is laid for the rig; a corner between two arms takes
			// the smaller of their fillets (FJunctionSolver), so it is the rig's only when both are.
			Arm.FilletRadius = Profile
				? (DesignVehicles ? Profile->ResolvedFilletRadius(DesignVehicles->For(Profile))
				                  : Profile->ResolvedFilletRadius())
				: 0.0;
			// A runway passes through: never trimmed, never filleted. See FJunctionArm.
			Arm.bContinuous    = Profile ? Profile->bContinuousThroughJunctions : false;
			Arm.UserData = SegmentId.Index;
			OutInput.Arms.Add(Arm);
			OutArmSegments.Add(SegmentId);
		}

		// THE WIDTH TAPER: both arms of a straight-through node inset by half of it - see
	// WidthTaperLength. Continuous arms (a runway) are never cut, so never tapered.
	if (OutInput.Arms.Num() == 2 && !OutInput.Arms[0].bContinuous && !OutInput.Arms[1].bContinuous
		&& RoadGeom::IsStraightThrough(RoadGeom::AngleBetween(OutInput.Arms[0].Tangent, OutInput.Arms[1].Tangent)))
	{
		double Shift = 0.0;
		const double Taper = WidthTaperLength(Network, NodeId, OutInput, OutArmSegments, DesignVehicles, Shift);
		for (FJunctionArm& Arm : OutInput.Arms)
		{
			Arm.MinCutDistance = 0.5 * Taper;
		}
	}

	// THE FLARE. At a node where a taxiway meets a runway, the corner an exit arc sweeps
		// through gets a fillet that follows the arc - a taxiway's half width inside it -
		// so the aircraft's wheels and wing are on pavement through the turn, not only its
		// centreline (samples/runway2.png, 2026-09-07). ExitGeometry decides the arc's
		// tangent length once for the solver and the guideline builder both; the formula
		// gives less than the default on the acute corner and the flare on the obtuse one,
		// so every corner between a runway arm and a taxiway arm is asked and the larger of
		// the two answers stands. Arms are CCW-sorted, so corner i is the wedge from arm i
		// to arm i + 1.
		const double ExitLength = ExitGeometry::NodeExitLength(Network, NodeIndex, OutArmSegments);
		if (ExitLength > 0.0 && OutInput.Arms.Num() >= 2)
		{
			const int32 Count = OutInput.Arms.Num();
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FJunctionArm& Arm = OutInput.Arms[Index];
				const FJunctionArm& Next = OutInput.Arms[(Index + 1) % Count];
				if (Arm.bContinuous == Next.bContinuous)
				{
					continue;
				}
				const FJunctionArm& Taxiway = Arm.bContinuous ? Next : Arm;
				const double TaxiwayHalfWidth = 0.5 * (FMath::Max(Taxiway.HalfWidthLeft, 0.0) + FMath::Max(Taxiway.HalfWidthRight, 0.0));
				double Corner = FMath::Atan2(Next.Tangent.Y, Next.Tangent.X) - FMath::Atan2(Arm.Tangent.Y, Arm.Tangent.X);
				while (Corner < 0.0) { Corner += 2.0 * PI; }
				while (Corner >= 2.0 * PI) { Corner -= 2.0 * PI; }
				// Every mixed corner is named: the flare where the arc asks for more than the
				// default, and a small kerb where it asks for less - see AcuteCornerRadius for
				// why the default is not left standing on the acute side.
				const double Flare = ExitGeometry::FlareRadius(ExitLength, Corner, TaxiwayHalfWidth);
				OutInput.Arms[Index].FilletRadiusToNext = FMath::Max(Flare, ExitGeometry::AcuteCornerRadius);
			}
		}
		return OutInput.Arms.Num() > 0;
	}
}

double FRoadNetworkSolver::ZeroRadiusCut(const URoadNetwork& Network, FRoadSegmentId Segment, FRoadNodeId AtNode,
	const FRoadDesignVehicles* DesignVehicles)
{
	FJunctionInput Input;
	TArray<FRoadSegmentId> ArmSegments;
	if (!BuildNodeInput(Network, AtNode.Index, 4, Input, ArmSegments, DesignVehicles) || Input.Arms.Num() == 1)
	{
		return 0.0;   // no node, or a dead end - whose cap shrinks rather than holding a floor
	}
	for (FJunctionArm& Arm : Input.Arms)
	{
		Arm.FilletRadius = 0.0;
		Arm.FilletRadiusToNext = 0.0;
		// The floor is the CORNER's: a taper is fitted into the slack like a fillet, never
		// allowed to fail a segment that is merely short (see SolveCuts' cap).
		Arm.MinCutDistance = 0.0;
	}
	const FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
	if (!Result.bValid)
	{
		return 0.0;
	}
	const int32 ArmIndex = ArmSegments.IndexOfByKey(Segment);
	return Result.Arms.IsValidIndex(ArmIndex) ? Result.Arms[ArmIndex].CutDistance : 0.0;
}

namespace
{
/**
 * SolveNodeCuts before the bend widening: the arms gathered, every corner's fillet fitted to the
 * arms' allowance. What SolveNodeCuts was until 2026-09-25, moved here unchanged.
 */
bool FitNodeCuts(const URoadNetwork& Network, int32 NodeIndex,
	int32 ArcSegments, FRoadNodeCuts& Out, const FRoadDesignVehicles* DesignVehicles)
{
	const TArray<FRoadNode>& Nodes = Network.GetNodes();
	if (!Nodes.IsValidIndex(NodeIndex))
	{
		return false;
	}

	const FRoadNode& Node = Nodes[NodeIndex];
	if (!Node.bAlive || Node.Incident.Num() == 0)
	{
		return false;
	}

	// Network.NodeIdAt, not a hand-built handle (#79, #173).
	const FRoadNodeId NodeId = Network.NodeIdAt(NodeIndex);

	// Incident is maintained sorted by CCW bearing, which is exactly what
	// FJunctionSolver requires. Do not re-sort here.
	Out.Input = FJunctionInput();
	Out.ArmSegments.Reset();
	if (!BuildNodeInput(Network, NodeIndex, ArcSegments, Out.Input, Out.ArmSegments, DesignVehicles))
	{
		return false;
	}

	TArray<double> PreferredRadii;
	TArray<double> PreferredCornerRadii;
	for (const FJunctionArm& Arm : Out.Input.Arms)
	{
		PreferredRadii.Add(Arm.FilletRadius);
		PreferredCornerRadii.Add(Arm.FilletRadiusToNext);
	}

	// THE ALLOWANCE IS SET BY BOTH ENDS. A segment holds its two cuts only if their sum is
	// under its length. Each end is solved on its own and cannot see the other's fillet, so
	// each is given its own zero-radius floor (the inner corner, or a dead end's cap) plus
	// HALF the slack the segment has left once both floors are paid for. Two ends that each
	// stay inside that can never cross. A flat fraction of the length was tried first and
	// refused honest roads: a 2300-wide L bend with 2500 arms needs 1150 at the corner and
	// 1150 at the cap, which fits, and a 45% cap said it did not.
	FJunctionInput ZeroInput = Out.Input;
	for (FJunctionArm& Arm : ZeroInput.Arms)
	{
		Arm.FilletRadius = 0.0;
		Arm.FilletRadiusToNext = 0.0;
		Arm.MinCutDistance = 0.0;   // as in ZeroRadiusCut: a taper takes slack, it is not a floor
	}
	const FJunctionResult ZeroHere = FJunctionSolver::SolveCuts(ZeroInput);
	if (!ZeroHere.bValid)
	{
		// Nothing fits at any radius; the fit below reports the same and fails the node.
		Out.Result = ZeroHere;
		return true;
	}

	TArray<double> ArmAllowance;
	for (int32 ArmIndex = 0; ArmIndex < Out.Input.Arms.Num(); ++ArmIndex)
	{
		const FRoadSegmentId SegmentId = Out.ArmSegments[ArmIndex];
		const FRoadSegment* Segment = Network.GetSegment(SegmentId);
		const double Length = Segment ? SegmentChordLength(Network, *Segment) : 0.0;
		// A dead end's floor is ZERO: its cap has no corner to respect and is drawn shorter
		// when the segment is short (FJunctionArm::MaxCutDistance). Only a corner has a floor.
		const double MinHere = Out.Input.Arms.Num() == 1 ? 0.0 : ZeroHere.Arms[ArmIndex].CutDistance;
		const double MinFar = Segment
			? FRoadNetworkSolver::ZeroRadiusCut(Network, SegmentId, Network.GetOtherEnd(SegmentId, NodeId), DesignVehicles)
			: 0.0;
		const double Slack = Length - MinHere - MinFar;
		if (Slack < 0.0)
		{
			// Even with no fillet at either end the two cuts cross: the segment is shorter
			// than its own width and corners need. The node FAILS - its segments are then
			// not drawn from this end rather than drawn folded and facing down (the road
			// that vanished, 2026-09-06). RoadPlacement refuses this before it exists, so
			// this line means a load, a heal or a drag got past that rule.
			UE_LOG(LogRoadSolve, Warning,
				TEXT("Node %d: segment %d is %.0f uu long but its two corners need %.0f + %.0f even ")
				TEXT("with no fillet. Draw it longer, widen the angle, or use a narrower profile."),
				NodeIndex, SegmentId.Index, Length, MinHere, MinFar);
			Out.Result.bValid = false;
			return true;
		}
		ArmAllowance.Add(MinHere + SlackShare * Slack);
	}
	for (int32 ArmIndex = 0; ArmIndex < Out.Input.Arms.Num(); ++ArmIndex)
	{
		Out.Input.Arms[ArmIndex].MaxCutDistance = ArmAllowance[ArmIndex];
	}

	// FIT THE RADII EXACTLY, in at most three solves. A cut distance is Reach + R * cot(Theta/2):
	// a constant part (where the inner edges meet) plus a part proportional to the radius.
	// The old loop divided the radii by the overshoot ratio and hoped, which is right only
	// when the constant part is zero; for a tight corner it ran out of attempts still
	// overshooting and said nothing, and at radius zero it warned and then carried on - both
	// emitted a folded ribbon, which is the road that vanished (2026-09-06). Now: solve at
	// the preferred radii; if any arm overshoots, solve at zero radius to learn the constant
	// part; if even that overshoots, the node FAILS - nothing this solver can do - otherwise
	// interpolate the scale that lands every arm inside its allowance and solve once more.
	auto SolveAtScale = [&](double Scale)
	{
		for (int32 ArmIndex = 0; ArmIndex < Out.Input.Arms.Num(); ++ArmIndex)
		{
			Out.Input.Arms[ArmIndex].FilletRadius = PreferredRadii[ArmIndex] * Scale;
			// The flare scales with the rest: a corner that does not fit its arms shrinks
			// as a whole, and a flare that stayed at full size would overshoot alone.
			Out.Input.Arms[ArmIndex].FilletRadiusToNext = PreferredCornerRadii[ArmIndex] * Scale;
		}
		Out.Result = FJunctionSolver::SolveCuts(Out.Input);
	};
	auto WorstOvershoot = [&]()
	{
		double Worst = 0.0;
		for (int32 ArmIndex = 0; ArmIndex < Out.Result.Arms.Num(); ++ArmIndex)
		{
			if (ArmAllowance[ArmIndex] > 0.0)
			{
				Worst = FMath::Max(Worst, Out.Result.Arms[ArmIndex].CutDistance / ArmAllowance[ArmIndex]);
			}
		}
		return Worst;
	};

	SolveAtScale(1.0);
	if (!Out.Result.bValid || WorstOvershoot() <= 1.0)
	{
		return true;
	}
	TArray<double> CutAtFull;
	for (const FJunctionArmResult& Arm : Out.Result.Arms) { CutAtFull.Add(Arm.CutDistance); }

	SolveAtScale(0.0);
	if (!Out.Result.bValid)
	{
		return true;
	}
	if (WorstOvershoot() > 1.0)
	{
		// Unreachable by construction - every allowance is at least this node's own
		// zero-radius cut - and kept as the last line of defence, saying so.
		UE_LOG(LogRoadSolve, Warning,
			TEXT("Node %d: zero-radius cut overshoots its allowance by %.0f%%; node failed"),
			NodeIndex, (WorstOvershoot() - 1.0) * 100.0);
		Out.Result.bValid = false;
		return true;
	}

	// Cut(s) is convex piecewise-linear in the scale (each arm takes the max over its two
	// corners, each linear), so the chord between Cut(0) and Cut(1) bounds it from above and
	// the chord's crossing of the allowance is a scale that fits, with a hair to spare.
	double Scale = 1.0;
	for (int32 ArmIndex = 0; ArmIndex < Out.Result.Arms.Num(); ++ArmIndex)
	{
		const double Allowance = ArmAllowance[ArmIndex];
		const double CutZero = Out.Result.Arms[ArmIndex].CutDistance;
		const double Rise = CutAtFull[ArmIndex] - CutZero;
		if (Allowance > 0.0 && Rise > 1e-9 && CutAtFull[ArmIndex] > Allowance)
		{
			Scale = FMath::Min(Scale, (Allowance - CutZero) / Rise);
		}
	}
	SolveAtScale(FMath::Clamp(Scale * 0.999, 0.0, 1.0));
	if (Out.Result.bValid && WorstOvershoot() > 1.0 + 1e-6)
	{
		// The bound above should make this unreachable; if it is ever reached, say so rather
		// than let a folded ribbon out.
		UE_LOG(LogRoadSolve, Warning, TEXT("Node %d: radius fit did not converge (overshoot %.3f); node failed"),
			NodeIndex, WorstOvershoot());
		Out.Result.bValid = false;
	}

	return true;
}

/**
 * A bend's widening as last TRACED, keyed by everything the trace reads but the vehicle: the node,
 * each arm's fitted geometry (tangent, widths, fillet, cut floor and allowance) and profile, and the
 * drive side. EWideningTrace's cache: a Topology rebuild writes it, every other solve reads it.
 */
struct FWideningKey
{
	TArray<double> Numbers;
	TArray<const void*> Profiles;

	bool operator==(const FWideningKey& Other) const { return Numbers == Other.Numbers && Profiles == Other.Profiles; }
};

uint32 GetTypeHash(const FWideningKey& Key)
{
	return HashCombine(FCrc::MemCrc32(Key.Numbers.GetData(), Key.Numbers.Num() * sizeof(double)),
		FCrc::MemCrc32(Key.Profiles.GetData(), Key.Profiles.Num() * sizeof(const void*)));
}

struct FCachedWidening
{
	bool bWidens = false;
	BendWidening::FWidening Widening;
	/** The design vehicle it was traced for, as VehicleSweep sees it - a changed vehicle re-traces on the next rebuild. */
	uint32 BodyDigest = 0;
};

/**
 * THE CACHE. Game-thread state, like every solve; bounded, since a key is a geometry and a drag
 * mints new ones - emptied past this many entries (a network's bends were a few dozen on
 * 2026-09-25), and the next Topology rebuild refills what is live.
 */
constexpr int32 MaxCachedWidenings = 4096;

TMap<FWideningKey, FCachedWidening>& WideningCache()
{
	static TMap<FWideningKey, FCachedWidening> Cache;
	return Cache;
}

uint32 BodyDigestOf(const FVehicle& Vehicle)
{
	const VehicleSweep::FBody Body = VehicleFit::BodyOf(Vehicle);
	TArray<double> Numbers = { Body.Wheelbase, Body.Width, Body.FrontX, Body.RearX, Vehicle.Chassis.TightestFollowableRadius() };
	for (const VehicleSweep::FLink& Link : Body.Tow)
	{
		Numbers.Append({ Link.HitchX, Link.Length, Link.BodyFront, Link.BodyRear, Link.Width });
	}
	return FCrc::MemCrc32(Numbers.GetData(), Numbers.Num() * sizeof(double));
}

/**
 * THE BEND'S INSIDE, WIDENED to what its design vehicle sweeps (BendWidening, user ruling
 * 2026-09-25), on a fitted two-arm bend whose service-road lanes the builder lays concentric.
 * The corner's design vehicle is the less demanding of the arms' - the one its fillet was taken
 * for (FJunctionSolver takes the smaller radius) - so a Wide bend is widened for the rig and a
 * Narrow or Standard one for the bowser, which leaves those tarmacs on no bend (measured).
 *
 * TRACED ONLY WHEN Widening SAYS SO (review of 75d3cbc0): a Topology rebuild traces what the
 * cache has not seen; the snap's queries and a drag frame's solves read it, and a bend it has not
 * seen - one the drag is reshaping - is laid unwidened until the rebuild that ends the drag. They
 * resolve no vehicle either: the self-resolving body lookup (#190) is the trace's alone.
 * ENFORCED BY: Airside.Build.BendLanes.WideBendCarriesTheRig, .WideningOnlyWhereNeeded, .SnapAndDragDoNotTrace
 */
void WidenBend(const URoadNetwork& Network, int32 NodeIndex, FRoadNodeCuts& Out, const FRoadDesignVehicles* DesignVehicles,
	EWideningTrace Mode)
{
	if (!Out.Result.bValid || Out.Input.Arms.Num() != 2 || Out.ArmSegments.Num() != 2)
	{
		return;
	}
	const FRoadNodeId NodeId = Network.NodeIdAt(NodeIndex);
	const URoadProfile* Profiles[2] = { nullptr, nullptr };
	FWideningKey Key;
	Key.Numbers = { Out.Input.Position.X, Out.Input.Position.Y, static_cast<double>(Network.GetDriveSide()) };
	for (int32 Arm = 0; Arm < 2; ++Arm)
	{
		const FRoadSegment* Segment = Network.GetSegment(Out.ArmSegments[Arm]);
		Profiles[Arm] = Segment != nullptr ? Network.ProfileFor(*Segment) : nullptr;
		if (Profiles[Arm] == nullptr)
		{
			return;
		}
		const FJunctionArm& In = Out.Input.Arms[Arm];
		Key.Numbers.Append({ In.Tangent.X, In.Tangent.Y, In.HalfWidthLeft, In.HalfWidthRight, In.FilletRadius,
			In.FilletRadiusToNext, In.MinCutDistance, In.MaxCutDistance, In.bContinuous ? 1.0 : 0.0,
			Segment->A == NodeId ? 1.0 : 0.0 });
		Key.Profiles.Add(Profiles[Arm]);
	}

	FCachedWidening* Cached = WideningCache().Find(Key);
	if (Mode == EWideningTrace::Trace)
	{
		TArray<BendWidening::FLane> Lanes[2];
		for (int32 Arm = 0; Arm < 2; ++Arm)
		{
			const bool bAtA = Network.GetSegment(Out.ArmSegments[Arm])->A == NodeId;
			for (const FProfileGuideline& Guideline : Profiles[Arm]->Guidelines)
			{
				// Service-road lanes only: the builder lays a taxiway's corner as it always did.
				if (Guideline.Class != ETraversalClass::GroundVehicle)
				{
					continue;
				}
				// A guideline's offset is to the left of its segment's A->B; the arm's frame is
				// outgoing from this node, which is B->A at end B.
				BendWidening::FLane& Lane = Lanes[Arm].AddDefaulted_GetRef();
				Lane.Lateral = (bAtA ? 1.0 : -1.0) * Guideline.OffsetFor(Network.GetDriveSide());
				Lane.bArrives = Guideline.ArrivesAt(bAtA);
				Lane.bLeaves = Guideline.LeavesFrom(bAtA);
			}
		}
		if (Lanes[0].Num() == 0 || Lanes[1].Num() == 0)
		{
			return;
		}
		auto VehicleOf = [DesignVehicles](const URoadProfile* Profile)
		{
			return DesignVehicles != nullptr ? DesignVehicles->VehicleFor(Profile) : Profile->ResolvedDesignBody();
		};
		const FVehicle First = VehicleOf(Profiles[0]);
		const FVehicle Second = VehicleOf(Profiles[1]);
		const FVehicle& Design = First.Chassis.TightestFollowableRadius() <= Second.Chassis.TightestFollowableRadius() ? First : Second;
		const uint32 Digest = BodyDigestOf(Design);
		if (Cached == nullptr || Cached->BodyDigest != Digest)
		{
			if (WideningCache().Num() >= MaxCachedWidenings)
			{
				WideningCache().Reset();
			}
			FCachedWidening Fresh;
			Fresh.BodyDigest = Digest;
			Fresh.bWidens = BendWidening::Measure(Out.Input, Out.Result, Lanes, Design, Fresh.Widening);
			FRoadNetworkSolver::WideningTraceCountForTest += Fresh.Widening.Drives;
			Cached = &WideningCache().Add(Key, MoveTemp(Fresh));
		}
	}
	if (Cached == nullptr || !Cached->bWidens)
	{
		return;
	}
	BendWidening::FWidening Widening = Cached->Widening;

	// The arms cut back to hold it: a floor under the fillet's cut, capped by each arm's allowance
	// like a taper's inset (FJunctionArm::MinCutDistance).
	for (int32 Arm = 0; Arm < 2; ++Arm)
	{
		Out.Input.Arms[Arm].MinCutDistance = FMath::Max(Out.Input.Arms[Arm].MinCutDistance, Widening.NeededCut[Arm]);
	}
	Out.Result = FJunctionSolver::SolveCuts(Out.Input);
	if (!Out.Result.bValid)
	{
		return;
	}
	TArray<FVector2D> Rim;
	double Shortfall = 0.0;
	BendWidening::Rim(Out.Input, Out.Result, Widening, Rim, Shortfall);
	Out.Input.Arms[Widening.Corner].RimToNext = MoveTemp(Rim);

	// A SHORT ARM CAPS THE WIDENING, and the design vehicle then still leaves the tarmac here.
	// Recorded, not logged: SolveAll says it once per Topology rebuild (the snap solves nodes on
	// every cursor move, and a warning there would be one per move). The length named is the one
	// whose allowance would hold the cut the widening needed: allowance = floor + SlackShare x slack.
	if (Shortfall > 1.0)
	{
		for (int32 Arm = 0; Arm < 2; ++Arm)
		{
			const double Missing = Widening.NeededCut[Arm] - Out.Result.Arms[Arm].CutDistance;
			const FRoadSegment* Segment = Network.GetSegment(Out.ArmSegments[Arm]);
			if (Missing > 1.0 && Segment != nullptr && Missing / SlackShare > Out.Capped.LengthNeeded - Out.Capped.Length)
			{
				Out.Capped.NodeIndex = NodeIndex;
				Out.Capped.Position = Out.Input.Position;
				Out.Capped.Missing = Shortfall;
				Out.Capped.Overrun = FMath::Max(0.0, Shortfall - BendWidening::Margin);
				Out.Capped.Segment = Out.ArmSegments[Arm];
				Out.Capped.Length = SegmentChordLength(Network, *Segment);
				Out.Capped.LengthNeeded = Out.Capped.Length + Missing / SlackShare;
			}
		}
	}
}
}

int32 FRoadNetworkSolver::WideningTraceCountForTest = 0;

bool FRoadNetworkSolver::SolveNodeCuts(const URoadNetwork& Network, int32 NodeIndex,
	int32 ArcSegments, FRoadNodeCuts& Out, const FRoadDesignVehicles* DesignVehicles, EWideningTrace Widening)
{
	// THE FIT, then the widening: the widening is laid round the FITTED fillet, and only ever
	// pushes a cut further out within the allowance the fit respected.
	if (!FitNodeCuts(Network, NodeIndex, ArcSegments, Out, DesignVehicles))
	{
		return false;
	}
	WidenBend(Network, NodeIndex, Out, DesignVehicles, Widening);
	return true;
}

int32 FRoadNetworkSolver::NodeClaimsCallCountForTest = 0;

bool FRoadNetworkSolver::NodeClaims(const URoadNetwork& Network, FRoadNodeId Node, const FVector2D& Point, double Factor,
	const FRoadDesignVehicles* DesignVehicles)
{
	// COUNTED BEFORE ANY REFUSAL BELOW: this is "did a solve run", not "did it succeed" - see
	// the counter's own comment. FRoadNodeSnapRule's cheap reject is meant to stop this
	// function being CALLED for a node the cursor could not possibly be inside, not merely to
	// make the call return false quickly.
	++NodeClaimsCallCountForTest;

	if (Factor <= 0.0)
	{
		return false;
	}
	const FRoadNode* Live = Network.GetNode(Node);
	if (Live == nullptr)
	{
		return false;
	}

	FRoadNodeCuts Cuts;
	if (!SolveNodeCuts(Network, Node.Index, 4, Cuts, DesignVehicles) || !Cuts.Result.bValid)
	{
		return false;
	}
	FJunctionSolver::SolveBoundary(Cuts.Input, Cuts.Result);

	// The rim is every boundary point but the trailing apex; fewer than three is a dead
	// end (two cut vertices) with no polygon of its own.
	const int32 RimCount = Cuts.Result.Boundary.Num() - 1;
	if (RimCount < 3)
	{
		double HalfWidth = 0.0;
		for (const FJunctionArm& Arm : Cuts.Input.Arms)
		{
			HalfWidth = FMath::Max(HalfWidth, FMath::Max(Arm.HalfWidthLeft, Arm.HalfWidthRight));
		}
		const double Reach = HalfWidth * Factor;
		return FVector2D::DistSquared(Live->Position, Point) <= Reach * Reach;
	}

	TArray<FVector2D> Rim;
	Rim.Reserve(RimCount);
	for (int32 Slot = 0; Slot < RimCount; ++Slot)
	{
		Rim.Add(Live->Position + (Cuts.Result.Boundary[Slot] - Live->Position) * Factor);
	}
	if (RoadGeom::PointInPolygon(Rim, Point))
	{
		return true;
	}

	// The rim ITSELF is pavement: the arm's derived guideline node sits exactly on the cut
	// line, and a point-in-polygon test is undefined on its own edge. One uu of tolerance
	// is far below anything a cursor can express and far above double noise.
	constexpr double EdgeTolerance = 1.0;
	for (int32 Slot = 0; Slot < RimCount; ++Slot)
	{
		const FVector2D& A = Rim[Slot];
		const FVector2D& B = Rim[(Slot + 1) % RimCount];
		const FVector2D AB = B - A;
		const double LengthSquared = AB.SizeSquared();
		const double T = LengthSquared > 0.0 ? FMath::Clamp(FVector2D::DotProduct(Point - A, AB) / LengthSquared, 0.0, 1.0) : 0.0;
		if (FVector2D::Distance(Point, A + AB * T) <= EdgeTolerance)
		{
			return true;
		}
	}
	return false;
}

double FRoadNetworkSolver::ArmCutDistance(const URoadNetwork& Network, FRoadSegmentId Segment, FRoadNodeId AtNode,
	const FRoadDesignVehicles* DesignVehicles)
{
	FRoadNodeCuts Cuts;
	if (!SolveNodeCuts(Network, AtNode.Index, 4, Cuts, DesignVehicles) || !Cuts.Result.bValid)
	{
		return 0.0;
	}
	const int32 ArmIndex = Cuts.ArmSegments.IndexOfByKey(Segment);
	return Cuts.Result.Arms.IsValidIndex(ArmIndex) ? Cuts.Result.Arms[ArmIndex].CutDistance : 0.0;
}

double FRoadNetworkSolver::NodeReach(const URoadNetwork& Network, FRoadNodeId Node,
	int32 ArcSegments, const FRoadDesignVehicles* DesignVehicles)
{
	FRoadNodeCuts Cuts;
	if (!SolveNodeCuts(Network, Node.Index, ArcSegments, Cuts, DesignVehicles) || !Cuts.Result.bValid)
	{
		// No arms, or a solve that declined. Either way this node paves nothing, so it
		// claims nothing - a bare node must not grow a snap radius around itself.
		return 0.0;
	}

	double Reach = 0.0;
	for (int32 ArmIndex = 0; ArmIndex < Cuts.Result.Arms.Num(); ++ArmIndex)
	{
		if (!Cuts.Input.Arms.IsValidIndex(ArmIndex))
		{
			continue;
		}

		const FJunctionArm& Arm = Cuts.Input.Arms[ArmIndex];
		const double HalfWidth = FMath::Max(Arm.HalfWidthLeft, Arm.HalfWidthRight);
		Reach = FMath::Max(Reach, Cuts.Result.Arms[ArmIndex].CutDistance + HalfWidth);
	}

	return Reach;
}

void FRoadNetworkSolver::SolveNodeInto(URoadNetwork& Network, int32 NodeIndex, int32 ArcSegments,
	FRoadSolveResult& InOutResult, const FRoadDesignVehicles* DesignVehicles, EWideningTrace Widening)
{
	const TArray<FRoadNode>& Nodes = Network.GetNodes();
	if (!Nodes.IsValidIndex(NodeIndex))
	{
		return;
	}

	const FRoadNode& Node = Nodes[NodeIndex];
	if (!Node.bAlive || Node.Incident.Num() == 0)
	{
		return;
	}

	// Network.NodeIdAt, not a hand-built handle (#79, #173).
	const FRoadNodeId NodeId = Network.NodeIdAt(NodeIndex);

	// The arm gathering, the skip rule and the fillet clamp all live in SolveNodeCuts,
	// so a tool asking how far this junction reaches gets the answer from the same
	// code that decides where the pavement actually stops.
	FRoadNodeCuts Cuts;
	if (!SolveNodeCuts(Network, NodeIndex, ArcSegments, Cuts, DesignVehicles, Widening))
	{
		return;
	}
	if (Cuts.Capped.NodeIndex != INDEX_NONE)
	{
		InOutResult.CappedWidenings.Add(Cuts.Capped);
	}

	FJunctionInput& Input = Cuts.Input;
	FJunctionResult& Result = Cuts.Result;
	const TArray<FRoadSegmentId>& ArmSegments = Cuts.ArmSegments;

	FJunctionSolver::SolveBoundary(Input, Result);

	if (!Result.bValid)
	{
		++InOutResult.FailedNodes;

		// A failed solve must not leave a previous solve's vertices stranded looking
		// valid. Clear only the end this node owns on every incident segment - the
		// other end (at the segment's other node) is untouched and keeps its own flag.
		// ClearSegmentEndSolve (#191), not a raw FRoadSegment* write: the accessor that
		// used to hand one out is private now, and this is the whole of what a failed
		// solve needs to say - "not this end", nothing about the stale vertices beside it.
		for (const FRoadSegmentId SegmentId : Node.Incident)
		{
			const FRoadSegment* Segment = Network.GetSegment(SegmentId);
			if (Segment == nullptr)
			{
				continue;
			}
			Network.ClearSegmentEndSolve(SegmentId, Segment->A == NodeId);
		}
		return;
	}

	// Write the solve back into the model. ArmSegments is index-aligned with
	// Result.Arms (both built in lockstep above), so ArmSegments[ArmIndex] is the
	// segment each arm belongs to, regardless of anything skipped while building Arms.
	for (int32 ArmIndex = 0; ArmIndex < Result.Arms.Num(); ++ArmIndex)
	{
		const FRoadSegmentId SegmentId = ArmSegments[ArmIndex];
		const FRoadSegment* Segment = Network.GetSegment(SegmentId);
		if (Segment == nullptr)
		{
			continue;
		}

		// WriteSegmentEndSolve (#191) takes ArmResult (Trim/LeftCut/RightCut/bSolved
		// together) directly - the same struct SolveBoundary just filled, rather than
		// this loop unpacking it into four separate writes through a raw FRoadSegment*.
		Network.WriteSegmentEndSolve(SegmentId, Segment->A == NodeId, Result.Arms[ArmIndex]);
	}

	// Copied BEFORE Result is moved from, and keyed on the same NodeIndex.
	InOutResult.NodeArmSegments.Add(NodeIndex, ArmSegments);
	InOutResult.NodeResults.Add(NodeIndex, MoveTemp(Result));
	++InOutResult.SolvedNodes;
}

FRoadSolveResult FRoadNetworkSolver::SolveAll(URoadNetwork& Network, int32 ArcSegments,
	const FRoadDesignVehicles* DesignVehicles, EWideningTrace Widening)
{
	FRoadSolveResult Out;

	// EVERY LIVE NODE, IN ORDER, THROUGH SolveNodeInto - see that function's own comment.
	// This loop and the ghost preview's two-call solve (BuildGhostBuffers) are now the same
	// code run a different number of times, which is the whole point of #166: neither can
	// decide where a junction's pavement stops without the other agreeing.
	const int32 NodeCount = Network.GetNodes().Num();
	for (int32 NodeIndex = 0; NodeIndex < NodeCount; ++NodeIndex)
	{
		SolveNodeInto(Network, NodeIndex, ArcSegments, Out, DesignVehicles, Widening);
	}

	// A CAPPED WIDENING, SAID ONCE PER REBUILD (review of 75d3cbc0): only a tracing solve - a
	// Topology rebuild - says it, so a drag's Geometry frames and the snap's queries stay quiet.
	// Named like the capped taper's warning (Airside.Build.WidthTaper.CappedTaperWarns): the bend,
	// what its design vehicle still overruns, and the segment length that would hold the widening.
	// ENFORCED BY: Airside.Build.BendLanes.CappedWideningWarns
	if (Widening == EWideningTrace::Trace)
	{
		for (const FCappedWidening& Capped : Out.CappedWidenings)
		{
			UE_LOG(LogRoadSolve, Warning,
				TEXT("Bend at (%.0f,%.0f): its inside widening is capped by a short arm - %.0f uu of it is missing, so its ")
				TEXT("design vehicle still leaves the tarmac by about %.0f uu there. Segment %d is %.1f m long; draw it at ")
				TEXT("least %.1f m long, plus its far junction's cut-back."),
				Capped.Position.X, Capped.Position.Y, Capped.Missing, Capped.Overrun, Capped.Segment.Index,
				Capped.Length / 100.0, Capped.LengthNeeded / 100.0);
		}
	}

	return Out;
}
