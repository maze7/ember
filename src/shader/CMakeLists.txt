# Ember::ShaderCompiler is the engine's way from Slang to SPIR-V: plain programs, and material types
# reflected into a material::Type. Tools and tests always link it; the runtime links it only with
# EMBER_SHADER_COMPILER on, for hot reload. slangc is already required to build the engine, so
# requiring its library adds no dependency.
find_package(slang CONFIG REQUIRED)

ember_add_module(ShaderCompiler
	SOURCES
		compiler.cpp
		fence.cpp
		reflection.cpp
	PUBLIC_DEPS
		Ember::Material
	PRIVATE_DEPS
		slang::slang
)

target_compile_definitions(ember_shadercompiler PRIVATE EMBER_SHADER_DIR="${EMBER_SHADER_SOURCE_DIR}")
