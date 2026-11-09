#pragma once
#include <cstdint>

// Hardware write watchpoint (DR0) on one 4-byte value, on every thread of the game: logs which code writes it.
// watch_arm: start watching (re-applies to threads started since); watch_report: log writers found since the last
// report (all of them, with counts, when final); watch_disarm: stop. Call from one thread.
void watch_arm(void* address, const char* name);
void watch_report(bool final);
void watch_disarm();
