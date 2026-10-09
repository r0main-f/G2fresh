// The module processors. Each module file defines a register function; list
// it here. Processors are written from re/notes/dsp-module-catalog.md and
// black-box measurements of the emulated G2 (see engine.hpp: clean-room rule,
// and re/notes/native-engine.md).
#include "g2/engine/engine.hpp"

namespace g2::engine {

void registerKeyboard();
void registerOut2();
void registerOut4();
void registerOscA();
void registerFltLP();
void registerEnvADSR();
void registerMix4_1A();
void registerMix2_1A();

void registerBuiltinProcessors()
{
    registerKeyboard();
    registerOut2();
    registerOut4();
    registerOscA();
    registerFltLP();
    registerEnvADSR();
    registerMix4_1A();
    registerMix2_1A();
}

} // namespace g2::engine
