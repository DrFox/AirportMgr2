# Progression and fuel supply - design

2026-10-02. Rulings from a design conversation with the owner; replaces GDD §13 (Research)
as the progression spine and fills in GDD §12's fuel contract, which was written and never
built. Figures are first guesses for the pacing model (`Tools/pacing_model.py`) to tune.

## 1. Rulings

| Topic | Ruling |
|---|---|
| Research | **Dropped.** No tree, no points. Its unlocks are rehomed (§8). |
| Spine | **What you build is the unlock**, and **certification** names the goal. |
| Building tiers | Unlocked by **traffic milestones** on four tracks. |
| Direction | **Airlines pull you forward**, up a ladder that ends with one airline basing itself at your airport. |
| Room | **Land** is bought with money. |
| Eras | **Rejected**: needs many more aircraft models, and Sky Haven did it. |
| Demand | **Scales with the airport**: admitting more of an airline's fleet brings more of its flights. |
| Fuel | Has a **purchase cost**: spot or contract, with storage gating contract tiers (§7). |
| Pace | **Money is the pace** of the game now. First beat: a paved runway within ~2 real hours. |

## 2. Four axes, not one ladder

| Axis | Driven by | Unlocks |
|---|---|---|
| Size - what can land | Certification: build the infrastructure, pass the audit | Aircraft classes (ICAO A-F) |
| Scale - how big buildings are | Traffic milestones | Building tiers: portacabin, brick, glass |
| Direction - what to build next | Airline ladder | Contracts, then a based airline |
| Room - where you can build | Money | Adjacent land plots |

Splitting them is the point. Milestones reward VOLUME, which is a fair thing to reward with
bigger buildings - volume is what a bigger terminal is for. Aircraft size stays earned by
CAPABILITY, so an airport cannot grind its way to a 737 on 172s.

## 3. Certification

- The airport holds a **category**, named by the largest aircraft class it is licensed for.
- Each category has a **checklist** of infrastructure: runway length, width and surface,
  strip and clearance, approach aid, fire cover (when it exists). Most of these checks
  exist already as `RunwayAdmission`, the strip work and stand admission; certification
  gathers them into one named goal.
- The player **requests an audit**. It passes, or returns the failing items by name.
- **A failed audit blocks the next category only.** It never suspends the current one.
- Offers for a class arrive only once the airport is certified for it. Today admission is
  per-flight physics with no licence; certification is the layer above that the player
  can read.

## 4. Traffic milestones

Four tracks, each counting a running figure: **passengers**, **movements**, **cargo**,
**revenue**. Each track unlocks the tiers of the buildings that serve it, so the player can
specialise:

| Track | Unlocks (first cut) |
|---|---|
| Passengers | Terminal tiers, retail, car parking |
| Cargo | Goods depot and warehouse tiers; larger fuel contract tiers (§7) |
| Movements | Control tiers, ground services depot tiers |
| Revenue | Editable fees, fuel sell price |

The tracks feed one another - passenger aircraft carry belly cargo, every flight is a
movement, everything earns - so a specialist still advances on all four, more slowly.

## 5. The airline ladder

1. **Ad-hoc offers** - today's inbox.
2. **Contract** - a fixed number of flights per day (GDD §6). Offered once the airline's
   reputation AND a milestone (for example, a passenger count) are both met.
3. **Focus** - the airline routes N daily rotations through the airport.
4. **Base** - the airline keeps aircraft at the airport overnight. New demands: night
   stands, hangar or line maintenance, crew facilities, early departure peaks. Large
   guaranteed income; its routes draw other airlines.

**One base only.** A rival airline may **counter-offer** to replace the based airline, so
the base is never permanent. Service that slips lets the base threaten to leave, which gives
the late game stakes without a fail screen.

## 6. Demand scales with the airport

Today `UOfferGenerator` offers at an airline's full rate whatever share of its fleet the
airport admits, and picks uniformly among the admissible types (`OfferGenerator.cpp:202`).
So paving the runway changes the MIX of Cumbria's flights and not the NUMBER - the pacing
model put the value of the whole grass-to-970 m-tarmac climb at +30% income.

First cut: the rate is scaled by the admissible share of the fleet. With this the same climb
is worth +65%. Whether a newly opened class also brings a new airline is §5's business.

## 7. Fuel supply

### Before (to 2026-10-02)

Superseded by Ruled below.

The depot holds unlimited fuel for free: a returning bowser refills at the pump rate
(`ServiceRolePolicy.cpp:42`) and nothing is bought. Fuel is pure margin at 1.5 per litre (the first draft's price; shipped 2.0),
about 75% of a small airport's income.

### Ruled

- **Stock.** ONE airport-wide pool of litres; bowser refills draw it down. **Deviation from the
  first draft ("the depot holds litres")**: depots can be many and a per-depot stock would need a
  tanker route between them. Capacity is derived, not stored: seated Tank modules x 30,000 L across
  live depots. Stock above capacity (tanks sold) is kept, never topped up. A new game starts with
  30,000 L (one full starter tank, 27,000 at contract price).
- **Capacity.** Tank module 20,000, upkeep 100/day.
- **Contract.** Tiers 5k/10k/20k/40k L/day at 0.9, 7-day term, cancellable (cancel charge = days left
  x litres x price x 0.5). **Take-or-pay** at day end: paid in full, only what fits is added.
  **Tier gate: `LitresPerDay <= capacity`.** While contracted the card offers the next tier up (fresh
  term, no charge); a downgrade is a cancel. Milestone gating of tiers is deferred until the cargo
  track exists.
- **Spot.** 1.2/L, charged on order, arrives after 7,200 game seconds. Refused if no room (free space
  minus pending orders) or unaffordable. Deliveries are polled every game minute: orders' due times
  are saved, the clock queue is not, so the poll re-arms itself after a load.
- **Dry depot.** `EServiceRefusal::NoFuelStock`. Partial service beats none; the flight leaves at its
  deadline with what it got. The airline `ShortfallPenalty` is unchanged (satisfaction hit, no fine).
  A delivery re-opens `NoFuelStock` jobs.
- **FuelLow alert.** Stock + pending spot < 25% of capacity, no contract, capacity > 0.
- **Bought vehicles** still arrive full (owner ruling; a bowser comes with its first load).
- **Sell price** stays fixed until the revenue track unlocks editing it.

The skill this creates is forecasting: too large a contract pays for fuel poured away, too
small a contract pays spot prices or loses sales.

Shipped prices: sell 2.0 (UPricing::FuelPricePerLitre; the model takes it via --fuel-price), contract 0.9, spot 1.2 per litre.

### Later

- **Delivered tanker** on the landside road. A jammed road starves the airport. Waits for a
  landside road, which does not exist yet.
- **Market price** that moves over time.

## 8. Where research's unlocks go

| Was research (GDD §13) | Now |
|---|---|
| Fuel depot, pushback | Buildable for money |
| Asphalt, concrete, PAPI, ILS, windsock | Buildable for money; certification asks for them |
| Faster pumps, loading, vehicle speed | Building tiers and add-on modules |
| Passenger handling, international, baggage | Terminal tiers (passengers track) |
| Control tiers, radar | Movements track |
| Scheduling | Arrives with the first airline contract, when it is first needed |
| Negotiation | Airline ladder: a price multiplier from reputation (GDD §12) |
| Editable fees | Revenue track |
| Retail, car parking | Passengers track |

## 9. Pacing

With research gone, build costs, fees, fuel margin and the starting balance ARE the pace.
`Tools/pacing_model.py` turns a target time per step into the scales that hit it.
Expected-value only; it says whether a step lands on day 2 or day 80, nothing finer.

Findings on the shipped figures (2026-10-02, 70% of offers accepted, 3 stands):

| Configuration | Pave | Saab | Code C |
|---|---|---|---|
| Shipped | 11.5 h | 16.6 h | 23.3 h |
| + demand scales | 14.6 h | 20.2 h | 27 h |
| + fuel costs 1.0/L | 24 h | 36 h | 51 h |
| Ruled supply (80% contract at 0.9, 20% spot at 1.2, 10% fuel lost) | 24.6 h | 36.6 h | 51.8 h |
| Ruled supply, builds x0.2, start 153,000 | **2.2 h** | 4.4 h | 7.2 h |

**Building costs about five times what earning pays for at this pace.** Builds x0.2 with a
starting balance of opening build + 50,000 hits the 2-hour target. Scaling income up x5
instead is the same ratio with bigger numbers. Which one is a question of how the numbers
should FEEL, not of pacing.

**Ruled: raise income, keep builds looking expensive** - "people don't know about landing
fees, but runways look expensive". Chosen scales: **landing and parking fees x5** (Code B
2,000), **builds x0.4**, **fuel sold at 2.0** so fuel stays a meaningful share (~30% of income).
Starting balance = opening build + 50,000 (about 257,000).

| Step | Real hours |
|---|---|
| Pave (King Air) | 1.6 |
| Lengthen to 970 m (Saab) | 2.9 |
| First tubeliner: 30 x 1,500 m (737-800 asks 1,490 m) | 5.7 |

Owner's target for the first tubeliner: about **7 hours**. The model's 5.7 h is a FLOOR: it
prices only runway, taxiway and stands, because the terminal, Control tier and Code C
certification that a real tubeliner step needs do not exist to price yet.

**Fuel supply shipped (2026-10-02):** re-run with contract 0.9 / spot 1.2, the command in `pacing_model.py`'s
header scenario (`--contract-share 0.8 --fuel-loss 0.1 --fee-scale 5 --fuel-price 2.0 --build-scale 0.4
--start-balance 260000`) gives Pave 1.6 h, Saab 2.9 h, tubeliner 5.7 h: unmoved, since the model already used
those prices. Not modelled: the 30,000 L starting stock (27,000 at contract price, a one-off) and tank upkeep
(100/day per tank).

**Other services will add income** - cargo, passengers, catering and the rest. Each one
speeds the whole ladder, so each re-solves the scales when it lands: price a new service so
that it pays for its own building, and re-run the model to keep the steps on target.

- **Landing fees barely matter**: hitting 2 h on fees alone needs them x14.
- **The build scale is very sensitive against the starting 500,000.** At half the build cost the
  pave is affordable on day 0 and the first beat disappears. Starting balance and build
  scale are tuned together.
- Target: the opening build leaves about 50,000, and the pave lands near hour 2.

Field lengths are already the game's own (#368: model roll x 1.1, no engine-out margin).
The Saab's 970 m is correct against that ruling; its published figure is 1,300 m.

## 10. Open questions

- Real-time targets for the steps after paving: the Saab, Code C, the airline base.
- The certification checklist per category, and whether an audit costs money or time.
- Milestone thresholds per track, and whether the tracks count totals or rates per day.
- Land: plot size and price curve; whether the map starts with a fixed plot.
- What the based airline's demands and income are, in figures.
- Rival airports: raised, not designed.
