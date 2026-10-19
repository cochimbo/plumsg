// ymfm's OPL code with its YM2413 engine instantiated in full: plumsg's Ym2413 mixes
// the channels itself (to turn some down for the Roland sound), which calls engine
// functions ymfm only ever uses inline.
#include "ymfm_opl.cpp"

namespace ymfm {
template class fm_engine_base<opll_registers>;
}  // namespace ymfm
