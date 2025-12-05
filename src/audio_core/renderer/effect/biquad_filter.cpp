// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "audio_core/renderer/effect/biquad_filter.h"

namespace AudioCore::Renderer {

void BiquadFilterInfo::Update(BehaviorInfo::ErrorInfo& error_info,
                              const InParameterVersion1& in_params, const PoolMapper& pool_mapper) {
    auto in_specific{reinterpret_cast<const ParameterVersion1*>(in_params.specific.data())};
    auto params{reinterpret_cast<ParameterVersion1*>(parameter.data())};

    std::memcpy(params, in_specific, sizeof(ParameterVersion1));

    // Check for corrupted parameters - if detected, disable the effect to prevent audio issues
    bool parameters_valid = true;

    // Validate channel_count
    if (params->channel_count < 0 || static_cast<u32>(params->channel_count) > MaxChannels) {
        LOG_WARNING(Service_Audio, "BiquadFilterInfo: Invalid channel_count {}, disabling effect",
                    params->channel_count);
        parameters_valid = false;
    }

    // Validate parameter state
    if (static_cast<u8>(params->state) > static_cast<u8>(EffectInfoBase::ParameterState::Updated)) {
        LOG_WARNING(Service_Audio, "BiquadFilterInfo: Invalid parameter state {}, disabling effect",
                    static_cast<u8>(params->state));
        parameters_valid = false;
    }

    // Validate input/output buffer indices
    for (s8 i = 0; static_cast<u32>(i) < MaxChannels && parameters_valid; i++) {
        // Negative values are allowed (indicate unused channels), but check for out-of-range values
        if (params->inputs[i] < -1 || params->inputs[i] >= static_cast<s8>(MaxChannels * 2)) {
            LOG_WARNING(Service_Audio, "BiquadFilterInfo: Invalid input buffer index {} for channel {}, disabling effect",
                        params->inputs[i], i);
            parameters_valid = false;
            break;
        }
        if (params->outputs[i] < -1 || params->outputs[i] >= static_cast<s8>(MaxChannels * 2)) {
            LOG_WARNING(Service_Audio, "BiquadFilterInfo: Invalid output buffer index {} for channel {}, disabling effect",
                        params->outputs[i], i);
            parameters_valid = false;
            break;
        }
    }

    mix_id = in_params.mix_id;
    process_order = in_params.process_order;
    // Disable effect if parameters are corrupted to prevent audio issues
    enabled = in_params.enabled && parameters_valid;

    error_info.error_code = ResultSuccess;
    error_info.address = CpuAddr(0);
}

void BiquadFilterInfo::Update(BehaviorInfo::ErrorInfo& error_info,
                              const InParameterVersion2& in_params, const PoolMapper& pool_mapper) {
    auto in_specific{reinterpret_cast<const ParameterVersion2*>(in_params.specific.data())};
    auto params{reinterpret_cast<ParameterVersion2*>(parameter.data())};

    std::memcpy(params, in_specific, sizeof(ParameterVersion2));

    // Check for corrupted parameters - if detected, disable the effect to prevent audio issues
    bool parameters_valid = true;

    // Validate channel_count
    if (params->channel_count < 0 || static_cast<u32>(params->channel_count) > MaxChannels) {
        LOG_WARNING(Service_Audio, "BiquadFilterInfo: Invalid channel_count {}, disabling effect",
                    params->channel_count);
        parameters_valid = false;
    }

    // Validate input/output buffer indices only for active channels
    const s8 active_channels = parameters_valid ? params->channel_count : 0;
    for (s8 i = 0; static_cast<u32>(i) < MaxChannels && i < active_channels && parameters_valid; i++) {
        // Negative values are allowed (indicate unused channels), but check for out-of-range values
        if (params->inputs[i] < -1 || params->inputs[i] >= static_cast<s8>(MaxChannels * 2)) {
            LOG_WARNING(Service_Audio, "BiquadFilterInfo: Invalid input buffer index {} for channel {}, disabling effect",
                        params->inputs[i], i);
            parameters_valid = false;
            break;
        }
        if (params->outputs[i] < -1 || params->outputs[i] >= static_cast<s8>(MaxChannels * 2)) {
            LOG_WARNING(Service_Audio, "BiquadFilterInfo: Invalid output buffer index {} for channel {}, disabling effect",
                        params->outputs[i], i);
            parameters_valid = false;
            break;
        }
    }

    // Validate parameter state
    if (static_cast<u8>(params->state) > static_cast<u8>(EffectInfoBase::ParameterState::Updated)) {
        LOG_WARNING(Service_Audio, "BiquadFilterInfo: Invalid parameter state {}, disabling effect",
                    static_cast<u8>(params->state));
        parameters_valid = false;
    }

    mix_id = in_params.mix_id;
    process_order = in_params.process_order;
    // ParameterVersion2 uses state field similar to v1
    // Effect is enabled if in_params.enabled, parameters are valid, and state is within valid range
    enabled = in_params.enabled && parameters_valid &&
              static_cast<u8>(params->state) <= static_cast<u8>(EffectInfoBase::ParameterState::Updated);

    error_info.error_code = ResultSuccess;
    error_info.address = CpuAddr(0);
}

void BiquadFilterInfo::UpdateForCommandGeneration() {
    if (enabled) {
        usage_state = UsageState::Enabled;
    } else {
        usage_state = UsageState::Disabled;
    }

    // Both v1 and v2 have the same structure with state field, so we can update using either cast
    auto params{reinterpret_cast<ParameterVersion1*>(parameter.data())};
    if (params->state == ParameterState::Initialized ||
        params->state == ParameterState::Updating) {
        params->state = ParameterState::Updated;
    }
}

void BiquadFilterInfo::InitializeResultState(EffectResultState& result_state) {}

void BiquadFilterInfo::UpdateResultState(EffectResultState& cpu_state,
                                         EffectResultState& dsp_state) {}

} // namespace AudioCore::Renderer
