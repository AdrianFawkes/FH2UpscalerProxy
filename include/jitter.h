#pragma once
// ============================================================================
// jitter.h — Halton-sequence sub-pixel jitter for DLSS temporal accumulation
// ============================================================================
#include <cstdint>

namespace Jitter {

// Set the render resolution (needed to normalize jitter offsets).
void SetRenderResolution(unsigned int width, unsigned int height);

// Get the jitter offset for the current frame (in pixel units, centered at 0).
// These values should be applied to the projection matrix before rendering.
void GetCurrentOffset(float& outX, float& outY);

// Advance to the next frame's jitter sample.
void AdvanceFrame();

// Reset the sequence (e.g. on scene cut).
void Reset();

// Get the current frame index in the sequence.
uint32_t GetFrameIndex();

// Helper: return the 4x4 jitter matrix to multiply into the projection.
// mat[0..15] is column-major.
void GetJitterMatrix(float outMat[16]);

} // namespace Jitter
