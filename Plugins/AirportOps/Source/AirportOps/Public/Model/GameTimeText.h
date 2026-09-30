#pragma once

#include "CoreMinimal.h"

/**
 * GAME TIME AS THE PLAYER READS IT - the one formatter (#447). Game time was worded in five places: the offer row's
 * DescribeDuration ("1 h 35 min", which the arrivals rows and the aircraft card borrowed), the depot card's own "+95 min" and
 * "late 4 min" printed in whole minutes by the job board, and "Day %d  %02d:%02d" twice (the bar's clock and the ledger's rows) plus
 * a third in a log line. So the depot card said "+95 min" beside the aircraft card's "1 h 35 min" for the same span.
 *
 * IN AirportOps, NOT IN THE GAME MODULE WHERE DescribeDuration LIVED: the job board words a duration too (the depot backlog), and the
 * ops layer may not call up into the game module. Every reader of the clock - the board, the bar, the ledger, the cards - goes
 * through here, and `%02d:%02d` is written in GameTimeText.cpp alone.
 * ENFORCED BY: Check-Architecture rule 4's 'game-clock text' row (the pattern appears in no other file)
 */
namespace GameTimeText
{
	/**
	 * "15 min", "1 h", "1 h 35 min": WHOLE MINUTES, rounded, never negative, never seconds - the clock's own words for a span. The
	 * arrivals rows, the aircraft card, the offer row and the depot backlog all word a duration through this, so two cards describing one
	 * span cannot differ.
	 */
	AIRPORTOPS_API FText Duration(double Seconds);

	/** "14:05" - a time of day in game seconds since midnight, hours and minutes TRUNCATED, as a clock face shows them. */
	AIRPORTOPS_API FString TimeOfDay(double TimeOfDaySeconds);

	/**
	 * "Day 3  14:05" - a game INSTANT (seconds since the game began; USimClock::Now, a ledger entry's At). The day is 1-based and read off the
	 * seconds the same way USimClock::Day reads it (+1), so a stamp of the clock's own Now() cannot disagree with the clock's day.
	 */
	AIRPORTOPS_API FString Stamp(double GameSeconds);
}
