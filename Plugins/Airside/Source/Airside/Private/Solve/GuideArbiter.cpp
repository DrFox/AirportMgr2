#include "Solve/GuideArbiter.h"

namespace
{
	/**
	 * Two candidates computed by different arithmetic paths - a frontage normalised from
	 * node positions and a world axis from a cosine - can land a rounding apart when they
	 * mean the same angle. A source-order tiebreak that only fired on bitwise equality would
	 * therefore be decided by the last bit of a double, which is not a rule anyone can read.
	 */
	constexpr double TieEpsilonDegrees = 1.0e-9;

	/** Acute angle between two unit directions, in degrees. A guide is a LINE: see Arbitrate. */
	double ErrorDegrees(const FVector2D& A, const FVector2D& B)
	{
		const double Aligned = FMath::Abs(FVector2D::DotProduct(A, B));
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Aligned, 0.0, 1.0)));
	}

	/** The cursor's perpendicular projection onto the line through Origin along Direction. */
	FVector2D Project(const FVector2D& Origin, const FVector2D& Direction, const FVector2D& Cursor)
	{
		const FVector2D Unit = Direction.GetSafeNormal();
		return Origin + Unit * FVector2D::DotProduct(Cursor - Origin, Unit);
	}
}

SnapGuide::FResult SnapGuide::Arbitrate(TConstArrayView<FCandidate> Candidates,
	const FVector2D& Origin, const FVector2D& Cursor,
	const FResult& Previous, const FTuning& Tuning)
{
	FResult Result;

	// A CURSOR ON TOP OF THE ORIGIN HAS NO DIRECTION, and this is not a degenerate case to
	// fudge: on the first frame of a drag the two ARE equal, and acos of a zero vector's dot
	// is 90 degrees against every candidate at once - seven guides tied, and whichever
	// sorted first would flash on before the player had moved the mouse.
	const FVector2D Reach = Cursor - Origin;
	const double Distance = Reach.Size();
	if (Distance <= 0.0)
	{
		return Result;
	}
	const FVector2D Heading = Reach / Distance;

	const FCandidate* Best = nullptr;
	double BestError = 0.0;
	for (const FCandidate& Candidate : Candidates)
	{
		// A SOURCE THAT HAD NOTHING TO SAY SHOULD HAVE SAID NOTHING, but a zero direction
		// would otherwise normalise to (1,0) and become a silent extra world axis.
		if (Candidate.Direction.IsNearlyZero())
		{
			continue;
		}

		const double Error = ErrorDegrees(Heading, Candidate.Direction.GetSafeNormal());
		if (Error > Tuning.ToleranceDegrees)
		{
			continue;
		}

		// SMALLEST ERROR FIRST, then SOURCE ORDER. The second half is what stops the answer
		// depending on the order candidates happened to be gathered in - which in stage 2 is
		// the network's iteration order, and therefore changes with an unrelated edit.
		const bool bClearlyBetter = Best == nullptr || Error < BestError - TieEpsilonDegrees;
		const bool bTiedAndHigherPriority = Best != nullptr
			&& Error <= BestError + TieEpsilonDegrees
			&& Candidate.Source < Best->Source;

		if (bClearlyBetter || bTiedAndHigherPriority)
		{
			Best = &Candidate;
			BestError = Error;
		}
	}

	// THE FLICKER RULE. The incumbent is re-measured against THIS cursor from the candidate
	// value itself, so the arbiter never has to recognise "the same candidate" in this
	// frame's list - an identity test that stage 2's network sources would make unreliable.
	// It still has to be IN TOLERANCE: an incumbent the cursor has walked away from is no
	// longer a guide, however sticky.
	if (Previous.bActive && !Previous.Winner.Direction.IsNearlyZero())
	{
		const double HeldError = ErrorDegrees(Heading, Previous.Winner.Direction.GetSafeNormal());
		const bool bStillEligible = HeldError <= Tuning.ToleranceDegrees;

		// THE INCUMBENT KEEPS THE BOUNDARY, and the epsilon is what makes that a rule rather
		// than a coin flip. A challenger better by EXACTLY StickinessDegrees is the case a
		// slow drag passes through, and the bare comparison decides it on whether acos
		// returned 1.0 or 0.99999999999998 - the first version of this failed its own test
		// that way. Holding is the answer that matches what stickiness is for: the tie goes
		// to not changing.
		const bool bChallengerWins = Best != nullptr
			&& BestError < HeldError - Tuning.StickinessDegrees - TieEpsilonDegrees;

		if (bStillEligible && !bChallengerWins)
		{
			Result.bActive = true;
			Result.Winner = Previous.Winner;
			Result.Point = Project(Origin, Result.Winner.Direction, Cursor);
			return Result;
		}
	}

	if (Best == nullptr)
	{
		// NOTHING IN TOLERANCE MEANS NO GUIDE, not a nearest-anyway answer. A guide that is
		// always on is a constraint, and the player never asked for one.
		return Result;
	}

	Result.bActive = true;
	Result.Winner = *Best;
	Result.Point = Project(Origin, Best->Direction, Cursor);
	return Result;
}
