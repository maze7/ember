#include <ember/render/view.h>

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

namespace
{
	using ember::f32;
	using namespace ember::render;

	// Eye at the origin looking down negative Z, 90 degree square fov, near 0.5:
	// the frustum boundaries land on round numbers.
	[[nodiscard]] View test_view()
	{
		const glm::mat4 view = glm::lookAt(glm::vec3{0.0f}, glm::vec3{0.0f, 0.0f, -1.0f}, glm::vec3{0.0f, 1.0f, 0.0f});

		return make_view(view, perspective_reverse_z(glm::radians(90.0f), 1.0f, 0.5f), {1280, 720});
	}

	TEST(View, ReverseZMapsNearToOneAndInfinityToZero)
	{
		const glm::mat4 projection = perspective_reverse_z(glm::radians(90.0f), 1.0f, 0.5f);

		const glm::vec4 near_clip = projection * glm::vec4{0.0f, 0.0f, -0.5f, 1.0f};
		const glm::vec4 far_clip  = projection * glm::vec4{0.0f, 0.0f, -50000.0f, 1.0f};

		EXPECT_NEAR(near_clip.z / near_clip.w, 1.0f, 1e-4f);
		EXPECT_LT(far_clip.z / far_clip.w, 1e-3f);
		EXPECT_GE(far_clip.z / far_clip.w, 0.0f);
	}

	TEST(View, CullsBehindTheCameraAndKeepsWhatIsInFront)
	{
		const View view = test_view();

		EXPECT_TRUE(intersects(view.frustum, {0.0f, 0.0f, -10.0f, 0.1f}));
		EXPECT_FALSE(intersects(view.frustum, {0.0f, 0.0f, 10.0f, 0.1f}));
	}

	TEST(View, InfiniteFarPlaneNeverCulls)
	{
		const View view = test_view();

		EXPECT_TRUE(intersects(view.frustum, {0.0f, 0.0f, -1.0e7f, 1.0f}));
	}

	TEST(View, NearPlaneRespectsTheRadius)
	{
		const View view = test_view();

		// Center 0.25 in front of the near plane at 0.5: out at radius 0.1,
		// straddling at radius 0.3.
		EXPECT_FALSE(intersects(view.frustum, {0.0f, 0.0f, -0.25f, 0.1f}));
		EXPECT_TRUE(intersects(view.frustum, {0.0f, 0.0f, -0.25f, 0.3f}));
	}

	TEST(View, SidePlanesMeasureInWorldUnits)
	{
		const View view = test_view();

		// At depth 10 the right boundary sits at x = 10; a center 1 outside is
		// 1/sqrt(2) from the 45 degree plane. Radii either side of that prove
		// the planes are normalized.
		EXPECT_FALSE(intersects(view.frustum, {11.0f, 0.0f, -10.0f, 0.5f}));
		EXPECT_TRUE(intersects(view.frustum, {11.0f, 0.0f, -10.0f, 1.0f}));
	}

	TEST(View, FiniteFarProjectionsCullThroughTheSamePath)
	{
		const View view = make_view(glm::mat4(1.0f), glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 100.0f), {64, 64});

		EXPECT_TRUE(intersects(view.frustum, {0.0f, 0.0f, -50.0f, 1.0f}));
		EXPECT_FALSE(intersects(view.frustum, {0.0f, 0.0f, -150.0f, 40.0f}));
		EXPECT_TRUE(intersects(view.frustum, {0.0f, 0.0f, -150.0f, 60.0f}));
	}

	TEST(View, RecoversPositionAndForwardFromTheViewMatrix)
	{
		const glm::vec3 eye{3.0f, 4.0f, 5.0f};
		const glm::mat4 view = glm::lookAt(eye, glm::vec3{0.0f}, glm::vec3{0.0f, 1.0f, 0.0f});

		const glm::vec3 position = view_position(view);
		EXPECT_NEAR(position.x, eye.x, 1e-4f);
		EXPECT_NEAR(position.y, eye.y, 1e-4f);
		EXPECT_NEAR(position.z, eye.z, 1e-4f);

		const glm::mat4 axis_view =
			glm::lookAt(glm::vec3{0.0f}, glm::vec3{2.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 1.0f, 0.0f});
		const glm::vec3 forward = view_forward(axis_view);

		EXPECT_NEAR(forward.x, 1.0f, 1e-4f);
		EXPECT_NEAR(forward.y, 0.0f, 1e-4f);
		EXPECT_NEAR(forward.z, 0.0f, 1e-4f);
	}

	TEST(View, MakeViewComposesTheMatrices)
	{
		const View view			 = test_view();
		const glm::mat4 composed = view.projection * view.view;

		for (int column = 0; column < 4; ++column)
			for (int row = 0; row < 4; ++row)
				EXPECT_EQ(view.view_projection[column][row], composed[column][row]);
	}
}
