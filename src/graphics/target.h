#pragma once

#include "core/handle.h"
#include "graphics/texture.h"

namespace Ember
{
    // Forward Declaration of RenderDevice
    class RenderDevice;

    class Target
    {
    public:
	    /**
	     * @brief Constructs a Target of the requested size. Allocates Texture attachments for requested TextureFormats.
	     * @param size Size of the Target
	     * @param attachments List of TextureFormats for target attachments.
	     */
	    Target(glm::uvec2 size, std::initializer_list<TextureFormat> attachments = { TextureFormat::Color });

        /**
         * @brief Constructs a Target of the requested size. Allocates Texture attachments for requested TextureFormats.
         * @param gpu Graphics device (render hardware interface)
         * @param size Size of the Target
         * @param attachments List of TextureFormats for target attachments.
         */
        Target(RenderDevice* gpu, glm::uvec2 size, std::initializer_list<TextureFormat> attachments = { TextureFormat::Color });

        /// @brief Target destructor
        ~Target();

        /// @returns Dimensions of the Target as a glm::uvec2
        [[nodiscard]] auto size() const {
            return m_size;
        }

        /// @returns const reference to the underlying Texture attachments
        [[nodiscard]] const auto& attachments() const {
            return m_attachments;
        }

    private:
        /// @brief RenderDevice that was used to create this Target
        RenderDevice* m_gpu = nullptr;

        /// @brief Dimensions of the Target and all underlying Texture attachments
        glm::uvec2 m_size;

        /// @brief Underlying Texture attachments for Target
        std::vector<Handle<Texture>> m_attachments;
    };
}
