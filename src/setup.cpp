// fast_spawn: the builds its fixed addresses and struct offsets were checked against.
//
// 0x6ae218ec (Steam update 2026-10-28), checked with tools/port_*.py: the tile manager now wants tiles within 3 of a
// player (was 2) and the tile step skips some tiles, both internal; arguments, tile / grid / player / spawning-state
// fields are unchanged. The world's tile grid ring moved +8 (fast_spawn.cpp tile_ring()).
#include "game.h"
#include "port_table.h"
#include "sdk.h"

namespace {
bool setup()
{
    mod_ported_to(kPortToBuild);
    return true;
}
ModSetup reg{&setup};
} // namespace
