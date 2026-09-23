#include "Solve/StandBox.h"

#include "Solve/RoadGeom.h"

namespace StandBox
{
	FStandPose PoseFor(const FVector2D& EntranceA, const FVector2D& EntranceB,
		const FVector2D& Inward, EIcaoCode Letter)
	{
		// THE TEMPLATE'S BACK EDGE (X = NoseFwd - Depth, the tail side) IS LAID ON THE ENTRANCE
		// EDGE, centred - see UEntityDefinition::BuildStandTemplate. So the stop mark is
		// Depth - NoseFwd in from it, and every metre the player drew beyond the floor is
		// apron past the nose.
		FStandPose Pose;
		Pose.Facing = Inward.GetSafeNormal();
		Pose.Position = (EntranceA + EntranceB) * 0.5
			+ Pose.Facing * (IcaoCode::StandDepthForLetter(Letter) - IcaoCode::MaxNoseFwdForLetter(Letter));
		return Pose;
	}

	void BoxAt(const FStandPose& Pose, EIcaoCode Letter, TArray<FVector2D>& OutCorners)
	{
		const double HalfWidth = 0.5 * IcaoCode::StandWidthForLetter(Letter);
		const double NoseFwd = IcaoCode::MaxNoseFwdForLetter(Letter);
		const double Depth = IcaoCode::StandDepthForLetter(Letter);
		const FVector2D Back = Pose.Position - Pose.Facing * (Depth - NoseFwd);
		const FVector2D Front = Pose.Position + Pose.Facing * NoseFwd;
		// -PerpCCW: entrance runs so that inward is on its LEFT, which is what makes the
		// quad counter-clockwise (positive area) - the winding the pad triangulator needs.
		const FVector2D Side = -RoadGeom::PerpCCW(Pose.Facing) * HalfWidth;
		OutCorners = { Back - Side, Back + Side, Front + Side, Front - Side };
	}

	double WidthOf(TArrayView<const FVector2D> Rect)
	{
		return (Rect[1] - Rect[0]).Length();
	}

	double DepthOf(TArrayView<const FVector2D> Rect)
	{
		return (Rect[2] - Rect[1]).Length();
	}

	TOptional<EIcaoCode> LetterOf(TArrayView<const FVector2D> Rect)
	{
		if (Rect.Num() < 4)
		{
			return TOptional<EIcaoCode>();
		}
		return IcaoCode::Parse(IcaoCode::LetterForStandSize(WidthOf(Rect), DepthOf(Rect)));
	}
}
