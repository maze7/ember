#pragma once

#include "ext/vector_float2.hpp"
#include "math/rect.h"

namespace Ember
{
	class Quad
	{
	public:
		Quad(const glm::vec2& a_, const glm::vec2& b_, const glm::vec2& c_, const glm::vec2& d_) {
			m_a = a_;
			m_b = b_;
			m_c = c_;
			m_d = d_;
			m_normal_ab = m_normal_bc = m_normal_cd = m_normal_da = { 0, 0 };
			m_normals_dirty = true;
		}

		Quad(const Rectf& rect)
			: Quad(rect.top_left(), rect.top_right(), rect.bottom_right(), rect.bottom_left()) {}

		const glm::vec2& a() const { return m_a; }
		const glm::vec2& b() const { return m_b; }
		const glm::vec2& c() const { return m_c; }
		const glm::vec2& d() const { return m_d; }

		const glm::vec2& normal_ab() {
			update_normals();
			return m_normal_ab;
		}

		const glm::vec2& normal_bc() {
			update_normals();
			return m_normal_bc;
		}

		const glm::vec2& normal_cd() {
			update_normals();
			return m_normal_cd;
		}

		const glm::vec2& normal_da() {
			update_normals();
			return m_normal_da;
		}

		void set_a(const glm::vec2& value) {
			if (m_a != value) {
				m_a = value;
				m_normals_dirty = true;
			}
		}

		void set_b(const glm::vec2& value) {
			if (m_b != value) {
				m_b = value;
				m_normals_dirty = true;
			}
		}

		void set_c(const glm::vec2& value) {
			if (m_c != value) {
				m_c = value;
				m_normals_dirty = true;
			}
		}

		void set_d(const glm::vec2& value) {
			if (m_d != value) {
				m_d = value;
				m_normals_dirty = true;
			}
		}

		glm::vec2 center() const {
			return (m_a + m_b + m_c + m_d) / 4.f;
		}

		Quad& translate(const glm::vec2& amount) {
			m_a += amount;
			m_b += amount;
			m_c += amount;
			m_d += amount;
			return *this;
		}

		int points() const { return 4; }

		const glm::vec2& get_point(int index) {
			switch (index) {
				case 0: return m_a;
				case 1: return m_b;
				case 2: return m_c;
				case 3: return m_d;
				default:
					throw std::out_of_range("Index out of range");
			};
		}

		int axes() const { return 4; }

		glm::vec2 get_axis(int index) {
			switch (index) {
				case 0: return normal_ab();
				case 1: return normal_bc();
				case 2: return normal_cd();
				case 3: return normal_da();
				default:
					throw std::out_of_range("Index out of range");
			}
		}

		Rectf bounding_rect() {
			return Rectf(
				min(m_a.x, min(m_b.x, min(m_c.x, m_d.x))),
				min(m_a.y, min(m_b.y, min(m_c.y, m_d.y))),
				max(m_a.x, min(m_b.x, min(m_c.x, m_d.x))),
				max(m_a.y, min(m_b.y, min(m_c.y, m_d.y)))
			);
		}

	private:
		void update_normals() {
			if (!m_normals_dirty)
				return;

			m_normal_ab = glm::normalize(m_b - m_a);
			m_normal_ab = { -m_normal_ab.y, -m_normal_ab.x };
			m_normal_bc = glm::normalize(m_c - m_b);
			m_normal_bc = { -m_normal_bc.y, -m_normal_bc.x };
			m_normal_cd = glm::normalize(m_d - m_c);
			m_normal_cd = { -m_normal_cd.y, -m_normal_cd.x };
			m_normal_da = glm::normalize(m_d - m_a);
			m_normal_da = { -m_normal_da.y, -m_normal_da.x };
			m_normals_dirty = false;
		}

		glm::vec2 m_a;
		glm::vec2 m_b;
		glm::vec2 m_c;
		glm::vec2 m_d;
		glm::vec2 m_normal_ab;
		glm::vec2 m_normal_bc;
		glm::vec2 m_normal_cd;
		glm::vec2 m_normal_da;
		bool m_normals_dirty;
	};
}
