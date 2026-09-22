#pragma once

#include "CoreMinimal.h"

/**
 * Where a plot's fence posts stand and where its fabric hangs.
 *
 * Dependency-free, like every Solve/ header: an outline and a gate in, posts and spans out,
 * testable with no world. The presenter turns this into instances and a mesh strip; nothing
 * here knows what a post looks like.
 *
 * WHY NOT A FIXED BAY LENGTH, which is what the grey-box fence did (floor(L / 250) panels and
 * the remainder dropped). A plot edge is whatever length the player dragged, so a fixed bay
 * either leaves a gap at the corner or overhangs it. Here every run between two FIXED posts
 * (corners, gate posts) is divided into round(L / Spacing) equal bays: the posts are rigid
 * and evenly spaced, and only the fabric's U stretches to take up the difference - see the
 * asset README's "Spacing and edge subdivision".
 */
namespace FenceLayout
{
	/** What a post is for, which decides its mesh. Corner and Gate both take the heavy post. */
	enum class EPostKind : uint8
	{
		Line,
		Corner,
		Gate,
	};

	/** The numbers the layout needs. Defaults are the asset contract's; see the README. */
	struct FSpec
	{
		/** Nominal post pitch, uu. Each run's actual pitch is its length / round(length / this). */
		double SpacingUu = 250.0;

		/**
		 * How far the fabric hangs OUTSIDE the post centreline, uu - on one face the way real
		 * fabric is tied on, not through the middle of the posts.
		 */
		double FaceOffsetUu = 3.0;

		/** Run per texture tile, uu. U is distance along the edge divided by this. */
		double TileUu = 240.0;

		/**
		 * The gate opening, post centre to post centre, uu. Zero means no gate. The caller passes
		 * PlotYard::GateCorridorUu so the gap and the lane the yard keeps clear are one number.
		 */
		double GateWidthUu = 0.0;
	};

	/** One post, standing on the outline. */
	struct FPost
	{
		FVector2D Position = FVector2D::ZeroVector;

		/** Heading in radians: along the edge for Line and Gate, the outward bisector for Corner. */
		double YawRad = 0.0;

		EPostKind Kind = EPostKind::Line;
	};

	/** One bay of fabric between two adjacent posts, already offset outward. */
	struct FSpan
	{
		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;

		/** Texture U at A and B: distance along the edge / TileUu, continuous along the edge. */
		double U0 = 0.0;
		double U1 = 0.0;
	};

	struct FLayout
	{
		TArray<FPost> Posts;
		TArray<FSpan> Spans;

		/**
		 * False when the gate's edge is too short to hold GateWidthUu plus a bay either side, or
		 * no gate was asked for. A fence with no gate is a depot no truck can leave, and it looks
		 * completely correct from every angle - so the presenter warns rather than draws quietly.
		 */
		bool bHasGate = false;

		/** Midpoint of the gate opening; the requested gate unless it slid clear of a corner. */
		FVector2D GateCentre = FVector2D::ZeroVector;

		/** Posts of one kind. */
		AIRSIDE_API int32 CountOf(EPostKind Kind) const;
	};

	/**
	 * The fence round Outline, gated on the edge nearest Gate.
	 *
	 * EITHER WINDING. The outline is read counter-clockwise internally (from its signed area),
	 * so a plot stored clockwise hangs its fabric outside too rather than inside. Consecutive
	 * vertices closer than 1 uu are merged. Fewer than three distinct vertices returns an empty
	 * layout.
	 */
	AIRSIDE_API FLayout Solve(TArrayView<const FVector2D> Outline, const FVector2D& Gate,
		const FSpec& Spec);
}
