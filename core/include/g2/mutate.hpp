// Parameter randomizer and Patch Mutator, as in Clavia's editor (v1.62,
// CMutaSynthData / CDialogMuta*; see re/notes/randomize-mutate.md).
//
// The original works on "individuals": a full set of parameter values of one
// variation (patch settings, VA and FX modules) plus that variation's morph
// assignments. Values are doubles in [0, max + 0.999]; the patch gets
// floor(value). A child is made from one parent by Randomize or Mutate, from
// two by Cross (recombination) or Interpolate. Locked modules and a fixed set
// of parameter classes (switches, mode selectors, MIDI settings, ...) are
// never changed by Randomize or Mutate; the dialog's "quick locks" and "solo"
// buttons exclude or select whole parameter groups.
//
// Random numbers come from BSD random() (the algorithm of the original's
// libc, which it never seeds: Random(1) reproduces it).
#pragma once

#include "g2/patch.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace g2::mutate {

// ---- Random numbers -----------------------------------------------------------

// BSD random(): additive feedback generator x[i] = x[i-3] + x[i-31] (TYPE_3),
// seeded with the Park-Miller generator and 310 discarded outputs. Same
// sequence as macOS/FreeBSD srandom()/random() (and glibc for seeds != 0).
class Random {
public:
    explicit Random(std::uint32_t seed = 1) { reseed(seed); }
    void reseed(std::uint32_t seed);
    std::int32_t next();                                  // 0 .. 2^31 - 1
    double unit() { return next() / 2147483647.0; }      // [0, 1], as the editor divides

private:
    std::array<std::uint32_t, 31> state_{};
    int front_ = 3, rear_ = 0;
};

// ---- Parameter classes and groups ----------------------------------------------

// The editor's param class of a parameter (param spec byte 0x12); nullopt for
// an unknown type or parameter index.
std::optional<u8> paramClass(u8 moduleType, std::size_t param);

// False for the 51 classes Randomize/Mutate never touch (on/off switches, KBT,
// modes and ranges, sequencer length/controls, MIDI channel/CC, ...).
bool isMutableClass(u8 paramClass);

// Distribution used for a class. Linear: uniform. Low/High/Centre: uniform in
// a warped domain, so random values crowd towards the low end (times),
// the high end, or the middle (pitch).
enum class Curve : u8 { Linear = 1, Low = 2, High = 3, Centre = 4 };
Curve curveOf(u8 paramClass);
// ApplyCurve: a new value for a parameter with range 0..max. x < 0 draws a
// fresh value (Randomize); otherwise x moves by up to `range` (Mutate). May
// leave [0, max + 0.999]; callers clamp (Randomize) or reflect (Mutate).
double applyCurve(Curve curve, int max, double x, double range, Random& rng);

// The dialog's quick-lock groups (bit values of CMutaSynthData+0x390/0x394).
enum Group : u8 {
    OscFreq = 0x01,  // osc coarse / pitch mod, DrumSynth tune, Operator coarse/fine
    OscFine = 0x02,  // osc fine, Operator detune
    Mixer = 0x04,    // mixer levels, inverts, pans, fades (no button in the dialog)
    Envelope = 0x08, // envelope times/levels/shapes
    SeqValue = 0x10, // SeqVal / SeqNote step values
    SeqEvent = 0x20, // sequencer step events (on/off)
    Delays = 0x40,   // every parameter of the Delay modules
    Effects = 0x80,  // every parameter of the FX modules (Reverb, Flanger, ...)
};
// The group of a parameter, or nullopt for an ungrouped one.
std::optional<Group> groupOf(u8 moduleType, u8 paramClass);

inline constexpr double kDefaultProbability = 0.3721; // 0x3fd7d07c84b5dcc6
inline constexpr double kDefaultRange = 0.189;        // 0x3fc83126e978d4fe
inline constexpr double kDefaultCrossover = 0.15;
inline constexpr double kMaxRange = 0.5; // the dialog's Range knob: 0-50 % (CSmallKnob(..., 0x32))

// The "Link" relation between mutation probability and range.
double rangeForProbability(double probability); // clamped to [0, 0.5]
double probabilityForRange(double range);       // clamped to [0, 1]

struct Settings {
    double probability = kDefaultProbability; // chance that a parameter mutates
    double range = kDefaultRange;             // max displacement, fraction of the param's range
    double crossover = kDefaultCrossover;     // chance to switch parent after each parameter
    bool link = true;
    u8 unlocked = 0xFE; // bit set: group may change (OscFreq quick-locked by default)
    u8 solo = 0;        // non-zero: only these groups change, ungrouped params don't

    // As the dialog's knobs (probability 0..1, range 0..kMaxRange, clamped):
    // with link on, the other value follows.
    void setProbability(double p);
    void setRange(double r);
    void setLink(bool on);
    // As the quick-lock check boxes (ignored while a solo button is on).
    void setQuickLock(Group g, bool locked);
    bool quickLocked(Group g) const { return ((solo ? solo : unlocked) & g) == 0; }
    void setSolo(Group g, bool on);
};

// CMutaSynthData::IsEnabled: whether Randomize/Mutate may change a parameter
// of a module (false for locked modules and non-mutable classes).
bool isEnabled(const Module& module, std::size_t param, const Settings& settings);

// ---- Individuals ------------------------------------------------------------------

struct ModuleGenes {
    u8 index = 0;
    u8 type = 0;
    std::vector<double> values; // one per parameter, in [0, max + 0.999]
    bool operator==(const ModuleGenes&) const = default;
};

struct Individual {
    std::vector<ModuleGenes> settings, va, fx; // ascending module index
    std::vector<MorphAssign> morphs;           // the variation's morph assignments
    bool operator==(const Individual& o) const;

    std::vector<ModuleGenes>& area(Location loc);
    const std::vector<ModuleGenes>& area(Location loc) const;
    const ModuleGenes* find(Location loc, u8 index) const;
    // The integer values the patch would get (floor, clamped).
    std::vector<u8> quantized(Location loc, u8 index) const;
};

// Captures a variation. With a generator, each value gets a uniform [0, 1]
// fraction (CModuleParamData<double>::Load, how the editor fills its mirror
// of the patch); without, v + 0.5 (CMutaSynthData::SetParamValue, after an edit).
Individual capture(const Patch& patch, u8 variation, Random* dither = nullptr);
// Writes floor(value), clamped to the parameter's range, into a variation of
// the modules the patch has, and replaces that variation's morph assignments
// (CPatch::HandleMutaParamDumpMolecule).
void apply(Patch& patch, u8 variation, const Individual& ind);

// ---- Operators (one child) ------------------------------------------------------------

// Which modules an operator may change on top of the lock / class / group
// filter; an empty filter allows all.
using ModuleFilter = std::function<bool(Location, u8 module)>;

// CopyRandomizeContext for VA then FX: every enabled parameter gets a new
// random value (curve distribution); patch settings and morphs are copied.
Individual randomized(const Patch& patch, const Individual& parent, const Settings& s, Random& rng,
                      const ModuleFilter& filter = {});
// CopyMutateContext for VA then FX: each enabled parameter mutates with
// probability s.probability, moving by up to s.range of its range (curve-
// warped), reflected at the ends.
Individual mutated(const Patch& patch, const Individual& parent, const Settings& s, Random& rng,
                   const ModuleFilter& filter = {});
// RecombineContext for settings, VA, FX: a random parent to start each area,
// then after every parameter switch parent with probability `crossover`.
// Values and morphs come from the parent in use. Ignores locks and classes
// (as the original).
Individual recombined(const Individual& mother, const Individual& father, double crossover, Random& rng);
// InterpolateContext: mother + (father - mother) * t for values and morph
// ranges (truncated; a zero range drops the assignment).
Individual interpolated(const Individual& mother, const Individual& father, double t);

// ---- Convenience: in-place randomize / mutate -----------------------------------------------

// Not a command of the original (its Randomize/Mutate only fill the
// Mutator's children); this applies one child directly to variations.
struct Scope {
    std::optional<u8> variation;                     // nullopt: user variations 0..7, each independently
    std::vector<std::pair<Location, u8>> modules;    // empty: every module
};
void randomize(Patch& patch, const Scope& scope, const Settings& s, Random& rng);
void mutate(Patch& patch, const Scope& scope, const Settings& s, Random& rng);

// ---- The Mutator ------------------------------------------------------------------------------

inline constexpr int kChildren = 6;     // population boxes 0..5
inline constexpr int kMotherBox = 6;    // population box of the mother
inline constexpr int kFatherBox = 7;    // population box of the father
inline constexpr int kGeneBankSize = 24; // "Temporary Storage": 3 rows of 8

enum class BoxKind : u8 { Population = 0, GeneBank = 1, Variation = 2 };
struct Box {
    BoxKind kind = BoxKind::Population;
    int index = 0;
    bool operator==(const Box&) const = default;
};
inline constexpr Box kMother{BoxKind::Population, kMotherBox};
inline constexpr Box kFather{BoxKind::Population, kFatherBox};

// The Mutator dialog's state without the UI (CMutaSynthData). Variation boxes
// read and write the patch through a dithered mirror, as the original.
class Mutator {
public:
    explicit Mutator(const Patch& patch, std::uint32_t seed = 1);

    Settings settings;
    std::array<std::optional<Individual>, 8> population;
    std::array<std::optional<Individual>, kGeneBankSize> geneBank;
    int parentCount = 0;           // 1 after Randomize/Mutate, 2 after Cross/Interpolate (box colours)
    std::optional<Box> focus;      // the individual being auditioned

    Random& rng() { return rng_; }
    // Updates the mirror after the patch changed outside the Mutator: edited
    // values become v + 0.5, new modules v + dither; removed modules go away
    // (also from every box).
    void sync(const Patch& patch);

    bool hasData(Box b) const;
    std::optional<Individual> get(Box b) const;

    // Six children from a variation (Randomize button).
    void randomize(const Patch& patch, u8 variation);
    // Six mutants of a box (Mutate: mother; double-click: that box); the
    // parent is copied to the mother box first.
    bool mutate(const Patch& patch, Box parent);
    // Six children of two boxes (Cross: mother x father; alt-drag: a onto b),
    // copied to mother/father first.
    bool cross(Box mother, Box father);
    // Six interpolations at 1/7 .. 6/7 (Interpolate; shift-drag).
    bool interpolate(Box mother, Box father);
    // Click on a box (CDialogMutaBackground::Click → GetFocusIndividMolecules
    // @00142ef8): plays the individual in the audition variation (9, added to
    // the patch if needed, edit::addAuditionVariation) and focuses it; the
    // user variations 1-8 are not touched. The caller makes variation 9 the
    // patch's active one. Returns false for an empty box.
    bool audition(Patch& patch, Box b);
    // Drag: copy (a Variation target writes the patch); gene bank to gene bank moves.
    void copy(Patch& patch, Box from, Box to);
    void move(Patch& patch, Box from, Box to);
    // Context menu Delete; only gene bank, mother and father boxes.
    void clear(Box b);
    // Row buttons of the gene bank (row 0..2): "Copy to Variations" (slots to
    // variations 1..8) and "Clear Row".
    void copyRowToVariations(Patch& patch, int row);
    void clearRow(int row);
    // KeyMutaStoreChild: the focused population box to the first free gene bank slot.
    bool storeFocused();

private:
    std::optional<Individual>* slot(Box b);
    const std::optional<Individual>* slot(Box b) const;
    void setChildren(std::array<Individual, kChildren>&& kids, int parents);

    Random rng_;
    std::vector<Individual> mirror_; // per variation (0..8)
};

} // namespace g2::mutate
