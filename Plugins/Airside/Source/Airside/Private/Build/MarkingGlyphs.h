#pragma once

#include "CoreMinimal.h"

/**
 * A stroke font for runway designations: the digits and L, C, R, each a set of polylines
 * on a cell 1 wide by 1.6 tall, drawn with a stroke 0.15 wide.
 *
 * Private to Build/, and a font of STROKES rather than outlines or a texture, because a
 * designation is painted as quads into the marking mesh like every other marking - one
 * mesh, one material, no atlas to author with the editor closed. The proportions are
 * ICAO Annex 14 Figure 3-3's, whose numerals are 9 m tall by roughly 6 m wide with a
 * 0.9 m stroke; the cell keeps that 1.6:1 ratio and a 0.15 stroke keeps the 6.7:1 of
 * stroke to width.
 *
 * Coordinates: x runs left to right AS THE PILOT READS IT approaching the threshold,
 * y runs from the bottom of the glyph (nearest the threshold) up the runway. The builder
 * maps x onto the reader's right and y onto the runway direction; mirroring is its job,
 * not the font's.
 */
namespace MarkingGlyphs
{
	/** Stroke width in cell units. Half of it extends past every polyline end so joints fill. */
	constexpr double StrokeWidth = 0.15;

	/** The cell's height; width is 1. */
	constexpr double CellHeight = 1.6;

	/**
	 * The polylines for one character, or none for a character the font lacks.
	 *
	 * Strokes are centred inside the cell by half a stroke, so the painted glyph with its
	 * stroke width fills exactly 0..1 by 0..1.6 - which is what lets the builder size a
	 * designation from DigitHeight alone and a test measure the bounding box it promised.
	 */
	inline TArray<TArray<FVector2D>> Strokes(TCHAR Character)
	{
		constexpr double H = StrokeWidth * 0.5;
		constexpr double L = H;               // left stroke centre
		constexpr double R = 1.0 - H;         // right
		constexpr double C = 0.5;             // centre
		constexpr double B = H;               // bottom
		constexpr double M = CellHeight * 0.5;// middle bar
		constexpr double T = CellHeight - H;  // top

		using FLine = TArray<FVector2D>;
		switch (Character)
		{
		case TEXT('0'): return { FLine{ {L, B}, {R, B}, {R, T}, {L, T}, {L, B} } };
		case TEXT('1'): return { FLine{ {C, B}, {C, T} } };
		case TEXT('2'): return { FLine{ {L, T}, {R, T}, {R, M}, {L, M}, {L, B}, {R, B} } };
		case TEXT('3'): return { FLine{ {L, T}, {R, T}, {R, B}, {L, B} }, FLine{ {L, M}, {R, M} } };
		case TEXT('4'): return { FLine{ {L, T}, {L, M}, {R, M} }, FLine{ {R, T}, {R, B} } };
		case TEXT('5'): return { FLine{ {R, T}, {L, T}, {L, M}, {R, M}, {R, B}, {L, B} } };
		case TEXT('6'): return { FLine{ {R, T}, {L, T}, {L, B}, {R, B}, {R, M}, {L, M} } };
		case TEXT('7'): return { FLine{ {L, T}, {R, T}, {R, B} } };
		case TEXT('8'): return { FLine{ {L, B}, {R, B}, {R, T}, {L, T}, {L, B} }, FLine{ {L, M}, {R, M} } };
		case TEXT('9'): return { FLine{ {R, M}, {L, M}, {L, T}, {R, T}, {R, B}, {L, B} } };
		case TEXT('L'): return { FLine{ {L, T}, {L, B}, {R, B} } };
		case TEXT('C'): return { FLine{ {R, T}, {L, T}, {L, B}, {R, B} } };
		case TEXT('R'): return { FLine{ {L, B}, {L, T}, {R, T}, {R, M}, {L, M} }, FLine{ {C, M}, {R, B} } };
		default: return {};
		}
	}
}
