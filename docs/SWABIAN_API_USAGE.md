# Swabian TimeTagger API used by the SDE scripts

Generated 2026-10-21 by `Dantagger/tools/scan_swabian_usage.py` (static AST
scan of every `.py` under `SDE/`, excluding `.venv`, `Dantagger`, macOS `._*`
files). Re-run after scripts change:

```sh
.venv/bin/python Dantagger/tools/scan_swabian_usage.py . --markdown Dantagger/SWABIAN_API_USAGE.md
```

How to read it:

- `TimeTagger.x` is a method of the tagger object from `createTimeTagger()`
  (or of the proxy from `SynchronizedMeasurements.getTagger()`).
- Scripts that use the `datautilities.py` wrappers (`ttCountRate`,
  `ttCountRateHist`, `ttHist1D`, `ttHist`) are expanded into the API those
  wrappers call. `setInput(v, t)` is `setTriggerLevel` + `setDeadtime`.
- Keyword arguments seen at call sites are in brackets; the Python binding
  must accept them by these names.
- Not visible to a static scan, checked by hand: `startFor(t*10**12)` and
  `TimeTagStream(n_max_events=100e6)` pass **floats**; channel numbers can be
  **negative** (falling edge: `-1`, `-2`, `-4`); `Correlation(ch, ch)`
  (auto-correlation, `autoCorrelation.py`); `setDelaySoftware` up to
  **31.55 µs** (`tomography_measure_hist.py`); live `getData()` while a
  measurement runs (`pulsePicker_streamCountrate.py`).
- `drivers/amcc/` is a vendored third-party tree (single "Initial Commit");
  its scripts are listed but their scope is an open question in `GAMEPLAN.md`.
- Eight files do not parse (Python 2 syntax or broken) and are skipped; none of them
  is a Time Tagger script.

## Totals (number of scripts using each name)

| API | Scripts |
|---|---|
| `TimeTagger.setTriggerLevel` | 55 |
| `createTimeTagger` | 55 |
| `TimeTagger.setDeadtime` | 45 |
| `SynchronizedMeasurements` | 44 |
| `SynchronizedMeasurements.getTagger` | 44 |
| `SynchronizedMeasurements.startFor` | 44 |
| `SynchronizedMeasurements.waitUntilFinished` | 43 |
| `Countrate` | 39 |
| `Countrate.getData` | 38 |
| `freeTimeTagger` | 32 |
| `TimeTagger.setInputDelay` | 10 |
| `Correlation` | 9 |
| `Correlation.getData` | 9 |
| `Correlation.getIndex` | 9 |
| `TimeTagger.reset` | 7 |
| `HistogramND` | 6 |
| `HistogramND.getData` | 6 |
| `TimeTagger.setDelaySoftware` | 6 |
| `TimeTagStream` | 5 |
| `TimeTagStream.getData` | 5 |
| `TimeTagStreamBuffer.getTimestamps` | 5 |
| `Correlation.startFor` | 4 |
| `Histogram` | 4 |
| `Histogram.getData` | 4 |
| `Resolution` | 4 |
| `Correlation.waitUntilFinished` | 3 |
| `Counter` | 3 |
| `Counter.getData` | 3 |
| `Histogram.getIndex` | 3 |
| `TimeTagStreamBuffer.getChannels` | 3 |
| `Correlation.clear` | 2 |
| `TimeTagStream.startFor` | 2 |
| `Counter.getDataTotalCounts` | 1 |
| `Countrate.getCountsTotal` | 1 |
| `Countrate.startFor` | 1 |
| `Countrate.waitUntilFinished` | 1 |
| `HistogramLogBins` | 1 |
| `HistogramLogBins.getBinEdges` | 1 |
| `HistogramLogBins.getDataNormalizedG2` | 1 |
| `HistogramLogBins.startFor` | 1 |
| `HistogramLogBins.waitUntilFinished` | 1 |
| `HistogramND.clear` | 1 |
| `HistogramND.startFor` | 1 |
| `HistogramND.waitUntilFinished` | 1 |
| `SynchronizedMeasurements.isRunning` | 1 |
| `SynchronizedMeasurements.stop` | 1 |
| `TimeTagStream.start` | 1 |
| `TimeTagStream.stop` | 1 |
| `TimeTagStream.waitUntilFinished` | 1 |
| `TimeTagStreamBuffer.getOverflows` | 1 |

Imported but never used (must exist so the import succeeds):

| Name | Scripts |
|---|---|
| `CHANNEL_UNUSED` | 3 |
| `ChannelEdge` | 1 |
| `Coincidence` | 3 |
| `Combiner` | 3 |
| `Correlation` | 2 |
| `Counter` | 4 |
| `Countrate` | 3 |
| `Event` | 3 |
| `HIGH` | 3 |
| `LOGGER_WARNING` | 3 |
| `LOW` | 3 |
| `Scope` | 3 |
| `TimeDifferences` | 3 |
| `TimeTagStream` | 2 |
| `UNKNOWN` | 3 |
| `setLogger` | 3 |

| Script | Wrappers (datautilities) | TimeTagger API (keyword args) | Imported only |
|---|---|---|---|
| `1dtomography_measure_hist.py` | – | `Correlation`, `Correlation.getData`, `Correlation.getIndex`, `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime` [channel, deadtime], `TimeTagger.setInputDelay`, `TimeTagger.setTriggerLevel` [channel, voltage], `createTimeTagger`, `freeTimeTagger` | – |
| `1dtomography_repeat_measurement_sweep.py` | – | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `createTimeTagger`, `freeTimeTagger` | – |
| `1dtomography_repeat_measurement_sweep_sandwichattcall.py` | – | `createTimeTagger`, `freeTimeTagger` | – |
| `autoCorrelation.py` | ttHist(measureFor) | `Correlation`, `Correlation.getData`, `Correlation.getIndex`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setInputDelay`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | – |
| `autoCorrelation_oneSided.py` | ttHist1D(measureFor) | `Histogram`, `Histogram.getData`, `Histogram.getIndex`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setInputDelay`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | – |
| `autoCorrelation_trigSweep.py` | ttHist(measureFor) | `Correlation`, `Correlation.getData`, `Correlation.getIndex`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setInputDelay`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `correlation.py` | – | `Correlation`, `Correlation.getData`, `Correlation.getIndex`, `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime` [channel, deadtime], `TimeTagger.setInputDelay`, `TimeTagger.setTriggerLevel` [channel, voltage], `createTimeTagger`, `freeTimeTagger` | – |
| `correlation_extinction_vs_trig.py` | – | `Correlation`, `Correlation.clear`, `Correlation.getData`, `Correlation.getIndex`, `Correlation.startFor`, `Correlation.waitUntilFinished`, `TimeTagger.setDeadtime` [channel, deadtime], `TimeTagger.setInputDelay`, `TimeTagger.setTriggerLevel` [channel, voltage], `createTimeTagger`, `freeTimeTagger` | – |
| `countrate_vs_reprate.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `datautilities.py` | – | `Correlation`, `Correlation.getData`, `Correlation.getIndex`, `Countrate`, `Countrate.getData`, `Histogram`, `Histogram.getData`, `Histogram.getIndex`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime` [channel, deadtime], `TimeTagger.setInputDelay` [channel, delay], `TimeTagger.setTriggerLevel` [channel, voltage], `createTimeTagger` | – |
| `drivers/amcc/measurement_scripts/ktron_hpdcode_pulse_2dplot_emin_ibias_ivsweeps.py` | – | `TimeTagger.reset`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel` | – |
| `drivers/amcc/measurement_scripts/pid_snspd_timetagger.py` | – | `TimeTagStream`, `TimeTagStream.getData`, `TimeTagStream.startFor`, `TimeTagStreamBuffer.getTimestamps`, `TimeTagger.reset`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | – |
| `drivers/amcc/measurement_scripts/swabian_timetagger_g2_loghistogram.py` | – | `HistogramLogBins` [click_channel, exp_start, exp_stop, n_bins, start_channel], `HistogramLogBins.getBinEdges`, `HistogramLogBins.getDataNormalizedG2`, `HistogramLogBins.startFor`, `HistogramLogBins.waitUntilFinished` [timeout], `TimeTagStream` [channels, n_max_events], `TimeTagStream.getData`, `TimeTagStream.startFor`, `TimeTagStream.waitUntilFinished` [timeout], `TimeTagStreamBuffer.getChannels`, `TimeTagStreamBuffer.getTimestamps`, `TimeTagger.reset`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | – |
| `drivers/amcc/measurement_scripts/swabian_timetagger_rawstream.py` | – | `TimeTagStream` [channels, n_max_events], `TimeTagStream.getData`, `TimeTagStreamBuffer.getChannels`, `TimeTagStreamBuffer.getTimestamps`, `TimeTagger.reset`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | `CHANNEL_UNUSED`, `Coincidence`, `Combiner`, `Correlation`, `Counter`, `Countrate`, `Event`, `HIGH`, `LOGGER_WARNING`, `LOW`, `Scope`, `TimeDifferences`, `UNKNOWN`, `setLogger` |
| `drivers/amcc/measurement_scripts/swabian_timetagger_rawstream_1ch.py` | – | `TimeTagStream` [channels, n_max_events], `TimeTagStream.getData`, `TimeTagStreamBuffer.getChannels`, `TimeTagStreamBuffer.getTimestamps`, `TimeTagger.reset`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | `CHANNEL_UNUSED`, `Coincidence`, `Combiner`, `Correlation`, `Counter`, `Countrate`, `Event`, `HIGH`, `LOGGER_WARNING`, `LOW`, `Scope`, `TimeDifferences`, `UNKNOWN`, `setLogger` |
| `drivers/amcc/measurement_scripts/timetagger_counts_vs_ibias.py` | – | `Correlation` [binwidth, channel_1, channel_2, n_bins], `Correlation.getData`, `Correlation.getIndex`, `Correlation.startFor`, `Correlation.waitUntilFinished` [timeout], `TimeTagger.reset`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | `CHANNEL_UNUSED`, `Coincidence`, `Combiner`, `Counter`, `Countrate`, `Event`, `HIGH`, `LOGGER_WARNING`, `LOW`, `Scope`, `TimeDifferences`, `TimeTagStream`, `UNKNOWN`, `setLogger` |
| `drivers/amcc/measurement_scripts/timetagger_swabian_propagation_delay_ktron.py` | – | `Correlation` [binwidth, channel_1, channel_2, n_bins], `Correlation.getData`, `Correlation.getIndex`, `Correlation.startFor` [clear], `TimeTagger.reset`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | – |
| `drivers/amcc/standard_measurements/parameterized.py` | – | `Countrate` [channels], `Countrate.getCountsTotal`, `Countrate.startFor`, `Countrate.waitUntilFinished` [timeout], `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel` | `TimeTagStream` |
| `live_countrate_plot.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `measure2dhistogram.py` | – | `HistogramND` [binwidths, n_bins, start_channel, stop_channels], `HistogramND.clear`, `HistogramND.getData`, `HistogramND.startFor`, `HistogramND.waitUntilFinished`, `Resolution`, `TimeTagger.setDelaySoftware`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | `ChannelEdge` |
| `measureHistogram.py` | – | `Correlation`, `Correlation.clear`, `Correlation.getData`, `Correlation.getIndex`, `Correlation.startFor`, `Correlation.waitUntilFinished`, `TimeTagger.setDeadtime` [channel, deadtime], `TimeTagger.setInputDelay`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `measure_counts_over_time.py` | – | `TimeTagStream`, `TimeTagStream.getData`, `TimeTagStream.start`, `TimeTagStream.stop`, `TimeTagStreamBuffer.getOverflows`, `TimeTagStreamBuffer.getTimestamps`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | – |
| `optimization_backup/estimate_photon_number.py` | – | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `optimization_backup/tomography_measure_hist.py` | – | `Counter` [binwidth, channels, n_values], `Counter.getData`, `HistogramND` [binwidths, n_bins, start_channel, stop_channels], `HistogramND.getData`, `Resolution`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDelaySoftware`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `pcr_vs_photonNumber.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `printcountRate.py` | ttCountRate(measureFor) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | – |
| `pulsePickerReprate.py` | – | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `pulsePicker_streamCountrate.py` | – | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.isRunning`, `SynchronizedMeasurements.startFor` [clear], `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `pulsepickerthresholdsweep.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `scanLambda_countrate.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `sde_optimize_pol.py` | ttCountRate(setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | – |
| `sde_pcr.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | – |
| `sde_pcr_and_attcall.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | – |
| `sde_pcr_and_attcall_pulsed.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `sde_pcr_and_attcall_pulsed_conditional.py` | ttCountRateHist(measureBoth, setInput) | `Countrate`, `Countrate.getData`, `Histogram`, `Histogram.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setInputDelay`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `sde_pcr_and_attcall_sweep_lambda.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `sde_pcr_and_attcall_sweep_power.py` | ttCountRate(measureFor) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | – |
| `sweepHist.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | – |
| `takePcr.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | – |
| `takePcr_noLight.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | – |
| `takePcr_noLight_conditional.py` | ttCountRateHist(measureBoth, setInput) | `Countrate`, `Countrate.getData`, `Histogram`, `Histogram.getData`, `Histogram.getIndex`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setInputDelay`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | – |
| `takePcr_noLight_sweepBias.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `takePcr_rigol.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | – |
| `takePcr_scanPulsedivision.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `takePcr_scanThreshold.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `takePcr_sweepLambda.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `takePcr_switch.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | – |
| `takePcr_trigSweep.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `timetaggersweep.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | – |
| `timetaggersweep_2d.py` | – | `HistogramND` [binwidths, n_bins, start_channel, stop_channels], `HistogramND.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor` [clear], `SynchronizedMeasurements.stop`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDelaySoftware`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `timetaggersweep_rigol.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger` | – |
| `tomography_calibrated_measurement_sweep.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `tomography_estimate_photon_number.py` | – | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `tomography_hist_autotune.py` | – | `Counter` [binwidth, channels, n_values], `Counter.getData`, `Countrate`, `Countrate.getData`, `HistogramND` [binwidths, n_bins, start_channel, stop_channels], `HistogramND.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDelaySoftware`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `tomography_hist_autotune_backup.py` | – | `HistogramND` [binwidths, n_bins, start_channel, stop_channels], `HistogramND.getData`, `Resolution`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDelaySoftware`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | `Counter` |
| `tomography_measure_hist.py` | – | `Counter` [binwidth, channels, n_values], `Counter.getData`, `Counter.getDataTotalCounts`, `Countrate`, `Countrate.getData`, `HistogramND` [binwidths, n_bins, start_channel, stop_channels], `HistogramND.getData`, `Resolution`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDelaySoftware`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
| `tomography_premeasure_optimalbias.py` | ttCountRate(measureFor, setInput) | `Countrate`, `Countrate.getData`, `SynchronizedMeasurements`, `SynchronizedMeasurements.getTagger`, `SynchronizedMeasurements.startFor`, `SynchronizedMeasurements.waitUntilFinished`, `TimeTagger.setDeadtime`, `TimeTagger.setTriggerLevel`, `createTimeTagger`, `freeTimeTagger` | – |
