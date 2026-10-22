#include "panels.hpp"

namespace app {

std::unique_ptr<Panel> make_panel(Kind kind, int id) {
    switch (kind) {
    case Kind::countrate: return make_countrate_panel(id);
    case Kind::counter: return make_counter_panel(id);
    case Kind::histogram: return make_histogram_panel(id);
    case Kind::correlation: return make_correlation_panel(id);
    case Kind::histogram2d: return make_histogram2d_panel(id);
    case Kind::stream: return make_stream_panel(id);
    }
    return nullptr;
}

}  // namespace app
