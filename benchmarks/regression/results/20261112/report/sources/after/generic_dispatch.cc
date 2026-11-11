        if (mwpm.predecode_enabled) {
            mwpm.predecoder.prepare(graph, detection_events, mwpm.flooder.negative_weight_detection_events);
            mwpm.predecoder.run();
            if (mwpm.predecoder.complete)
                return;
            mwpm.import_predecode_state();
        } else if (mwpm.flooder.negative_weight_detection_events.empty()) {
