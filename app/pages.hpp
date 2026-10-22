#pragma once

// The setup pages (dockable windows like the measurements): Comparator
// (shared channel settings, LEDs, board status), TDC-GPX2 & LVDS (firmware
// release and frames, the one-click setup, manual GPX2 configuration,
// calibration, link test) and Countrate vs trigger.  Taken from the TDC
// Control app; every value shown as "board" is what the FPGA read back.

#include "panel.hpp"

namespace app {

struct PagesState {
    // TDC-GPX2 (manual configuration)
    double lsb_ps{1.0};
    int ref_bits_idx{7};                // 24 (used when auto_widths is off)
    int stop_bits_idx{3};               // 20
    bool auto_widths{true};             // release 2: shortest frame for the LSB and LCLKIN
    bool ddr{true};                     // release 2 only
    std::array<bool, 4> channels{{true, true, true, true}};
    int hires{0};
    bool test_pattern{false};
    bool synced_from_board{false};      // the inputs start at what the board has
    // LVDS
    bool lclk_manual_on{false};
    int divider{0};
    int bit_phase{0};
    int phase_step{0};
    int link_window_ms{1};
    // LEDs
    std::array<float, 3> led1_rgb{{1.f, 0.f, 0.f}};
    int led1_current{0};
    std::array<bool, 3> led2{{false, false, false}};
    // Countrate vs trigger
    ScanRequest scan;
    bool scan_log_y{true};
};

void page_comparator(AppContext& ctx, PagesState& s);
void page_tdc(AppContext& ctx, PagesState& s);
void page_scan(AppContext& ctx, PagesState& s);

// The board setup card (release, frames, configured/calibrated, Set up), also
// used by the sidebar's prompt.
void board_setup_card(AppContext& ctx);

}  // namespace app
