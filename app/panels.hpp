#pragma once

// The measurement panel types (one file each) and the factory.

#include "panel.hpp"

namespace app {

std::unique_ptr<Panel> make_countrate_panel(int id);
std::unique_ptr<Panel> make_counter_panel(int id);
std::unique_ptr<Panel> make_histogram_panel(int id);
std::unique_ptr<Panel> make_correlation_panel(int id);
std::unique_ptr<Panel> make_histogram2d_panel(int id);
std::unique_ptr<Panel> make_stream_panel(int id);

}  // namespace app
