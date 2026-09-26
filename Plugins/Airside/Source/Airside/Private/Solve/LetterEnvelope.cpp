#include "Solve/LetterEnvelope.h"

FLetterEnvelopeTable FLetterEnvelopeTable::Floor()
{
	FLetterEnvelopeTable Table;
	for (uint8 Index = 0; Index < UE_ARRAY_COUNT(Table.Envelopes); ++Index)
	{
		Table.Envelopes[Index] = IcaoCode::FloorEnvelopeForLetter(static_cast<EIcaoCode>(Index));
	}
	return Table;
}
