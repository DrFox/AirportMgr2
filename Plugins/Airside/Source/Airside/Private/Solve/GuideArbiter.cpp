#include "Solve/GuideArbiter.h"

namespace
{
	/**
	 * Two candidates computed by different arithmetic paths - a frontage normalised from
	 * node positions and a world axis from a cosine - can land a rounding apart when they
	 * mean the same angle. A source-order tiebreak that only fired on bitwise equality would
	 * therefore be decided by the last bit of a double, which is not a rule anyone can read.
	 */
	constexpr double TieEpsilon = 1.0e-9;

	/** Acute angle between two unit directions, in degrees. A guide is a LINE: see Arbitrate. */
	double ErrorDegrees(const FVector2D& A, const FVector2D& B)
	{
		const double Aligned = FMath::Abs(FVector2D::DotProduct(A, B));
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Aligned, 0.0, 1.0)));
	}

	/** Perpendicular distance from Point to the line through Through along Direction, uu. */
	double ErrorUu(const FVector2D& Through, const FVector2D& Direction, const FVector2D& Point)
	{
		const FVector2D Unit = Direction.GetSafeNormal();
		const FVector2D Offset = Point - Through;
		return FMath::Abs(Offset.X * Unit.Y - Offset.Y * Unit.X);
	}

	/** The cursor's perpendicular projection onto a candidate's line. */
	FVector2D Project(const SnapGuide::FCandidate& Candidate, const FVector2D& Cursor)
	{
		const FVector2D Unit = Candidate.Direction.GetSafeNormal();
		return Candidate.Through + Unit * FVector2D::DotProduct(Cursor - Candidate.Through, Unit);
	}

	/**
	 * Where two candidates' lines cross. False when they are parallel - which includes the
	 * case of one candidate against itself, and is why this is a question rather than an
	 * assumption.
	 */
	bool Intersect(const SnapGuide::FCandidate& A, const SnapGuide::FCandidate& B, FVector2D& Out)
	{
		const FVector2D DirA = A.Direction.GetSafeNormal();
		const FVector2D DirB = B.Direction.GetSafeNormal();

		const double Cross = DirA.X * DirB.Y - DirA.Y * DirB.X;
		if (FMath::IsNearlyZero(Cross, 1.0e-9))
		{
			return false;
		}

		const FVector2D Delta = B.Through - A.Through;
		const double Along = (Delta.X * DirB.Y - Delta.Y * DirB.X) / Cross;
		Out = A.Through + DirA * Along;
		return true;
	}

	/**
	 * This frame's error for a candidate, in its own kind's units, and whether it is eligible.
	 *
	 * The two kinds are never compared with each other - see EFit. This returns each in its
	 * own currency and the caller keeps them in separate races.
	 */
	bool Measure(const SnapGuide::FCandidate& Candidate, const FVector2D& Heading,
		bool bHasHeading, const FVector2D& Cursor, const SnapGuide::FTuning& Tuning,
		double& OutError)
	{
		if (Candidate.Direction.IsNearlyZero())
		{
			return false;
		}

		if (Candidate.Fit == SnapGuide::EFit::Angular)
		{
			// A CURSOR ON TOP OF THE ORIGIN HAS NO DIRECTION, so no angular candidate can be
			// measured at all - see Arbitrate. A perpendicular one still can, which is why
			// this is asked per candidate rather than once at the top.
			if (!bHasHeading)
			{
				return false;
			}
			OutError = ErrorDegrees(Heading, Candidate.Direction.GetSafeNormal());
			return OutError <= Tuning.ToleranceDegrees;
		}

		OutError = ErrorUu(Candidate.Through, Candidate.Direction, Cursor);
		return OutError <= Tuning.ToleranceUu;
	}

	/** The stickiness that applies to this kind, in that kind's units. */
	double StickinessFor(SnapGuide::EFit Fit, const SnapGuide::FTuning& Tuning)
	{
		return Fit == SnapGuide::EFit::Angular ? Tuning.StickinessDegrees : Tuning.StickinessUu;
	}
}

SnapGuide::FResult SnapGuide::Arbitrate(TConstArrayView<FCandidate> Candidates,
	const FVector2D& Origin, const FVector2D& Cursor,
	const FResult& Previous, const FTuning& Tuning)
{
	FResult Result;

	// THE DRAG'S OWN DIRECTION, which every Angular candidate is judged against. On the first
	// frame of a drag the cursor IS the origin and there is no direction to speak of: acos of
	// a zero vector's dot is 90 degrees against every candidate at once, and whichever sorted
	// first would flash on before the player had moved the mouse. A Perpendicular candidate
	// needs no heading, so this gates that kind alone rather than the whole function.
	const FVector2D Reach = Cursor - Origin;
	const double Distance = Reach.Size();
	const bool bHasHeading = Distance > 0.0;
	const FVector2D Heading = bHasHeading ? Reach / Distance : FVector2D(1.0, 0.0);

	// Whether a candidate of this kind can be usefully paired with Against - see the
	// perpendicular race below for why this is a filter and not an afterthought.
	auto CrossesUsefully = [&](const FCandidate& Candidate, const FCandidate* Against)
	{
		if (Against == nullptr)
		{
			return true;
		}
		FVector2D Crossing = FVector2D::ZeroVector;
		return Intersect(*Against, Candidate, Crossing)
			&& FVector2D::Distance(Crossing, Cursor) <= Tuning.MaxPullUu;
	};

	// ONE RACE PER FIT KIND. Errors are never compared across kinds: see EFit for why an
	// exchange rate between degrees and uu has no business in here.
	//
	// Pairable is the angular winner once it is known, and null for the angular race itself.
	auto RunRace = [&](EFit Fit, const FCandidate* Pairable, FCandidate& Out)
	{
		const FCandidate* Best = nullptr;
		double BestError = 0.0;

		for (const FCandidate& Candidate : Candidates)
		{
			double Error = 0.0;
			if (Candidate.Fit != Fit
				|| !Measure(Candidate, Heading, bHasHeading, Cursor, Tuning, Error)
				|| !CrossesUsefully(Candidate, Pairable))
			{
				continue;
			}

			// SMALLEST ERROR FIRST, then SOURCE ORDER. The second half is what stops the
			// answer depending on the order candidates happened to be gathered in - which in
			// stage 2 is the network's iteration order, and so changes with an unrelated edit.
			const bool bClearlyBetter = Best == nullptr || Error < BestError - TieEpsilon;
			const bool bTiedAndHigherPriority = Best != nullptr
				&& Error <= BestError + TieEpsilon
				&& Candidate.Source < Best->Source;

			if (bClearlyBetter || bTiedAndHigherPriority)
			{
				Best = &Candidate;
				BestError = Error;
			}
		}

		// THE FLICKER RULE, within this kind. The incumbent is re-measured against THIS cursor
		// from the candidate value itself, so the arbiter never has to recognise "the same
		// candidate" in this frame's list - an identity test that stage 2's network sources
		// would make unreliable. It still has to be IN TOLERANCE, and still has to pair: one
		// the cursor has walked away from is no longer a guide, however sticky.
		//
		// THE INCUMBENT KEEPS THE BOUNDARY. A challenger better by EXACTLY the stickiness is
		// the case a slow drag passes through, and the bare comparison decides it on whether
		// acos returned 1.0 or 0.99999999999998 - the first version of this failed its own
		// test that way. Holding is what stickiness is for: the tie goes to not changing.
		for (const FCandidate& Held : Previous.Winners)
		{
			double HeldError = 0.0;
			if (Held.Fit != Fit
				|| !Measure(Held, Heading, bHasHeading, Cursor, Tuning, HeldError)
				|| !CrossesUsefully(Held, Pairable))
			{
				continue;
			}

			if (Best == nullptr
				|| !(BestError < HeldError - StickinessFor(Fit, Tuning) - TieEpsilon))
			{
				// By VALUE, not a pointer into Previous: the winner outlives this call.
				Out = Held;
				return true;
			}
		}

		if (Best == nullptr)
		{
			return false;
		}
		Out = *Best;
		return true;
	};

	// ANGULAR FIRST, and not merely for display order: it is the guide that describes the
	// direction the player is actively dragging, so it is the one the alignment has to pair
	// WITH rather than compete against.
	FCandidate Angular;
	const bool bHasAngular = RunRace(EFit::Angular, nullptr, Angular);

	// THE PERPENDICULAR RACE SKIPS ANYTHING THAT CANNOT USEFULLY CROSS THE ANGULAR WINNER,
	// and that filter is the whole reason this is two passes rather than one.
	//
	// The second guide exists to pin the degree of freedom the first leaves free. A line
	// parallel to the angular winner pins nothing it has not already pinned, and one that
	// meets it a kilometre away pins it somewhere the player cannot see. Ranking on error
	// alone let exactly that happen: dragging a plot's last corner, a redundant line through
	// the anchor sat 70 uu away and beat the useful alignment 80 uu away, and the pair was
	// then thrown out for being parallel - losing the alignment the player actually wanted.
	FCandidate Perpendicular;
	const bool bHasPerpendicular = RunRace(
		EFit::Perpendicular, bHasAngular ? &Angular : nullptr, Perpendicular);

	if (bHasAngular)
	{
		Result.Winners.Add(Angular);
	}
	if (bHasPerpendicular)
	{
		Result.Winners.Add(Perpendicular);
	}

	if (Result.Winners.Num() == 0)
	{
		// NOTHING IN TOLERANCE MEANS NO GUIDE, not a nearest-anyway answer. A guide that is
		// always on is a constraint, and the player never asked for one.
		return Result;
	}

	Result.bActive = true;

	if (Result.Winners.Num() == 2)
	{
		// THE INTERSECTION IS WHAT MAKES BOTH LABELS TRUE. A corner shown as "0 degrees to
		// corner 3" while sitting off being level with corner 3 is a mark whose meaning has
		// gone, and this codebase deletes those rather than shipping them.
		//
		// It cannot fail here: the pair was chosen by the filter above precisely because it
		// crosses within the pull guard. Honoured rather than assumed, because a filter and
		// its consequence living apart is how the two come to disagree.
		FVector2D Crossing = FVector2D::ZeroVector;
		if (Intersect(Result.Winners[0], Result.Winners[1], Crossing))
		{
			Result.Point = Crossing;
			return Result;
		}
		Result.Winners.RemoveAt(1);
	}

	Result.Point = Project(Result.Winners[0], Cursor);
	return Result;
}
