#pragma once

// Host-side player controllers (docs/AI.md §2): they read the sim and emit Commands, like a
// player. They are not part of the sim: replays and determinism checks replay their commands, not
// them. These two are deliberately simple (M6 vertical slice), rule-based and deterministic
// (integer and Fixed maths over the sim's ascending-id pools), so a headless play-test reproduces.
//
//  * Dummy opponent (makeDummyAi): deploys an Infantry Division HQ and a Motor Pool, keeps oil and
//    ammo out of the red with depots, recruits a few regiments (rifle and AT infantry), defends its buildings
//    against anything that comes near, and every few minutes sends all but a home guard as a wave
//    at the enemy Corps HQ.
//  * Slice player (makeSlicePlayer): plays the vertical slice the way MILESTONES.md describes it:
//    Corps HQ -> Infantry Division HQ, Motor Pool (AT infantry needs it), Mechanized and Armour
//    Division HQs (the Armour HQ needs the Mechanized one) -> rifle infantry, AT infantry, light and medium
//    tanks -> takes every city -> once its army is big enough, attacks the enemy's HQs (the Corps
//    HQ first, which deploys the others) one after another until the enemy is eliminated, sending
//    reinforcements in groups. It defends its base like the dummy.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/command.hpp"

namespace corps {
class Sim;
}

namespace corps::ai {

class Controller {
public:
    virtual ~Controller() = default;
    // Called before each tick runs (the commands are for that tick: the host sets tick and seq).
    // Thinks at most once a second.
    virtual void think(const Sim& sim, std::vector<Command>& out) = 0;
    std::uint8_t player() const { return me; }
    // What it decided, one line each, oldest first (the host drains it for logs).
    std::vector<std::string> notes;

protected:
    std::uint8_t me = 0;
};

// What a controller does: see the file comment.
struct Plan {
    std::vector<std::string> buildings; // buildings the Corps HQ deploys, in order (HQs, motor pool, ...)
    std::vector<std::string> roster;    // regiments to recruit, cycled (skipped while blocked)
    std::uint32_t armyCap = 6;          // fighting regiments (not HQs or trucks), queued included
    bool depots = true;                 // deploy oil / ammo depots while their balance is negative
    bool takeCities = false;            // send a regiment to every city the team doesn't hold
    std::uint32_t firstWaveTicks = 0;   // first attack wave (0: no waves)
    std::uint32_t waveTicks = 0;        // between waves
    std::uint32_t keepHome = 2;         // regiments a wave leaves at home
    std::uint32_t pushAt = 0;           // attack every enemy HQ once the army has this many (0: never)
    std::int32_t defendRadius = 45;     // metres round each own building
};

Plan dummyPlan(const std::string& nation);
Plan slicePlan(const std::string& nation);

std::unique_ptr<Controller> makeController(std::uint8_t player, const std::string& nation, const Plan& plan);
std::unique_ptr<Controller> makeDummyAi(std::uint8_t player, const std::string& nation);
std::unique_ptr<Controller> makeSlicePlayer(std::uint8_t player, const std::string& nation);

}  // namespace corps::ai
