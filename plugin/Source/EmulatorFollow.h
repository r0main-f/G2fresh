#pragma once
// When the Emulated G2 starts and what it plays: G2fresh's own (the original has no emulator). Plain C++.

namespace g2io {

// At launch: the stand-alone app always starts it (the user's decision, 2026-10-18); a plugin instance as its project
// left it ("emulated" in its state).
constexpr bool emulatorAtLaunch(bool standalone, bool projectWantsIt) { return standalone || projectWantsIt; }

// The Emulated G2 plays the document: once it is up, and again for each document opened or made since (`generation`,
// PatchDocument::generation) while nothing binds it, so that a patch opened after a performance plays. A document
// the user took off the synth (Stop Live Editing) stays off. `sentGeneration`: the document last sent (-1: none).
constexpr bool emulatorSendsDocument(bool emulated, bool ready, bool bound, int generation, int sentGeneration)
{
    return emulated && ready && !bound && generation != sentGeneration;
}

} // namespace g2io
