// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#ifndef BB_STRUCTURED_SERIAL_IMPORT_TEST_H
#define BB_STRUCTURED_SERIAL_IMPORT_TEST_H
#include "beyond_blossom/structured/matcher/mwpm.h"

namespace beyond_blossom::structured {
// Test-only access shared by serial, component and double-buffer regressions.
// Retained extraction scratch proves a real packed decode used this path,
// without introducing counters or inspection branches into production builds.
struct SerialImportTestAccess {
    using TimelineEntry = void (*)(Mwpm&, bool, bool, bool);
    static TimelineEntry exchange_process_entry(Mwpm& mwpm, TimelineEntry entry) {
        const auto previous = mwpm.process_timeline_entry_;
        mwpm.process_timeline_entry_ = entry;
        return previous;
    }
    static bool direct_bound(const Mwpm& mwpm) {
        return mwpm.import_entry == &Mwpm::import_predecode_state_impl<true>;
    }
    static void use_owned_packed_import(Mwpm& mwpm) {
        mwpm.packed_import_entry = mwpm.import_entry;
        mwpm.packed_fixed_prepare_entry = &Mwpm::prepare_packed_fixed_import_impl<false>;
        mwpm.packed_captained_import_entries = mwpm.captained_import_entries;
        mwpm.packed_captained_no_scratch_entries = mwpm.captained_no_scratch_entries;
    }
    static bool used_borrowed_extraction(const Mwpm& mwpm) {
        return !mwpm.changed_seed_bits_.empty();
    }
};
}  // namespace beyond_blossom::structured
#endif
