// vst3-to-clap - layout of the configuration block embedded in converted .clap binaries.
// Shared between the wrapper (reader) and the converter tool (writer).
#pragma once

// The marker must appear exactly once in the wrapper binary.
#define V3C_EMBED_MARKER "@@VST3-TO-CLAP:EMBEDDED-CONFIG:v1@@"
// Total size of the block, marker included. Payload: "<vst3 path>\0<class filter>\0".
#define V3C_EMBED_TOTAL_SIZE 4096
