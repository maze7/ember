#pragma once

#include <concepts>

namespace Ember
{
	/**
	 * This order matters.
	 * Assets which depend on other types should show up on the list AFTER.
	 * e.g. since materials depend on shaders, they are listed after shaders.
	 */
	enum class AssetType
	{
		BinaryFile,
		TextFile,
		ConfigFile,
		GameProperties,
		Texture,
		Shader,
		MaterialDefinition, // Depends on Texture and Shader
		Image,
		SpriteSheet, // Depends on MaterialDefinition
		Sprite, // Depends on SpriteSheet
		Animation, // Depends on SpriteSheet
		Font, // Depends on SpriteSheet
		AudioClip,
		AudioObject, // Depends on AudioClip
		AudioEvent, // Depends on AudioObject
	};

	class Asset
	{
	public:
		virtual ~Asset() = default;

		virtual void reload(Asset&& other) {}
	};

	template <class T>
	concept IsAsset = std::derived_from<T, Asset>;
}
