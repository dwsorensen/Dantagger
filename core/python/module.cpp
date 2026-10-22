// pybind11 bindings of the Dantagger core (module Dantagger._dantagger).
// The Swabian-compatible API is the pure-Python layer in Dantagger/__init__.py;
// this module is deliberately low level.  Every blocking call releases the GIL.

#include "engine.hpp"
#include "measurements.hpp"
#include "sim_backend.hpp"
#include "tagger.hpp"
#include "tdc_backend.hpp"

#include "d3xx_common.hpp"
#include "tdcctl/fake_fpga.hpp"

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <memory>

namespace py = pybind11;
using namespace dt;

namespace {

template <class T>
py::array_t<T> to_array(const std::vector<T>& v) {
    py::array_t<T> a(static_cast<py::ssize_t>(v.size()));
    if (!v.empty()) std::memcpy(a.mutable_data(), v.data(), v.size() * sizeof(T));
    return a;
}

template <class T>
py::array_t<T> to_array_2d(const std::vector<T>& v, py::ssize_t rows, py::ssize_t cols) {
    py::array_t<T> a({rows, cols});
    if (!v.empty()) std::memcpy(a.mutable_data(), v.data(), v.size() * sizeof(T));
    return a;
}

// A measurement as Python holds it: keeps the tagger alive and unregisters
// the measurement when the Python object goes away.
struct Handle {
    std::shared_ptr<Tagger> tagger;
    std::shared_ptr<Measurement> m;
    Handle(std::shared_ptr<Tagger> t, std::shared_ptr<Measurement> meas, bool autostart)
        : tagger(std::move(t)), m(std::move(meas)) {
        if (tagger->closed()) throw std::runtime_error("the time tagger has been freed");
        tagger->engine().add(m, autostart);
    }
    ~Handle() {
        if (tagger && m && !tagger->closed()) tagger->engine().remove(m.get());
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    template <class T>
    T& as() const {
        return static_cast<T&>(*m);
    }
    std::vector<Measurement*> self() const { return {m.get()}; }
};

std::vector<Measurement*> raw_list(const std::vector<Handle*>& hs) {
    std::vector<Measurement*> out;
    for (auto* h : hs) out.push_back(h->m.get());
    return out;
}

Engine& engine_of(const std::vector<Handle*>& hs) {
    if (hs.empty()) throw std::invalid_argument("no measurements");
    for (auto* h : hs)
        if (h->tagger.get() != hs.front()->tagger.get())
            throw std::invalid_argument("synchronized measurements must use the same time tagger");
    return hs.front()->tagger->engine();
}

SimSignal parse_signal(const py::dict& d) {
    SimSignal s;
    const std::string kind = d.contains("kind") ? py::str(d["kind"]).cast<std::string>() : "none";
    if (kind == "none") s.kind = SimSignal::Kind::none;
    else if (kind == "poisson") s.kind = SimSignal::Kind::poisson;
    else if (kind == "periodic" || kind == "laser") s.kind = SimSignal::Kind::periodic;
    else if (kind == "detector") s.kind = SimSignal::Kind::detector;
    else throw std::invalid_argument("unknown simulated signal kind '" + kind + "'");
    auto num = [&](const char* key, double& out) {
        if (d.contains(key)) out = d[key].cast<double>();
    };
    num("rate", s.rate_hz);
    num("jitter", s.jitter_ps);
    num("phase", s.phase_ps);
    num("efficiency", s.efficiency);
    num("delay", s.delay_ps);
    num("dark_rate", s.dark_hz);
    num("amplitude", s.amplitude_v);
    if (d.contains("ref")) s.ref = d["ref"].cast<int>();
    return s;
}

}  // namespace

PYBIND11_MODULE(_dantagger, mod) {
    mod.doc() = "Dantagger C++ core";

    py::register_exception<SettingsBusyError>(mod, "SettingsBusyError", PyExc_RuntimeError);
    py::register_exception<DeviceError>(mod, "DeviceError", PyExc_RuntimeError);

    mod.def("list_devices", [] {
        std::vector<std::string> out;
        for (const auto& d : au2ft601::d3xx::enumerate_devices()) out.push_back(d.serial);
        return out;
    });

    py::class_<Tagger, std::shared_ptr<Tagger>>(mod, "Tagger")
        .def("set_trigger_level", &Tagger::set_trigger_level, py::call_guard<py::gil_scoped_release>())
        .def("trigger_level", &Tagger::trigger_level)
        .def("set_hysteresis", &Tagger::set_hysteresis, py::call_guard<py::gil_scoped_release>())
        .def("hysteresis", &Tagger::hysteresis)
        .def("set_deadtime", &Tagger::set_deadtime, py::call_guard<py::gil_scoped_release>())
        .def("deadtime", &Tagger::deadtime)
        .def("set_delay", &Tagger::set_delay, py::call_guard<py::gil_scoped_release>())
        .def("delay", &Tagger::delay)
        .def("set_resolution", &Tagger::set_resolution, py::call_guard<py::gil_scoped_release>())
        .def("resolution", &Tagger::resolution)
        .def("reset", &Tagger::reset, py::call_guard<py::gil_scoped_release>())
        .def("close", &Tagger::close, py::call_guard<py::gil_scoped_release>())
        .def("closed", &Tagger::closed)
        .def("streaming", &Tagger::streaming)
        .def("serial", [](Tagger& t) { return t.backend().serial(); })
        .def("model", [](Tagger& t) { return t.backend().model(); })
        .def("info", [](Tagger& t) { return t.backend().info(); })
        .def("setup_action",
             [](Tagger& t) {
                 auto* b = dynamic_cast<TdcBackend*>(&t.backend());
                 return b ? b->setup_action() : std::string("none");
             })
        .def("setup_message",
             [](Tagger& t) {
                 auto* b = dynamic_cast<TdcBackend*>(&t.backend());
                 return b ? b->setup_message() : std::string();
             })
        .def("take_warnings", [](Tagger& t) { return t.log().take(); })
        .def("dropped_total", [](Tagger& t) { return t.log().dropped(); })
        .def("overflow_intervals", [](Tagger& t) { return t.log().overflow_intervals(); })
        .def("warning_count", [](Tagger& t, const std::string& kind) { return t.log().count(kind); })
        .def("tags_processed", [](Tagger& t) { return t.engine().tags_processed(); })
        .def("deadtime_dropped", [](Tagger& t) { return t.engine().deadtime_dropped(); })
        .def("position", [](Tagger& t) { return t.engine().position(); })
        .def("error", [](Tagger& t) { return t.engine().error(); })
        .def("explicit_running", [](Tagger& t) { return t.engine().explicit_running(); });

    mod.def(
        "open_tdc",
        [](const std::string& serial, bool simulate_fpga, bool recalibrate, int ref_bits, int stop_bits,
           const std::string& state_dir) {
            TdcOptions o;
            o.serial = serial;
            o.simulate_fpga = simulate_fpga;
            o.recalibrate = recalibrate;
            o.ref_index_bits = ref_bits;
            o.stop_bits = stop_bits;
            o.state_dir = state_dir;
            py::gil_scoped_release release;
            return std::make_shared<Tagger>(std::make_unique<TdcBackend>(o));
        },
        py::arg("serial") = "", py::arg("simulate_fpga") = false, py::arg("recalibrate") = false,
        py::arg("ref_bits") = 0, py::arg("stop_bits") = 0, py::arg("state_dir") = "");
    mod.def(
        "open_sim",
        [](const py::list& signals, double speed, std::uint64_t seed) {
            std::array<SimSignal, kNumChannels> s{};
            if (signals.size() != kNumChannels) throw std::invalid_argument("four channel signal descriptions expected");
            for (int c = 0; c < kNumChannels; ++c) s[static_cast<std::size_t>(c)] = parse_signal(signals[static_cast<std::size_t>(c)].cast<py::dict>());
            auto backend = std::make_unique<SimBackend>(s, speed, seed);
            py::gil_scoped_release release;
            return std::make_shared<Tagger>(std::move(backend));
        },
        py::arg("signals"), py::arg("speed") = 1.0, py::arg("seed") = 1);
    mod.def(
        "open_replay",
        [](py::array_t<std::int64_t, py::array::c_style | py::array::forcecast> times,
           py::array_t<std::int8_t, py::array::c_style | py::array::forcecast> channels, double speed) {
            std::vector<timestamp_t> t(times.data(), times.data() + times.size());
            std::vector<std::int8_t> c(channels.data(), channels.data() + channels.size());
            auto backend = std::make_unique<ReplayBackend>(std::move(t), std::move(c), speed);
            py::gil_scoped_release release;
            return std::make_shared<Tagger>(std::move(backend));
        },
        py::arg("times"), py::arg("channels"), py::arg("speed") = -1.0);

    // Simulated-FPGA controls (tests).
    mod.def("sim_fpga_unplug", [] { TdcBackend::simulate_unplug(); });
    mod.def(
        "sim_fpga_set",
        [](double event_rate_hz, double gpx_ppm, std::uint64_t latency_ps, std::uint64_t latency_jitter_ps,
           std::uint64_t drop_every) {
            auto& f = TdcBackend::simulated_fpga();
            f.event_rate_hz = event_rate_hz;
            f.gpx_ppm = gpx_ppm;
            f.latency_ps = latency_ps;
            f.latency_jitter_ps = latency_jitter_ps;
            f.drop_every = drop_every;
        },
        py::arg("event_rate_hz") = 20000.0, py::arg("gpx_ppm") = 0.0, py::arg("latency_ps") = 600000,
        py::arg("latency_jitter_ps") = 0, py::arg("drop_every") = 0);
    mod.def("sim_fpga_state", [] {
        auto& f = TdcBackend::simulated_fpga();
        py::dict d;
        d["phase"] = f.phase;
        d["gpx_verified"] = f.gpx_verified;
        d["lclk_en"] = f.lclk_en;
        d["commands"] = f.commands;
        d["dac"] = std::vector<int>(f.dac.begin(), f.dac.end());
        d["wiper"] = std::vector<int>(f.wiper.begin(), f.wiper.end());
        d["dropped_total"] = f.dropped_total();
        return d;
    });

    // ---- measurements ----------------------------------------------------------
    py::class_<Handle>(mod, "Measurement")
        .def("start", [](Handle& h) { h.tagger->engine().start(h.self(), -1, false); })
        .def("start_for", [](Handle& h, timestamp_t d, bool clear) { h.tagger->engine().start(h.self(), d, clear); })
        .def("stop", [](Handle& h) { h.tagger->engine().stop(h.self()); }, py::call_guard<py::gil_scoped_release>())
        .def("clear", [](Handle& h) { h.tagger->engine().clear(h.self()); }, py::call_guard<py::gil_scoped_release>())
        .def("is_running", [](Handle& h) { return h.tagger->engine().any_running(h.self()); })
        .def("wait", [](Handle& h, int timeout_ms) { return h.tagger->engine().wait(h.self(), timeout_ms); },
             py::call_guard<py::gil_scoped_release>())
        .def("capture_duration", [](Handle& h) {
            auto g = h.m->guard();
            return h.m->capture_duration;
        })
        .def("name", [](Handle& h) { return std::string(h.m->name()); })
        .def("detach", [](Handle& h) {
            if (h.tagger && !h.tagger->closed()) h.tagger->engine().remove(h.m.get());
        });

    mod.def("group_start", [](const std::vector<Handle*>& hs, timestamp_t duration, bool clear) {
        engine_of(hs).start(raw_list(hs), duration, clear);
    });
    mod.def("group_stop", [](const std::vector<Handle*>& hs) {
        auto& e = engine_of(hs);
        auto l = raw_list(hs);
        py::gil_scoped_release release;
        e.stop(l);
    });
    mod.def("group_clear", [](const std::vector<Handle*>& hs) {
        auto& e = engine_of(hs);
        auto l = raw_list(hs);
        py::gil_scoped_release release;
        e.clear(l);
    });
    mod.def("group_wait", [](const std::vector<Handle*>& hs, int timeout_ms) {
        auto& e = engine_of(hs);
        auto l = raw_list(hs);
        py::gil_scoped_release release;
        return e.wait(l, timeout_ms);
    });
    mod.def("group_running", [](const std::vector<Handle*>& hs) { return engine_of(hs).any_running(raw_list(hs)); });

    struct CountrateH : Handle {
        using Handle::Handle;
    };
    py::class_<CountrateH, Handle>(mod, "Countrate")
        .def(py::init([](std::shared_ptr<Tagger> t, std::vector<int> channels, bool autostart) {
            return new CountrateH(std::move(t), std::make_shared<Countrate>(std::move(channels)), autostart);
        }))
        .def("data", [](CountrateH& h) {
            auto g = h.m->guard();
            return to_array(h.as<Countrate>().rates());
        })
        .def("counts_total", [](CountrateH& h) {
            auto g = h.m->guard();
            return to_array(h.as<Countrate>().counts_total());
        });

    struct CounterH : Handle {
        using Handle::Handle;
    };
    py::class_<CounterH, Handle>(mod, "Counter")
        .def(py::init([](std::shared_ptr<Tagger> t, std::vector<int> channels, timestamp_t binwidth, int n_values,
                         bool autostart) {
            return new CounterH(std::move(t), std::make_shared<Counter>(std::move(channels), binwidth, n_values), autostart);
        }))
        .def("data", [](CounterH& h, bool rolling) {
            auto g = h.m->guard();
            auto& c = h.as<Counter>();
            return to_array_2d(c.data(rolling), c.n_channels(), c.n_values());
        })
        .def("data_normalized", [](CounterH& h, bool rolling) {
            auto g = h.m->guard();
            auto& c = h.as<Counter>();
            return to_array_2d(c.data_normalized(rolling), c.n_channels(), c.n_values());
        })
        .def("index", [](CounterH& h) {
            auto g = h.m->guard();
            return to_array(h.as<Counter>().index());
        })
        .def("total_counts", [](CounterH& h) {
            auto g = h.m->guard();
            return to_array(h.as<Counter>().total_counts());
        });

    struct HistogramH : Handle {
        using Handle::Handle;
    };
    py::class_<HistogramH, Handle>(mod, "Histogram")
        .def(py::init([](std::shared_ptr<Tagger> t, int click, int start, timestamp_t binwidth, int n_bins, bool autostart) {
            return new HistogramH(std::move(t), std::make_shared<Histogram>(click, start, binwidth, n_bins), autostart);
        }))
        .def("data", [](HistogramH& h) {
            auto g = h.m->guard();
            return to_array(h.as<Histogram>().data());
        })
        .def("index", [](HistogramH& h) {
            auto g = h.m->guard();
            return to_array(h.as<Histogram>().index());
        });

    struct CorrelationH : Handle {
        using Handle::Handle;
    };
    py::class_<CorrelationH, Handle>(mod, "Correlation")
        .def(py::init([](std::shared_ptr<Tagger> t, int c1, int c2, timestamp_t binwidth, int n_bins, bool autostart) {
            return new CorrelationH(std::move(t), std::make_shared<Correlation>(c1, c2, binwidth, n_bins), autostart);
        }))
        .def("data", [](CorrelationH& h) {
            auto g = h.m->guard();
            return to_array(h.as<Correlation>().data());
        })
        .def("data_normalized", [](CorrelationH& h) {
            auto g = h.m->guard();
            return to_array(h.as<Correlation>().data_normalized());
        })
        .def("index", [](CorrelationH& h) {
            auto g = h.m->guard();
            return to_array(h.as<Correlation>().index());
        });

    struct HistogramNDH : Handle {
        using Handle::Handle;
    };
    py::class_<HistogramNDH, Handle>(mod, "HistogramND")
        .def(py::init([](std::shared_ptr<Tagger> t, int start, std::vector<int> stops, std::vector<timestamp_t> bws,
                         std::vector<int> n_bins, bool autostart) {
            return new HistogramNDH(std::move(t),
                                    std::make_shared<HistogramND>(start, std::move(stops), std::move(bws), std::move(n_bins)),
                                    autostart);
        }))
        .def("data", [](HistogramNDH& h) {
            auto g = h.m->guard();
            return to_array(h.as<HistogramND>().data());
        })
        .def("index", [](HistogramNDH& h, int dim) {
            auto g = h.m->guard();
            return to_array(h.as<HistogramND>().index(dim));
        });

    py::class_<StreamBuffer>(mod, "StreamBuffer")
        .def("timestamps", [](const StreamBuffer& b) { return to_array(b.timestamps); })
        .def("channels", [](const StreamBuffer& b) { return to_array(b.channels); })
        .def("types", [](const StreamBuffer& b) { return to_array(b.types); })
        .def("missed", [](const StreamBuffer& b) { return to_array(b.missed); })
        .def_readonly("t_start", &StreamBuffer::t_start)
        .def_readonly("t_get_data", &StreamBuffer::t_get_data)
        .def_readonly("has_overflows", &StreamBuffer::has_overflows)
        .def_readonly("lost", &StreamBuffer::lost)
        .def("size", [](const StreamBuffer& b) { return b.timestamps.size(); });

    struct StreamH : Handle {
        using Handle::Handle;
    };
    py::class_<StreamH, Handle>(mod, "TimeTagStream")
        .def(py::init([](std::shared_ptr<Tagger> t, std::size_t n_max, std::vector<int> channels, bool autostart) {
            return new StreamH(std::move(t), std::make_shared<TimeTagStream>(n_max, std::move(channels)), autostart);
        }))
        .def("take", [](StreamH& h) {
            auto g = h.m->guard();
            return h.as<TimeTagStream>().take();
        })
        .def("counts", [](StreamH& h) {
            auto g = h.m->guard();
            return h.as<TimeTagStream>().counts();
        });
}
