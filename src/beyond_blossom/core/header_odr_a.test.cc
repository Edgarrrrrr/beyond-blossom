// Copyright 2026 Beyond Blossom Contributors
// Licensed under the Apache License, Version 2.0.
#include "beyond_blossom/structured/matcher/mwpm.h"
#include "beyond_blossom/core/matching_result.h"
#include "beyond_blossom/core/observable_bits.h"

using StructuredEventEntry = void (beyond_blossom::structured::Mwpm::*)(
    const beyond_blossom::structured::MwpmEvent&);

// The second translation unit takes these addresses too. A duplicate definition
// or a TU-local header implementation must fail linking or pointer equality.
StructuredEventEntry structured_event_entry_from_other_translation_unit() {
    return &beyond_blossom::structured::Mwpm::process_event;
}
using StructuredRootEntry = const beyond_blossom::structured::AltTreeNode* (
    beyond_blossom::structured::AltTreeNode::*)() const;
StructuredRootEntry structured_root_entry_from_other_translation_unit() {
    return &beyond_blossom::structured::AltTreeNode::find_root;
}
