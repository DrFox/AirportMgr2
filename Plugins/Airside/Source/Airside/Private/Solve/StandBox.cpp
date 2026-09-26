#include "Solve/StandBox.h"

#include "Solve/RoadGeom.h"

namespace StandBox
{
	double EntranceSetback(EIcaoCode Letter, const FLetterEnvelope& Envelope)
	{
		return Envelope.MaxTailAft + IcaoCode::WingtipClearanceForLetter(Letter);
	}

	FStandPose PoseFor(const FVector2D& EntranceA, const FVector2D& EntranceB,
		const FVector2D& Inward, EIcaoCode Letter, const FLetterEnvelope& Envelope)
	{
		// THE TAIL SITS EntranceSetback IN FROM THE ENTRANCE EDGE, centred - see that
		// function's own comment. Every metre of Depth beyond it and the nose overhang is
		// slack the far-side entry spends AHEAD of the nose, not room the airframe uses.
		FStandPose Pose;
		Pose.Facing = Inward.GetSafeNormal();
		Pose.Position = (EntranceA + EntranceB) * 0.5
			+ Pose.Facing * EntranceSetback(Letter, Envelope);
		return Pose;
	}

	void BoxAt(const FStandPose& Pose, EIcaoCode Letter, const FLetterEnvelope& Envelope,
		TArray<FVector2D>& OutCorners)
	{
		const double HalfWidth = 0.5 * IcaoCode::StandWidthForLetter(Letter);
		const double Depth = IcaoCode::StandDepthForLetter(Letter);
		const FVector2D Back = Pose.Position - Pose.Facing * EntranceSetback(Letter, Envelope);
		const FVector2D Front = Back + Pose.Facing * Depth;
		// -PerpCCW: entrance runs so that inward is on its LEFT, which is what makes the
		// quad counter-clockwise (positive area) - the winding the pad triangulator needs.
		const FVector2D Side = -RoadGeom::PerpCCW(Pose.Facing) * HalfWidth;
		OutCorners = { Back - Side, Back + Side, Front + Side, Front - Side };
	}

	double WidthOf(TArrayView<const FVector2D> Rect)
	{
		// ROUNDED TO A WHOLE uu, because the raw length is not the length the player drew.
		// The stand tool builds a corner as a unit direction times a whole-uu length, and off
		// the axes that unit vector is 1 give or take an ulp - so a stand dragged exactly to a
		// letter's floor measured 5499.999999999 against a 5500 floor, and
		// LetterForStandSize's exact >= read it as the letter below while Size said "55 m".
		// Worse, URoadEditFacade::PlaceStandInPlot reverses a clockwise outline and so
		// measures the OPPOSITE edge, which is not bitwise equal to this one: the readout could
		// light Build for one letter and the commit store another, or refuse.
		//
		// HERE, NOT AT EACH CALLER, because every consumer - the tool's readout and preview,
		// WhyStandRefused, PlaceStandInPlot, LetterOf - measures through these two, so rounding
		// once makes them agree by construction. Whole uu (1 cm) because every floor in
		// IcaoCode's table is a whole uu and no gesture quantises finer than 1 m.
		// ENFORCED BY: Airside.Solve.StandBox.MeasuresWholeUu, and
		// Airside.Tool.StandPlot.DiagonalTaxiwayReadsItsLetter for the tool and the commit.
		return FMath::RoundToDouble((Rect[1] - Rect[0]).Length());
	}

	double DepthOf(TArrayView<const FVector2D> Rect)
	{
		// Rounded for WidthOf's reason - see there.
		return FMath::RoundToDouble((Rect[2] - Rect[1]).Length());
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
