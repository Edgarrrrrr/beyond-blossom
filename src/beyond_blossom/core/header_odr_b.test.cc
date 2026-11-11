// Copyright 2026 Beyond Blossom Contributors
// Licensed under the Apache License, Version 2.0.
#include "beyond_blossom/structured/matcher/mwpm.h"
#include "beyond_blossom/core/matching_result.h"
#include "beyond_blossom/core/observable_bits.h"
#include "gtest/gtest.h"

using StructuredEventEntry = void (beyond_blossom::structured::Mwpm::*)(
    const beyond_blossom::structured::MwpmEvent&);
using StructuredRootEntry = const beyond_blossom::structured::AltTreeNode* (
    beyond_blossom::structured::AltTreeNode::*)() const;
StructuredEventEntry structured_event_entry_from_other_translation_unit();
StructuredRootEntry structured_root_entry_from_other_translation_unit();

TEST(HeaderODR, StructuredPrimalUsesOneDefinitionAcrossTranslationUnits) {
    EXPECT_EQ(structured_event_entry_from_other_translation_unit(),
              &beyond_blossom::structured::Mwpm::process_event);
    EXPECT_EQ(structured_root_entry_from_other_translation_unit(),
              &beyond_blossom::structured::AltTreeNode::find_root);
    beyond_blossom::structured::Mwpm decoder;
    (decoder.*structured_event_entry_from_other_translation_unit())({});
    EXPECT_EQ(decoder.active_tree_count(), 0);
}
