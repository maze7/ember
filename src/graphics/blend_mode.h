#pragma once

#include "core/hash.h"
#include "graphics/color.h"
#include "graphics/enums/blend_op.h"
#include "graphics/enums/blend_factor.h"
#include "graphics/enums/blend_mask.h"

namespace Ember
{
    #pragma pack(push, 1)
    struct BlendMode
    {
        BlendOp color_op;
        BlendFactor color_src;
        BlendFactor color_dst;
        BlendOp alpha_op;
        BlendFactor alpha_src;
        BlendFactor alpha_dst;
        BlendMask mask;
        Color color;

        /// @brief A simplified constructor for common blend modes.
        constexpr BlendMode(BlendOp op, BlendFactor src, BlendFactor dst)
            : color_op(op)
            , color_src(src)
            , color_dst(dst)
            , alpha_op(op)
            , alpha_src(src)
            , alpha_dst(dst)
            , mask(BlendMask::RGBA)
            , color(Color(255, 255, 255, 255)) {}

        /// @brief A full constructor to specify all blend state properties.
        constexpr BlendMode(
            BlendOp color_op, BlendFactor color_src, BlendFactor color_dst,
            BlendOp alpha_op, BlendFactor alpha_src, BlendFactor alpha_dst,
            BlendMask m, Color c)
            : color_op(color_op)
            , color_src(color_src)
            , color_dst(color_dst)
            , alpha_op(alpha_op)
            , alpha_src(alpha_src)
            , alpha_dst(alpha_dst)
            , mask(m)
            , color(c) {}

        auto operator<=>(const BlendMode&) const = default;

        static const BlendMode Premultiply;
        static const BlendMode NonPremultiplied;
        static const BlendMode Add;
        static const BlendMode Subtract;
        static const BlendMode Multiply;
        static const BlendMode Screen;
    };
    #pragma pack(pop)
}

namespace std
{
    template<>
    struct hash<Ember::BlendMode>
    {
        // The return type must be size_t for std::hash
        size_t operator()(const Ember::BlendMode& b) const noexcept {
            return Ember::combined_hash(
                b.color_op,
                b.color_src,
                b.color_dst,
                b.alpha_op,
                b.alpha_src,
                b.alpha_dst,
                b.mask,
                b.color
            );
        }
    };
}
