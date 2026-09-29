#pragma once

#include "CoreMinimal.h"
#include "Build/MarkingGlyphs.h"
#include "Build/MarkingQuads.h"

/**
 * A line of painted text - MarkingGlyphs' stroke font laid out and emitted as marking quads.
 *
 * EXTRACTED FROM THE RUNWAY BUILDER'S Designation (taxiway strip stage 5, 2026-09-29) so the
 * stand turn-off can paint its number in the same font rather than growing a second text
 * renderer beside a second font: the runway's 36/18 and a stand's 12 are the same kind of
 * paint. Private to Build/ and header-only inline, like MarkingQuads and MarkingGlyphs beside
 * it: there is no state, and every caller is a marking builder in this folder.
 *
 * FRAMED BY AN ORIGIN AND TWO AXES, not the runway's FRunwayFrame, because a stand's number
 * has no runway: Origin is the centre of the text's FOOT line (the edge nearest the reader),
 * Up runs from the glyphs' feet to their tops, Right is the reader's right. Both unit length;
 * mirroring is the caller's choice of axes, exactly as MarkingGlyphs leaves it to the builder.
 * ENFORCED BY: Airside.Build.RunwayMarkings.PaintPinned (the runway paint did not move).
 */
namespace MarkingText
{
	/** The painted width of Text, uu: Len cells plus the gaps between them. */
	inline double Width(const FString& Text, double CellHeight, double Spacing)
	{
		const double Scale = CellHeight / MarkingGlyphs::CellHeight;   // one cell unit in uu
		return Text.Len() > 0 ? Text.Len() * Scale + (Text.Len() - 1) * Spacing * Scale : 0.0;
	}

	/**
	 * Paint Text, CellHeight tall, centred on Origin across Right, feet on Origin. Spacing is
	 * the gap between glyphs as a fraction of a cell's width (the runway's DigitSpacing).
	 * Returns strokes painted - one quad per polyline span - which is what the runway census
	 * has always counted.
	 */
	inline int32 AddString(FRoadMeshBuffers& Out, double Z, const FVector2D& Origin,
		const FVector2D& Up, const FVector2D& Right, double CellHeight, double Spacing,
		const FString& Text, int32 MaterialID = 0)
	{
		const double Scale = CellHeight / MarkingGlyphs::CellHeight;   // one cell unit in uu
		double Left = -Width(Text, CellHeight, Spacing) * 0.5;
		int32 Strokes = 0;
		for (const TCHAR Character : Text)
		{
			for (const TArray<FVector2D>& Line : MarkingGlyphs::Strokes(Character))
			{
				for (int32 Index = 0; Index + 1 < Line.Num(); ++Index)
				{
					// Each polyline span is a quad StrokeWidth wide, extended half a
					// stroke at both ends so consecutive spans meet square at a corner
					// rather than leaving a notch - which is also what makes a glyph fill
					// its cell exactly (see MarkingGlyphs).
					const FVector2D A = Line[Index];
					const FVector2D C = Line[Index + 1];
					const FVector2D U = (C - A).GetSafeNormal();
					const FVector2D N(-U.Y, U.X);
					constexpr double H = MarkingGlyphs::StrokeWidth * 0.5;
					const FVector2D A2 = A - U * H;
					const FVector2D C2 = C + U * H;
					auto World = [&](const FVector2D& Cell)
					{
						return Origin + Up * (Cell.Y * Scale) + Right * (Left + Cell.X * Scale);
					};
					MarkingQuads::AddQuad(Out, Z,
						World(A2 - N * H), World(C2 - N * H), World(C2 + N * H), World(A2 + N * H),
						MaterialID);
					++Strokes;
				}
			}
			Left += Scale * (1.0 + Spacing);
		}
		return Strokes;
	}
}
