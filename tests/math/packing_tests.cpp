#include <ember/math/packing.h>

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

#include <random>

namespace
{
	using ember::pack_octahedral;
	using ember::unpack_octahedral;

	TEST(Packing, OctahedralRoundTripsTheAxes)
	{
		const glm::vec3 axes[] = {
			{1, 0, 0},
			{-1, 0, 0},
			{0, 1, 0},
			{0, -1, 0},
			{0, 0, 1},
			{0, 0, -1},
		};

		for (const glm::vec3& axis : axes)
		{
			const glm::vec3 decoded = unpack_octahedral(pack_octahedral(axis));
			EXPECT_NEAR(glm::dot(decoded, axis), 1.0f, 1e-4f);
		}
	}

	TEST(Packing, OctahedralRoundTripsRandomDirections)
	{
		std::mt19937 random(0x0C7A);
		std::uniform_real_distribution<float> unit(-1.0f, 1.0f);

		for (int i = 0; i < 1000; ++i)
		{
			glm::vec3 direction{unit(random), unit(random), unit(random)};
			if (glm::dot(direction, direction) < 1e-4f)
				continue;

			direction = glm::normalize(direction);

			const glm::vec3 decoded = unpack_octahedral(pack_octahedral(direction));

			// snorm16 quantization bounds the angular error well under a degree.
			EXPECT_GT(glm::dot(decoded, direction), 0.9999f);
		}
	}
}
