"""DA_Aircraft_Plane7 / ABP_Plane7 - the Piper PA-46 Meridian.

THIS SPEC AUTHORS NO PERFORMANCE FIGURE, AND THAT IS THE DIFFERENCE BETWEEN IT AND ITS THIRTEEN
SIBLINGS. Every other aircraft/<key>.py carries its aeroplane's published numbers, because those
types exist nowhere else. The Meridian is not like that: it is ALSO
UAirsideSettings::ResolveDefaultAirframe's fallback, so its figures live in C++ -
UAircraftType::BuildPiperMeridian and the PiperMeridian*() statics beside it - and a second copy
here would be precisely the failure AirsideContent.h names in as many words: "#30 was seven call
sites hardcoding a Piper's numbers while the MESH already came from here".

So `cpp_builder="build_piper_meridian"` below routes authoring through
`unreal.AircraftType.build_piper_meridian()` to lay the type down from the one description,
exactly as build_stand_asset.py did when the asset was first created, and
`build_aircraft_type._author_via_cpp_builder` then sets only:

  * THE MESH AND THE ANIM CLASS. Content, which Airside may not name - Check-Architecture
    enforces the include direction.
  * THE FIGURES FAircraft's C++ STRUCT HAS NO FIELD FOR - main gear track and propeller
    diameter - MEASURED off the export.
  * THE GEAR CYCLE, which is a new capability rather than a new number: see GEAR below.

OWNERSHIP MOVED HERE FROM build_stand_asset.py, which authored DA_Aircraft_Piper as one of three
aircraft it needed to size a stand. Its reason for the asset existing at all is worth carrying
rather than losing: M_Starter's runway is 15 m wide and admits a 15 m wingspan, so neither Code
C type can ever land there and every offer was refused at the gate. A light type an airline can
actually list is what gives a GA field traffic - and what makes the jets a REASON to build a
wider runway rather than a broken inbox.

THE RENAME, NOT A NEW ASSET (`old_name="DA_Aircraft_Piper"`). DA_Airline_Cumbria's fleet and
DA_AirsideContent's DefaultAircraft both point at this object. A create-and-delete would break
both references rather than update them; a rename fixes them up by construction. It runs once
and is a no-op on every later run.

WHAT THIS DOES NOT TOUCH. TurnaroundSeconds and PushbackNeed are authored on the existing asset
and are decisions about the AEROPLANE, not about the model that replaced its mesh. Re-deciding
them in the same breath as an import would be exactly the scope creep that hides a real change
inside a mechanical one. They are READ BACK and reported, so a reader can see what survived.

THE GEAR CYCLE - A NEW CAPABILITY, NOT A NEW NUMBER. SM_PiperMeridian had no retract bones at
all, so this type shipped with an empty Gear block: a Meridian's gear stayed down through the
whole climb, on screen, because there was nothing in the rig to fold. plane7 retracts and has
doors at both ends, so the block can be authored for the first time - and
UAircraftType::BuildPiperMeridian deliberately does NOT author it, for the reason
Airside.Model.Gear737IsAuthoredAndTravels pins in both directions: the ASSET declares a cycle
and the C++ builder does not, so an edit that "completes" the builder by copying these figures
back goes red.

SIX SECONDS AND ONE, plane5's figures and for plane5's reason. A PA-46's gear is hydraulically
actuated and quick; VLO is 168 KIAS, which is a gear designed to be used briskly. Copying a
fleet-mate's procedure timings is right where nothing distinguishes the two aeroplanes.
"""
from build_aircraft_type import AircraftSpec, Typed

TYPE_NAME = "DA_Aircraft_Plane7"
OLD_NAME = "DA_Aircraft_Piper"
ABP = "/Game/Aircraft/Plane7/ABP_Plane7"
MESH = "/Game/Aircraft/Plane7/SK_Plane7"
SOURCE = r"C:\repos\AirportMgr2Models\plane7\export\plane7.glb"

# THE GEAR CYCLE - see the header.
GEAR = {
    "travel_seconds": 6.0,
    "door_seconds": 1.0,
    "retract_above_height": 9000.0,
    "extend_below_height": 15000.0,
}

# FOUR BLADES - a Hartzell HC-E4N-3Q, TCDS A25SO, and what plane7/scripts/build_prop.py arrays.
PROP_BLADE_COUNT = 4

# NINETY DEGREES, where UAirsideAgentAnim defaults to plane4's measured 81. plane7/scripts/
# build_rig.py's CYCLE keys the doors at 0 (hanging open) and 90 (closed).
BAY_DOOR_CLOSED_ANGLE_DEGREES = 90.0


def get_spec():
    return AircraftSpec(
        key="plane7",
        type_name=TYPE_NAME,
        old_name=OLD_NAME,
        cpp_builder="build_piper_meridian",
        # short_code/display_name/code are authored by the C++ builder, not here.
        short_code="",
        display_name="",
        code="",
        mesh=MESH,
        abp=ABP,
        source=SOURCE,
        wing="wing",
        stabiliser="tailplane",
        gear_leg="maingear_L",
        prop="prop",
        gear=GEAR,
        angles=Typed(door=BAY_DOOR_CLOSED_ANGLE_DEGREES),
        prop_blade_count=PROP_BLADE_COUNT,
        # ground/steering/climb/approach/engine/requirements/turnaround_seconds are ALL authored
        # by unreal.AircraftType.build_piper_meridian() - a second copy here is the failure the
        # header names. Left at their AircraftSpec defaults ({}), which the cpp_builder path
        # never reads for authoring (only the ground-regime read-back loop touches the asset's
        # OWN saved values, not this spec's).
        pushback_need="SELF_MANOEUVRE",
        pushback_reason=(
            "the starter aeroplane reverses itself, so a new airport needs no depot"
        ),
        yard_label="Plane7 (PA-46 Meridian)",
    )
