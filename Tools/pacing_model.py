"""Economy pacing model: when can the player afford each step of the progression ladder?

Plain Python, no unreal - run it anywhere:

  python Tools/pacing_model.py                      # the shipped figures
  python Tools/pacing_model.py --fee-scale 3        # every landing/parking fee x3
  python Tools/pacing_model.py --fuel-price 3 --build-scale 0.5
  python Tools/pacing_model.py --target-hours 2     # solve the fee scale that paves by then

WHY THIS EXISTS (2026-10-02): research was dropped, so money IS the pace of the game. The
shipped figures were "chosen for internal coherence against 500,000, and all meant to be
tuned" (spec 2026-09-13-ledger-and-fees section 7) and never played. This turns a pacing
TARGET ("paved runway within ~2 real hours") into the fee and build scales that hit it,
instead of tuning prices one playtest at a time.

EXPECTED VALUE, NOT A SIMULATION. Offers per day are the curve's sum, a flight earns its mean
fuel load, and the player accepts a fixed fraction. Variance, lapsed windows and the player's
attention are all folded into ACCEPT_FRACTION. That is enough to say whether a step lands on
day 2 or day 80 - which is the question - and nothing finer should be read from it.

EVERY FIGURE IS COPIED from its authoring source, named beside it. The model does not read the
.uassets, so a value retuned by hand in the editor will not be here. When a source changes,
change it here; a second copy is the price of running without an editor.
"""
import argparse
from dataclasses import dataclass, field

# --- Scenario (UScenario CDO, OpsDefinition.h; build_scenario.py) ---------------------------
STARTING_BALANCE = 500_000.0
REAL_MINUTES_PER_GAME_DAY = (2400.0 + 480.0) / 60.0   # daylight + night real seconds
DAYLIGHT_HOURS = range(6, 21)                         # floor applies in daylight; approximate

# --- Pricing (Pricing.cpp:13; Pricing.h) ----------------------------------------------------
LANDING_FEE_BY_LETTER = {"A": 150.0, "B": 400.0, "C": 1200.0, "D": 2600.0, "E": 4500.0, "F": 7000.0}
PARKING_FRACTION_PER_HOUR = 0.1        # parking = 0.1 x landing fee per parked hour
FUEL_PRICE_PER_LITRE = 1.5             # what the airport charges per litre delivered
FUEL_COST_PER_LITRE = 0.0              # what the airport PAYS - zero in the shipped game
# THE RULED SUPPLY (spec 2026-10-02 section 7), first guesses: a daily take-or-pay contract and
# a dearer spot order. --contract-share is the fraction of litres sold that came in on the
# contract; the rest was bought spot. Waste (contract litres that did not fit) and sales lost to
# a dry depot are both folded into --fuel-loss, a fraction of fuel revenue that never arrives.
FUEL_CONTRACT_PRICE = 0.9
FUEL_SPOT_PRICE = 1.2
# Shipped 2026-10-02 (OpsDefinition.h FFuelSupplyFigures, Tank offer). Checked against the headers; NOT
# fed into the model below: the opening depot already holds one starter tank, and a 30,000 L start is
# worth 27,000 at the contract price (a one-off, under 0.5 day of income). Extra tanks cost 20,000 and
# 100/day upkeep - at the model's end state (936/day upkeep) one tank is ~10% of upkeep, 0.15% of income.
FUEL_TANK_LITRES, FUEL_TANK_COST, FUEL_TANK_UPKEEP = 30_000.0, 20_000.0, 100.0
FUEL_STARTING_STOCK = 30_000.0
FUEL_LOAD_MEAN = 0.7                   # OpsDesignDefaults FuelLoadDraw 0.5-0.9
ELASTICITY = 1.0                       # offers x multiplier^-elasticity (spec D8)

# --- Build costs (build_cost_rates.py; Pavement.cpp:69) ------------------------------------
RUNWAY_RATE_M2 = 25.0
TAXIWAY_RATE_M2 = 13.0
PAVEMENT_FACTOR = {"grass": 0.4, "tarmac": 1.0, "concrete": 1.4, "reinforced": 1.8}
UPKEEP_FRACTION_PER_DAY = 0.001
STAND_COST = 40_000.0
FUEL_DEPOT_COST = 120_000.0
BOWSER_COST, BOWSER_UPKEEP = 90_000.0, 500.0
SHED_COST, SHED_UPKEEP = 40_000.0, 200.0
RUNWAY_WIDTHS = [20.0, 26.0, 30.0, 46.0, 60.0]        # build_road_profiles.py:202
MAX_SPAN_FOR_WIDTH = {20.0: 15.0, 26.0: 24.0, 30.0: 36.0, 46.0: 52.0, 60.0: 80.0}  # IcaoCode.h

# --- Player behaviour (assumed, not authored) -----------------------------------------------
ACCEPT_FRACTION = 0.7       # offers actually taken and flown: lapses, attention, refusals
STAND_HOURS_PER_DAY = 17.0  # hours a stand can turn aircraft; Cumbria flies 06-23
STAND_OCCUPANCY_FACTOR = 2.0  # park-to-pushback over the authored turnaround (waits, push)
RESERVE = 25_000.0          # the player keeps this much back rather than build to zero


@dataclass
class Aircraft:
    name: str
    span: float            # m
    surface: str           # weakest pavement it may use
    takeoff: float         # m, game field length (model roll x 1.1, #368)
    landing: float         # m
    turnaround_s: float
    tank_l: float

    @property
    def letter(self) -> str:
        for letter, limit in (("A", 15.0), ("B", 24.0), ("C", 36.0), ("D", 52.0), ("E", 65.0)):
            if self.span < limit:
                return letter
        return "F"


# Tools/Python/aircraft/planeN.py REQUIREMENTS + TURNAROUND_SECONDS; build_fuel_capacities.py.
# The Meridian's field lengths are AircraftType.cpp:521's fallback - unverified.
AIRCRAFT = {
    "172":        Aircraft("172",        11.00, "grass",  370, 220,  600,  212),
    "Meridian":   Aircraft("Meridian",   13.11, "grass",  380, 510,  600,  454),
    "Cherokee":   Aircraft("Cherokee",    9.14, "grass",  300, 260,  600,  189),
    "SR22":       Aircraft("SR22",       11.68, "grass",  430, 390,  600,  348),
    "Baron":      Aircraft("Baron",      11.53, "grass",  570, 560,  720,  734),
    "Seneca":     Aircraft("Seneca",     11.85, "grass",  430, 440,  720,  466),
    "Twin Otter": Aircraft("Twin Otter", 19.75, "grass",  200, 290,  900, 1466),
    "Caravan":    Aircraft("Caravan",    15.88, "grass",  560, 370,  720, 1268),
    "King Air":   Aircraft("King Air",   17.69, "tarmac", 580, 630,  720, 2040),
    "Saab 340":   Aircraft("Saab 340",   21.44, "tarmac", 970, 780, 1200, 3220),
}


def curve(**hours):
    out = [0.0] * 24
    for key, weight in hours.items():
        out[int(key[1:])] = weight
    return out


@dataclass
class Airline:
    name: str
    fleet: list
    curve: list
    peak: float     # offers per game hour at weight 1.0
    floor: float    # offers per hour held through daylight

    def offers_per_day(self, multiplier: float) -> float:
        demand = multiplier ** -ELASTICITY
        total = 0.0
        for hour, weight in enumerate(self.curve):
            rate = self.peak * weight * demand
            if hour in DAYLIGHT_HOURS:
                rate = max(rate, self.floor)   # the floor no fee takes away
            total += rate
        return total


# build_airlines.py. An offer picks UNIFORMLY among the admissible fleet
# (OfferGenerator.cpp:202), and the airline's rate does not depend on how many types qualify.
AIRLINES = [
    Airline("Flying Club", ["172", "Meridian", "Cherokee", "SR22"],
            curve(h07=0.6, h08=0.8, h09=0.9, h10=1.0, h11=1.0, h12=1.0, h13=1.0, h14=1.0,
                  h15=1.0, h16=1.0, h17=0.7, h18=0.7, h19=0.7), peak=3.0, floor=1.0),
    Airline("Cumbria Air", ["Twin Otter", "King Air", "Caravan", "Baron", "Seneca", "Saab 340"],
            curve(h06=0.8, h07=1.0, h08=1.0, h09=1.0, h10=0.4, h11=0.4, h12=0.4, h13=0.4,
                  h14=0.4, h15=0.4, h16=1.0, h17=1.0, h18=1.0, h19=1.0, h20=0.5, h21=0.5,
                  h22=0.1, h23=0.1), peak=2.0, floor=0.0),
]


@dataclass
class Airport:
    runway_width: float = 0.0
    runway_length: float = 0.0
    runway_surface: str = "grass"
    stands: int = 0
    fuel: bool = False          # depot + bowser + shed
    taxiway_m: float = 0.0
    taxiway_width: float = 12.0
    upkeep: float = 0.0         # per game day

    def admits(self, a: Aircraft) -> bool:
        order = list(PAVEMENT_FACTOR)
        return (self.runway_width > 0.0
                and order.index(self.runway_surface) >= order.index(a.surface)
                and self.runway_length >= max(a.takeoff, a.landing)
                and a.span <= MAX_SPAN_FOR_WIDTH[self.runway_width])


def runway_cost(width, length, surface):
    return width * length * RUNWAY_RATE_M2 * PAVEMENT_FACTOR[surface]


@dataclass
class Step:
    """One purchase on the ladder. cost() reads the airport BEFORE apply() changes it, so a
    re-pave charges only the difference (BuildCost::ForUpgrade) and a lengthening only the
    new metres."""
    label: str
    cost: callable
    apply: callable
    milestone: bool = False   # a progression beat worth reporting the time of


def runway_to(width, length, surface, label, milestone=False):
    def cost(ap: Airport):
        new = runway_cost(width, length, surface)
        old = runway_cost(ap.runway_width, ap.runway_length, ap.runway_surface) if ap.runway_width else 0.0
        return max(0.0, new - old)

    def apply(ap: Airport):
        ap.upkeep -= runway_cost(ap.runway_width, ap.runway_length, ap.runway_surface) * UPKEEP_FRACTION_PER_DAY if ap.runway_width else 0.0
        ap.runway_width, ap.runway_length, ap.runway_surface = width, length, surface
        ap.upkeep += runway_cost(width, length, surface) * UPKEEP_FRACTION_PER_DAY

    return Step(label, cost, apply, milestone)


def stands(n):
    def apply(ap: Airport):
        ap.stands += n
        ap.upkeep += n * STAND_COST * UPKEEP_FRACTION_PER_DAY
    return Step(f"{n} stand(s)", lambda ap: n * STAND_COST, apply)


def taxiway(metres, width=12.0):
    cost = metres * width * TAXIWAY_RATE_M2

    def apply(ap: Airport):
        ap.taxiway_m += metres
        ap.upkeep += cost * UPKEEP_FRACTION_PER_DAY
    return Step(f"{metres:.0f} m taxiway", lambda ap: cost, apply)


def fuel_farm():
    cost = FUEL_DEPOT_COST + BOWSER_COST + SHED_COST

    def apply(ap: Airport):
        ap.fuel = True
        ap.upkeep += FUEL_DEPOT_COST * UPKEEP_FRACTION_PER_DAY + BOWSER_UPKEEP + SHED_UPKEEP
    return Step("fuel depot + bowser + shed", lambda ap: cost, apply, milestone=True)


# THE LADDER. Opening build first, then the beats the progression design names. Code B on
# grass is the start: 26 m admits the Twin Otter, 600 m the Baron.
LADDER = [
    runway_to(26.0, 600.0, "grass", "grass runway 26 x 600"),
    taxiway(200.0),
    stands(2),
    fuel_farm(),
    stands(1),
    runway_to(26.0, 630.0, "tarmac", "PAVE: tarmac 26 x 630 (King Air)", milestone=True),
    runway_to(26.0, 970.0, "tarmac", "LENGTHEN: tarmac 26 x 970 (Saab 340)", milestone=True),
    stands(2),
    # THE FIRST TUBELINER. The 737-800 asks 1,490 m (plane4.py, #368's figure; the A320 1,430),
    # so 30 x 1,500. A Code C airport also re-lays its taxiway at 16 m and adds Code C stands.
    # Nothing ELSE a real Code C airport needs (terminal, Control tier, certification) exists to
    # price yet, so this step is a FLOOR on the cost, and its time a floor on the wait.
    runway_to(30.0, 1500.0, "tarmac", "TUBELINER: tarmac 30 x 1500 (737/A320; no airline yet)", milestone=True),
    taxiway(400.0, 16.0),
    stands(2),
]


@dataclass
class Knobs:
    fee_scale: float = 1.0      # multiplies landing and parking fees (a price change, not the lever)
    fee_lever: float = 1.0      # the in-game LandingFeeMultiplier: scales fee AND demand
    fuel_price: float = FUEL_PRICE_PER_LITRE
    build_scale: float = 1.0    # multiplies every build cost and upkeep
    accept: float = ACCEPT_FRACTION
    fuel_cost: float = FUEL_COST_PER_LITRE
    demand_scales: bool = False   # True: offer rate x qualifying share of the airline's fleet
    contract_share: float = -1.0  # >= 0: fuel bought on the ruled supply, overriding fuel_cost
    fuel_loss: float = 0.0
    start_balance: float = STARTING_BALANCE

    def unit_fuel_cost(self) -> float:
        if self.contract_share < 0.0:
            return self.fuel_cost
        return self.contract_share * FUEL_CONTRACT_PRICE + (1.0 - self.contract_share) * FUEL_SPOT_PRICE


def day_income(ap: Airport, k: Knobs):
    """(income, flights, breakdown) for one game day at this airport."""
    capacity = 0.0
    landing = parking = fuel = flights = 0.0
    occupancy_hours = 0.0
    rows = []
    for airline in AIRLINES:
        admissible = [AIRCRAFT[n] for n in airline.fleet if ap.admits(AIRCRAFT[n])]
        if not admissible or ap.stands == 0:
            continue
        offers = airline.offers_per_day(k.fee_lever) * k.accept
        # DEMAND SCALES WITH THE AIRPORT (ruled 2026-10-02). The shipped generator offers at the
        # airline's full rate whatever share of its fleet qualifies, so paving changed only the
        # MIX. Here the rate is scaled by the qualifying share: a capability that admits a new
        # type brings more flights, not just different ones.
        if k.demand_scales:
            offers *= len(admissible) / len(airline.fleet)
        for a in admissible:
            n = offers / len(admissible)
            fee = LANDING_FEE_BY_LETTER[a.letter] * k.fee_scale * k.fee_lever
            hours = a.turnaround_s * STAND_OCCUPANCY_FACTOR / 3600.0
            rows.append((a, n, fee, hours))
            occupancy_hours += n * hours
    capacity = ap.stands * STAND_HOURS_PER_DAY
    squeeze = min(1.0, capacity / occupancy_hours) if occupancy_hours else 0.0
    for a, n, fee, hours in rows:
        n *= squeeze
        flights += n
        landing += n * fee
        parking += n * fee * PARKING_FRACTION_PER_HOUR * hours
        if ap.fuel:
            litres = n * a.tank_l * FUEL_LOAD_MEAN * (1.0 - k.fuel_loss)
            fuel += litres * (k.fuel_price - k.unit_fuel_cost())
    return landing + parking + fuel, flights, (landing, parking, fuel, squeeze)


def run(k: Knobs, max_days=200, verbose=True):
    ap = Airport()
    balance = k.start_balance
    pending = list(LADDER)
    beats = {}
    day = 0.0

    def hours(d):
        return d * REAL_MINUTES_PER_GAME_DAY / 60.0

    def buy_what_we_can():
        nonlocal balance
        while pending:
            step = pending[0]
            cost = step.cost(ap) * k.build_scale
            if balance - cost < RESERVE:
                return
            balance -= cost
            step.apply(ap)
            pending.pop(0)
            beats[step.label] = day
            if verbose:
                print(f"  day {day:6.1f}  {hours(day):6.1f} h  -{cost:>10,.0f}  {step.label}")

    if verbose:
        print(f"  {'when':>18}  {'cost':>11}  bought")
    buy_what_we_can()
    # Tenth-of-a-day steps: income accrues through the day, and a beat that lands mid-afternoon
    # should not be reported a whole day late.
    dt = 0.1
    while pending and day < max_days:
        income, _, _ = day_income(ap, k)
        balance += (income - ap.upkeep * k.build_scale) * dt
        day += dt
        buy_what_we_can()

    if verbose:
        income, flights, (landing, parking, fuel, squeeze) = day_income(ap, k)
        print()
        print(f"  end state, day {day:.1f}: {flights:.1f} flights/day, income {income:,.0f}/day "
              f"(landing {landing:,.0f}, parking {parking:,.0f}, fuel {fuel:,.0f}), "
              f"upkeep {ap.upkeep * k.build_scale:,.0f}/day, stand squeeze {squeeze:.2f}")
        if pending:
            print(f"  NOT REACHED in {max_days} days: " + ", ".join(s.label for s in pending))
    return beats


def report_income_by_airport(k: Knobs):
    """Daily income at each runway configuration, all else equal - what each beat is WORTH."""
    print("  income per game day by runway (3 stands, fuel farm):")
    for label, w, l, s in [("grass 26x600", 26, 600, "grass"), ("tarmac 26x630", 26, 630, "tarmac"),
                           ("tarmac 26x970", 26, 970, "tarmac")]:
        ap = Airport(runway_width=w, runway_length=l, runway_surface=s, stands=3, fuel=True)
        income, flights, (landing, parking, fuel, _) = day_income(ap, k)
        print(f"    {label:14} {income:>9,.0f}  ({flights:.1f} flights; landing {landing:,.0f}, "
              f"parking {parking:,.0f}, fuel {fuel:,.0f})")


def solve(k: Knobs, target_hours, beat, knob):
    """Bisect `knob` until `beat` lands at target_hours of real play."""
    lo, hi = 0.1, 50.0
    for _ in range(40):
        mid = (lo + hi) / 2
        setattr(k, knob, mid)
        when = run(k, verbose=False).get(beat)
        hrs = when * REAL_MINUTES_PER_GAME_DAY / 60.0 if when is not None else float("inf")
        if hrs > target_hours:
            lo = mid
        else:
            hi = mid
    setattr(k, knob, hi)
    return hi


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--fee-scale", type=float, default=1.0)
    p.add_argument("--fee-lever", type=float, default=1.0)
    p.add_argument("--fuel-price", type=float, default=FUEL_PRICE_PER_LITRE)
    p.add_argument("--build-scale", type=float, default=1.0)
    p.add_argument("--accept", type=float, default=ACCEPT_FRACTION)
    p.add_argument("--fuel-cost", type=float, default=FUEL_COST_PER_LITRE)
    p.add_argument("--demand-scales", action="store_true", help="offer rate x qualifying fleet share")
    p.add_argument("--contract-share", type=float, default=-1.0,
                   help="fraction of fuel on contract (rest spot); enables the ruled supply")
    p.add_argument("--fuel-loss", type=float, default=0.0, help="fraction of fuel sales lost/wasted")
    p.add_argument("--start-balance", type=float, default=STARTING_BALANCE)
    p.add_argument("--target-hours", type=float, help="solve --solve-knob so the PAVE beat lands here")
    p.add_argument("--solve-knob", default="fee_scale", choices=["fee_scale", "fuel_price", "build_scale"])
    a = p.parse_args()
    k = Knobs(a.fee_scale, a.fee_lever, a.fuel_price, a.build_scale, a.accept, a.fuel_cost,
              a.demand_scales, a.contract_share, a.fuel_loss, a.start_balance)

    if a.target_hours is not None:
        beat = next(s.label for s in LADDER if s.label.startswith("PAVE"))
        value = solve(k, a.target_hours, beat, a.solve_knob)
        print(f"{a.solve_knob} = {value:.2f} paves within {a.target_hours} real hours\n")

    print(f"knobs: {k}\n")
    run(k)
    print()
    report_income_by_airport(k)


if __name__ == "__main__":
    main()
