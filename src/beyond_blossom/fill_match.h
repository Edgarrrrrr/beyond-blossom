// Copyright 2022 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "beyond_blossom/driver/mwpm_decoding.h"
#include "beyond_blossom/driver/namespaced_main.h"
#include "beyond_blossom/unstructured/flooder/graph.h"
#include "beyond_blossom/unstructured/flooder/graph_fill_region.h"
#include "beyond_blossom/unstructured/flooder/graph_flooder.h"
#include "beyond_blossom/unstructured/interop/compressed_edge.h"
#include "beyond_blossom/unstructured/interop/mwpm_event.h"
#include "beyond_blossom/unstructured/interop/region_edge.h"
#include "beyond_blossom/core/interop/varying.h"
#include "beyond_blossom/unstructured/matcher/alternating_tree.h"
#include "beyond_blossom/unstructured/matcher/mwpm.h"
#include "beyond_blossom/core/tracker/cyclic.h"
#include "beyond_blossom/unstructured/tracker/flood_check_event.h"
#include "beyond_blossom/unstructured/tracker/queued_event_tracker.h"
#include "beyond_blossom/unstructured/tracker/radix_heap_queue.h"
