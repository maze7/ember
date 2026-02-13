#include "blend_mode.h"

using namespace Ember;

const BlendMode BlendMode::Premultiply = { BlendOp::Add, BlendFactor::One, BlendFactor::OneMinusSrcAlpha };
const BlendMode BlendMode::NonPremultiplied = { BlendOp::Add, BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha };
const BlendMode BlendMode::Add = { BlendOp::Add, BlendFactor::One, BlendFactor::DstAlpha };
const BlendMode BlendMode::Subtract = { BlendOp::ReverseSubtract, BlendFactor::One, BlendFactor::One };
const BlendMode BlendMode::Multiply = { BlendOp::Add, BlendFactor::DstColor, BlendFactor::OneMinusSrcAlpha };
const BlendMode BlendMode::Screen = { BlendOp::Add, BlendFactor::One, BlendFactor::OneMinusSrcColor };
