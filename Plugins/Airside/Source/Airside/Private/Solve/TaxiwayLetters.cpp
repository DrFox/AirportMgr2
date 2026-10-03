#include "Solve/TaxiwayLetters.h"

namespace
{
	/** The 23, in order. A literal rather than A..Z filtered at run time: the order IS the issue order. */
	const TCHAR* const TaxiwayLettersAlphabet = TEXT("ABCDEFGHJKLMNPQRSTUVWYZ");
}

FString TaxiwayLetters::LetterAt(int32 Index)
{
	// BIJECTIVE BASE 23 (no zero digit), so Z is followed by AA, not BA - the spreadsheet-column rule the spec's
	// "then AA, AB" names.
	FString Out;
	for (int64 N = static_cast<int64>(FMath::Max(Index, 0)) + 1; N > 0; N = (N - 1) / Count)
	{
		Out.InsertAt(0, TaxiwayLettersAlphabet[(N - 1) % Count]);
	}
	return Out;
}

bool TaxiwayLetters::IsAvoided(TCHAR Character)
{
	const TCHAR Upper = FChar::ToUpper(Character);
	return Upper == TEXT('I') || Upper == TEXT('O') || Upper == TEXT('X');
}
