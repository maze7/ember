#pragma once

#include "core/common.h"
#include "core/handle.h"
#include "graphics/texture.h"

namespace Ember
{
    struct Texture;
    struct Shader;

    /// @brief A material holds state for a Shader to be used during rendering, including bound Texture Samplers and Uniform Buffer data.
    class Material
    {
    public:
        /// @brief Combination of Texture and Sampler bound to a slot in the Material.
        struct BoundSampler
        {
            Handle<Texture> texture = Handle<Texture>::null;
            TextureSampler sampler;

            // autogenerate comparison operators
            auto operator <=>(const BoundSampler&) const = default;
        };

        class Stage
        {
        public:
            static constexpr u32 MAX_UNIFORM_BUFFERS = 8;
            static constexpr u32 MAX_SAMPLERS = 16;

            /// @brief Texture Samplers bound to this Shader state.
            std::array<BoundSampler, MAX_SAMPLERS> samplers;

            /// Default constructor
            Stage() = default;

            /**
             * @brief Sets the data stored in a uniform buffer.
             * @tparam T type of data being set to the slot.
             * @param data instance of T to be added to slot
             * @param slot Uniform Buffer slot to be used.
             */
            template <class T> requires std::is_trivially_copyable_v<T>
            void set_uniform_buffer(const T& data, int slot = 0) {
                const auto bytes = std::as_bytes(std::span{ &data, 1 });
                set_uniform_buffer(bytes, slot);
            }

            /**
             * @brief Sets the data stored in a uniform buffer.
             * @param data to be added to slot
             * @param slot Uniform Buffer slot to be used.
             */
            void set_uniform_buffer(std::span<const std::byte> data, int slot = 0) {
                if (slot >= 0 && slot < MAX_UNIFORM_BUFFERS) {
                    m_uniform_buffers[slot].assign(data.begin(), data.end());
                }
            }

            /**
             * @brief Gets a view of the data stored in a Uniform Buffer
             * @tparam T type to interpret the buffer data as
             * @param slot Slot of the Uniform Buffer to access.
             * @returns data in the buffer interpreted as T.
             */
            template <class T> requires std::is_trivially_copyable_v<T>
            T get_uniform_buffer(int slot = 0) const {
                auto data = get_uniform_buffer(slot);
                EMBER_ASSERT(data.size_bytes() == sizeof(T));

                T result{};
                std::memcpy(&result, data.data(), sizeof(T));
                return result;
            }

            /**
             * @brief Gets a view of the data stored in a Uniform Buffer
             * @param slot Slot of the Uniform Buffer to access.
             * @returns data in the buffer as
             */
            std::span<const std::byte> get_uniform_buffer(int slot = 0) const {
                if (slot >= 0 && slot < MAX_UNIFORM_BUFFERS) {
                    return m_uniform_buffers[slot];
                }

                return {}; // Return an empty span for an invalid slot.
            }

            auto operator<=>(const Stage&) const = default;

        private:
            std::array<std::vector<std::byte>, MAX_UNIFORM_BUFFERS> m_uniform_buffers;
        };

        auto operator<=>(const Material&) const = default;

        /// @brief Shader used by the Material
        Handle<Shader> shader = Handle<Shader>::null;

        /// @brief Data for the vertex shader stage.
        Stage vertex;

        /// @brief Data for the Fragment shader stage.
        Stage fragment;

        Material() = default;
        explicit Material(Handle<Shader> shader) : shader(shader) {}
    };
}
