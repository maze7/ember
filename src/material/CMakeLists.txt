# Ember::Material is what a material type is once compiled: its bytecode, the record its values
# encode into, its per-object data and its say in its pipelines, plus the file a cook carries them
# in. It speaks the GPU's vocabulary and knows nothing of Slang, so ship builds load cooked types
# through this module alone.
ember_add_module(Material
	SOURCES
		type.cpp
	PUBLIC_DEPS
		Ember::GpuTypes
)
