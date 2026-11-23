// SPDX-License-Identifier: GPL-3.0-or-later
// em34xx.app: the macOS GUI executable (end-user build: no monitor, no GDB
// stub). See docs/frontend-cocoa.md.
#import "frontend/cocoa/cocoa_frontend.h"

int main(int argc, char** argv) { return em34xx::cocoa_main(argc, argv); }
