#pragma once

#include "core/common.h"
#include <span>

namespace Ember
{
    struct Shader {};
    struct ShaderDef
    {
        struct ShaderStageDef
        {
            /// @brief Shader Code for the given program
            std::span<u8> code;

            /// @brief The number of samplers
            u32 num_samplers = 0;

            /// @brief The number of uniform buffers
            u32 num_uniform_buffers = 0;

            /// @brief The entry function of the shader program
            const char* entrypoint = "main";
        };

        /// @brief Debug name of the Shader
        const char* name = nullptr;

        /// @brief Vertex shader stage definiton
        ShaderStageDef vertex;

        /// @brief Fragment shader stage definiton
        ShaderStageDef fragment;
    };
}
