#include <ember/render/scene.h>

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

namespace
{
	using ember::u32;
	using ember::u64;
	using namespace ember::render;

	static_assert(sizeof(RenderObjectHandle) == 8);
	static_assert(sizeof(GeometryHandle) == 4);
	static_assert(sizeof(MaterialHandle) == 4);

	[[nodiscard]] std::vector<u32> dirty_vector(const RenderScene& scene)
	{
		const auto slots = scene.dirty_slots();
		return {slots.begin(), slots.end()};
	}

	TEST(RenderScene, UninitializedSceneRejectsCreates)
	{
		RenderScene scene;

		EXPECT_TRUE(scene.create_object({}).is_null());
		EXPECT_EQ(scene.object_count(), 0u);
		EXPECT_EQ(scene.slot_count(), 0u);
	}

	TEST(RenderScene, CreateAscendsSlotsAndMarksThemDirty)
	{
		RenderScene scene;
		scene.init(8);

		const RenderObjectDef def{
			.geometry = {3, 1},
			.material = {5, 1},
			.layers	  = 0b110,
			.flags	  = ObjectFlags::CastsShadow | ObjectFlags::Billboard,
		};

		const auto a = scene.create_object(def);
		const auto b = scene.create_object(def);
		const auto c = scene.create_object(def);

		EXPECT_EQ(a.index, 0u);
		EXPECT_EQ(b.index, 1u);
		EXPECT_EQ(c.index, 2u);

		EXPECT_EQ(scene.object_count(), 3u);
		EXPECT_EQ(scene.slot_count(), 3u);
		EXPECT_EQ(dirty_vector(scene), (std::vector<u32>{0, 1, 2}));

		const ObjectData& record = scene.object(b.index);

		EXPECT_EQ(record.geometry, 3u);
		EXPECT_EQ(record.material, 5u);
		EXPECT_EQ(record.layers, 0b110u);
		EXPECT_EQ(record.flags, static_cast<u32>(ObjectFlags::CastsShadow | ObjectFlags::Billboard));
	}

	TEST(RenderScene, PackedTransformMatchesGlm)
	{
		const glm::mat4 world = glm::translate(glm::mat4(1.0f), {1.5f, -2.0f, 3.25f}) *
								glm::rotate(glm::mat4(1.0f), 0.7f, glm::normalize(glm::vec3{1, 2, 3})) *
								glm::scale(glm::mat4(1.0f), {2.0f, 2.0f, 2.0f});

		const TransformData packed = pack_transform(world);
		const glm::vec4 point{0.3f, -1.1f, 0.9f, 1.0f};
		const glm::vec4 expected = world * point;

		EXPECT_NEAR(glm::dot(packed.rows[0], point), expected.x, 1e-5f);
		EXPECT_NEAR(glm::dot(packed.rows[1], point), expected.y, 1e-5f);
		EXPECT_NEAR(glm::dot(packed.rows[2], point), expected.z, 1e-5f);
	}

	TEST(RenderScene, WorldSphereIsConservativeUnderNonUniformScale)
	{
		const glm::mat4 world =
			glm::translate(glm::mat4(1.0f), {10.0f, 0.0f, 0.0f}) * glm::scale(glm::mat4(1.0f), {2.0f, 3.0f, 4.0f});

		const glm::vec4 sphere = transform_sphere(pack_transform(world), {1.0f, 0.0f, 0.0f, 0.5f});

		// Center transforms exactly; the radius takes the largest axis scale.
		EXPECT_NEAR(sphere.x, 12.0f, 1e-5f);
		EXPECT_NEAR(sphere.y, 0.0f, 1e-5f);
		EXPECT_NEAR(sphere.z, 0.0f, 1e-5f);
		EXPECT_NEAR(sphere.w, 2.0f, 1e-5f);
	}

	TEST(RenderScene, SetTransformRewritesTheWorldSphere)
	{
		RenderScene scene;
		scene.init(4);

		const auto handle = scene.create_object({.sphere = {0.0f, 0.0f, 0.0f, 1.0f}});
		scene.clear_dirty();

		scene.set_transform(handle, glm::translate(glm::mat4(1.0f), {5.0f, 6.0f, 7.0f}));

		const ObjectData& record = scene.object(handle.index);

		EXPECT_NEAR(record.sphere.x, 5.0f, 1e-5f);
		EXPECT_NEAR(record.sphere.y, 6.0f, 1e-5f);
		EXPECT_NEAR(record.sphere.z, 7.0f, 1e-5f);
		EXPECT_NEAR(record.sphere.w, 1.0f, 1e-5f);

		const TransformData& rows = scene.transform(handle.index);

		EXPECT_NEAR(rows.rows[0].w, 5.0f, 1e-5f);
		EXPECT_NEAR(rows.rows[1].w, 6.0f, 1e-5f);
		EXPECT_NEAR(rows.rows[2].w, 7.0f, 1e-5f);
	}

	TEST(RenderScene, DirtyListDeduplicatesAcrossMutations)
	{
		RenderScene scene;
		scene.init(4);

		const auto a = scene.create_object({});
		const auto b = scene.create_object({});

		scene.set_transform(a, glm::mat4(1.0f));
		scene.set_material(a, MaterialHandle{7, 1});
		scene.set_transform(b, glm::mat4(1.0f));

		EXPECT_EQ(dirty_vector(scene), (std::vector<u32>{0, 1}));
		EXPECT_EQ(scene.object(a.index).material, 7u);
	}

	TEST(RenderScene, ClearDirtyResetsBitsSoSlotsRedirty)
	{
		RenderScene scene;
		scene.init(4);

		const auto handle = scene.create_object({});
		scene.clear_dirty();

		EXPECT_TRUE(scene.dirty_slots().empty());

		scene.set_transform(handle, glm::mat4(1.0f));

		EXPECT_EQ(dirty_vector(scene), (std::vector<u32>{handle.index}));
	}

	TEST(RenderScene, DestroyScrubsTheRecordAndKeepsItReadable)
	{
		RenderScene scene;
		scene.init(4);

		const auto handle = scene.create_object({.layers = LAYER_ALL});
		scene.clear_dirty();

		scene.destroy_object(handle);

		EXPECT_FALSE(scene.is_valid(handle));
		EXPECT_EQ(scene.object_count(), 0u);
		EXPECT_EQ(dirty_vector(scene), (std::vector<u32>{handle.index}));

		// The dead slot serves the scrub for upload: layer zero retires it on
		// the GPU and the sphere is degenerate.
		const ObjectData& record = scene.object(handle.index);

		EXPECT_EQ(record.layers, 0u);
		EXPECT_EQ(record.sphere.w, 0.0f);

		// Destroy of the now stale handle stays a quiet no-op.
		scene.destroy_object(handle);
		EXPECT_EQ(dirty_vector(scene), (std::vector<u32>{handle.index}));
	}

	TEST(RenderScene, SameFrameSlotReuseCoalescesToTheNewRecord)
	{
		RenderScene scene;
		scene.init(2);

		const auto a = scene.create_object({});
		const auto b = scene.create_object({});
		(void)b;
		scene.clear_dirty();

		// A full pool forces the FIFO ring to hand slot 0 straight back.
		scene.destroy_object(a);
		const auto reused = scene.create_object({.layers = 0b100});

		EXPECT_EQ(reused.index, a.index);
		EXPECT_NE(reused.generation, a.generation);
		EXPECT_FALSE(scene.is_valid(a));

		// One dirty entry, and the slot reads the new object; the scrub was
		// subsumed by the create that followed it.
		EXPECT_EQ(dirty_vector(scene), (std::vector<u32>{a.index}));
		EXPECT_EQ(scene.object(a.index).layers, 0b100u);
	}

	TEST(RenderScene, SlotCountIsAHighWaterMark)
	{
		RenderScene scene;
		scene.init(8);

		const auto a = scene.create_object({});
		const auto b = scene.create_object({});
		const auto c = scene.create_object({});

		scene.destroy_object(a);
		scene.destroy_object(b);
		scene.destroy_object(c);

		// GPU tables keep covering scrubbed slots, so the bound never shrinks.
		EXPECT_EQ(scene.object_count(), 0u);
		EXPECT_EQ(scene.slot_count(), 3u);
	}

	TEST(RenderScene, SceneFullReturnsNullWithoutDirtying)
	{
		RenderScene scene;
		scene.init(1);

		const auto a = scene.create_object({});
		scene.clear_dirty();

		const auto overflow = scene.create_object({});

		EXPECT_FALSE(a.is_null());
		EXPECT_TRUE(overflow.is_null());
		EXPECT_TRUE(scene.dirty_slots().empty());
	}

#ifndef NDEBUG
	TEST(RenderSceneDeathTest, SetTransformOnAStaleHandleIsFatal)
	{
		EXPECT_DEATH(
			{
				RenderScene scene;
				scene.init(2);

				const auto handle = scene.create_object({});
				scene.destroy_object(handle);
				scene.set_transform(handle, glm::mat4(1.0f));
			},
			"assert");
	}

	TEST(RenderSceneDeathTest, SlotReadPastTheHighWaterIsFatal)
	{
		EXPECT_DEATH(
			{
				RenderScene scene;
				scene.init(2);
				(void)scene.object(0);
			},
			"assert");
	}
#endif
}
