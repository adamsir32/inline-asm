#pragma once

namespace sealpir_2d {

// Runs the shared SealPIR implementation. The legacy entry point keeps query
// expansion and dot products in separate packages; the concentrated entry
// point records each recursive server stage in as few graphs as requested.
int run(int argc, char** argv, bool concentrated_default);

} // namespace sealpir_2d
