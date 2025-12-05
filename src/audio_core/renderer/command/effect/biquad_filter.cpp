// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio_core/adsp/apps/audio_renderer/command_list_processor.h"
#include "audio_core/renderer/command/effect/biquad_filter.h"
#include "audio_core/renderer/voice/voice_state.h"
#include "common/bit_cast.h"

namespace AudioCore::Renderer {
/**
 * Biquad filter float implementation (Direct Form 2).
 * This matches Ryujinx's implementation for better numerical stability.
 *
 * @param output       - Output container for filtered samples.
 * @param input        - Input container for samples to be filtered.
 * @param b            - Feedforward coefficients.
 * @param a            - Feedback coefficients.
 * @param state        - State to track previous samples between calls.
 * @param sample_count - Number of samples to process.
 */
void ApplyBiquadFilterFloat(std::span<s32> output, std::span<const s32> input,
                            std::array<s16, 3>& b_, std::array<s16, 2>& a_,
                            VoiceState::BiquadFilterState& state, const u32 sample_count) {
    constexpr f64 min{std::numeric_limits<s32>::min()};
    constexpr f64 max{std::numeric_limits<s32>::max()};
    std::array<f64, 3> b{Common::FixedPoint<50, 14>::from_base(b_[0]).to_double(),
                         Common::FixedPoint<50, 14>::from_base(b_[1]).to_double(),
                         Common::FixedPoint<50, 14>::from_base(b_[2]).to_double()};
    std::array<f64, 2> a{Common::FixedPoint<50, 14>::from_base(a_[0]).to_double(),
                         Common::FixedPoint<50, 14>::from_base(a_[1]).to_double()};

    // Direct Form 2 uses only 2 state variables (s0, s1)
    // s2 and s3 are unused in Direct Form 2
    f64 s0{Common::BitCast<f64>(state.s0)};
    f64 s1{Common::BitCast<f64>(state.s1)};

    for (u32 i = 0; i < sample_count; i++) {
        f64 in_sample{static_cast<f64>(input[i])};
        f64 sample{in_sample * b[0] + s0};

        output[i] = static_cast<s32>(std::clamp(sample, min, max));

        // Update state using Direct Form 2
        s0 = in_sample * b[1] + sample * a[0] + s1;
        s1 = in_sample * b[2] + sample * a[1];
    }

    state.s0 = Common::BitCast<s64>(s0);
    state.s1 = Common::BitCast<s64>(s1);
    // s2 and s3 are unused in Direct Form 2, but we keep them zeroed for consistency
    state.s2 = 0;
    state.s3 = 0;
}

/**
 * Biquad filter float implementation with native float coefficients (SDK REV15+).
 * Uses Direct Form 2 for better numerical stability, matching Ryujinx.
 */
void ApplyBiquadFilterFloat2(std::span<s32> output, std::span<const s32> input,
                             std::array<f32, 3>& b, std::array<f32, 2>& a,
                             VoiceState::BiquadFilterState& state, const u32 sample_count) {
    constexpr f64 min{std::numeric_limits<s32>::min()};
    constexpr f64 max{std::numeric_limits<s32>::max()};

    std::array<f64, 3> b_double{static_cast<f64>(b[0]), static_cast<f64>(b[1]), static_cast<f64>(b[2])};
    std::array<f64, 2> a_double{static_cast<f64>(a[0]), static_cast<f64>(a[1])};

    // Direct Form 2 uses only 2 state variables (s0, s1)
    // s2 and s3 are unused in Direct Form 2
    f64 s0{Common::BitCast<f64>(state.s0)};
    f64 s1{Common::BitCast<f64>(state.s1)};

    for (u32 i = 0; i < sample_count; i++) {
        f64 in_sample{static_cast<f64>(input[i])};
        f64 sample{in_sample * b_double[0] + s0};

        output[i] = static_cast<s32>(std::clamp(sample, min, max));

        // Update state using Direct Form 2
        s0 = in_sample * b_double[1] + sample * a_double[0] + s1;
        s1 = in_sample * b_double[2] + sample * a_double[1];
    }

    state.s0 = Common::BitCast<s64>(s0);
    state.s1 = Common::BitCast<s64>(s1);
    // s2 and s3 are unused in Direct Form 2, but we keep them zeroed for consistency
    state.s2 = 0;
    state.s3 = 0;
}

/**
 * Biquad filter s32 implementation.
 *
 * @param output       - Output container for filtered samples.
 * @param input        - Input container for samples to be filtered.
 * @param b            - Feedforward coefficients.
 * @param a            - Feedback coefficients.
 * @param state        - State to track previous samples between calls.
 * @param sample_count - Number of samples to process.
 */
static void ApplyBiquadFilterInt(std::span<s32> output, std::span<const s32> input,
                                 std::array<s16, 3>& b, std::array<s16, 2>& a,
                                 VoiceState::BiquadFilterState& state, const u32 sample_count) {
    constexpr s64 min{std::numeric_limits<s32>::min()};
    constexpr s64 max{std::numeric_limits<s32>::max()};

    for (u32 i = 0; i < sample_count; i++) {
        const s64 in_sample{input[i]};
        const s64 sample{in_sample * b[0] + state.s0};
        const s64 out_sample{std::clamp<s64>((sample + (1 << 13)) >> 14, min, max)};

        output[i] = static_cast<s32>(out_sample);

        state.s0 = state.s1 + b[1] * in_sample + a[0] * out_sample;
        state.s1 = b[2] * in_sample + a[1] * out_sample;
    }
}

void BiquadFilterCommand::Dump(
    [[maybe_unused]] const AudioRenderer::CommandListProcessor& processor, std::string& string) {
    string += fmt::format(
        "BiquadFilterCommand\n\tinput {:02X} output {:02X} needs_init {} use_float_processing {}\n",
        input, output, needs_init, use_float_processing);
}

void BiquadFilterCommand::Process(const AudioRenderer::CommandListProcessor& processor) {
    if (state == 0) {
        LOG_ERROR(Service_Audio, "BiquadFilterCommand: Invalid state pointer (null)");
        return;
    }

    auto state_{reinterpret_cast<VoiceState::BiquadFilterState*>(state)};
    if (needs_init) {
        *state_ = {};
    }

    // Validate buffer indices and bounds
    if (input < 0 || processor.sample_count == 0) {
        LOG_ERROR(Service_Audio,
                  "BiquadFilterCommand: Invalid input buffer index or sample count - input={}, "
                  "sample_count={}",
                  input, processor.sample_count);
        return;
    }

    // If output is invalid but input is valid, use input as output (in-place processing)
    s16 effective_output = output;
    if (output < 0) {
        LOG_WARNING(Service_Audio,
                    "BiquadFilterCommand: Invalid output buffer index ({}), using input ({}) for "
                    "in-place processing",
                    output, input);
        effective_output = input;
    }

    const u64 input_offset = static_cast<u64>(input) * processor.sample_count;
    const u64 output_offset = static_cast<u64>(effective_output) * processor.sample_count;

    if (input_offset + processor.sample_count > processor.mix_buffers.size() ||
        output_offset + processor.sample_count > processor.mix_buffers.size()) {
        LOG_ERROR(Service_Audio,
                  "BiquadFilterCommand: Buffer indices out of bounds - input_offset={}, "
                  "output_offset={}, sample_count={}, buffer_size={}",
                  input_offset, output_offset, processor.sample_count,
                  processor.mix_buffers.size());
        return;
    }

    auto input_buffer{
        processor.mix_buffers.subspan(input_offset, processor.sample_count)};
    auto output_buffer{
        processor.mix_buffers.subspan(output_offset, processor.sample_count)};

    if (use_float_processing) {
        // REV15+: Use native float coefficients if available
        if (use_float_coefficients) {
            ApplyBiquadFilterFloat2(output_buffer, input_buffer, biquad_float.numerator,
                                   biquad_float.denominator, *state_, processor.sample_count);
        } else {
            ApplyBiquadFilterFloat(output_buffer, input_buffer, biquad.b, biquad.a, *state_,
                                   processor.sample_count);
        }
    } else {
        ApplyBiquadFilterInt(output_buffer, input_buffer, biquad.b, biquad.a, *state_,
                             processor.sample_count);
    }
}

bool BiquadFilterCommand::Verify(const AudioRenderer::CommandListProcessor& processor) {
    // Validate state pointer
    if (state == 0) {
        LOG_ERROR(Service_Audio, "BiquadFilterCommand: Invalid state pointer (null)");
        return false;
    }

    // Validate input buffer index (required)
    if (input < 0) {
        LOG_ERROR(Service_Audio, "BiquadFilterCommand: Invalid input buffer index - input={}", input);
        return false;
    }

    // Output can be invalid - we'll handle it by using input as output (in-place processing)
    // So we don't fail verification if only output is invalid
    s16 effective_output = output;
    if (output < 0) {
        effective_output = input;
    }

    if (processor.sample_count == 0) {
        LOG_ERROR(Service_Audio, "BiquadFilterCommand: Invalid sample count - sample_count={}",
                  processor.sample_count);
        return false;
    }

    const u64 input_offset = static_cast<u64>(input) * processor.sample_count;
    const u64 output_offset = static_cast<u64>(effective_output) * processor.sample_count;

    if (input_offset + processor.sample_count > processor.mix_buffers.size() ||
        output_offset + processor.sample_count > processor.mix_buffers.size()) {
        LOG_ERROR(Service_Audio,
                  "BiquadFilterCommand: Buffer indices out of bounds - input_offset={}, "
                  "output_offset={}, sample_count={}, buffer_size={}",
                  input_offset, output_offset, processor.sample_count,
                  processor.mix_buffers.size());
        return false;
    }

    return true;
}

} // namespace AudioCore::Renderer
