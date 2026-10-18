// ymfm's OPN code with its YM2612 templates instantiated in full: plumsg's Ym2612 mixes
// the FM channels itself (to turn some down for the Roland sound), which calls engine
// functions ymfm only ever uses inline.
#include "ymfm_opn.cpp"

namespace ymfm {
template class opn_registers_base<true>;
template class fm_engine_base<opn_registers_base<true>>;
}  // namespace ymfm
