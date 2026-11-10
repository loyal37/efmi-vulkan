#pragma once
// What the layer does not do as the original does (D-137): said through the original's warning channel
// (its overlay text; the generation's diagnostics) and handed to the sink of the hosting runtime, which
// keeps it for the host to show. One call per occurrence the caller wants shown: callers say a thing once.
void LayerGap(const wchar_t* format, ...);
// The hosting runtime's sink (frontend/xxmi/runtime.cpp); null in a host that only keeps diagnostics.
extern void (*g_layer_gap_sink)(const wchar_t* text);
