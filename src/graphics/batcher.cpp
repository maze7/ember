#include "graphics/batcher.h"
#include "ext/matrix_clip_space.hpp"
#include "graphics/draw_cmd.h"
#include <gtx/matrix_transform_2d.hpp>

using namespace Ember;

Batcher::Batcher() : Batcher(RenderDevice::instance()) {}

Batcher::Batcher(RenderDevice* gpu) : m_gpu(gpu) {
	// Create Mesh
	m_mesh = make_ref<Mesh<BatcherVertex, u32>>();

	// Create default Shader if it hasn't been created by another Batcher.
	auto vertex_code = load_file("assets/shaders/batcher.vert.spv");
	auto fragment_code = load_file("assets/shaders/batcher.frag.spv");
	m_default_shader = gpu->create_shader({
		.vertex = {
			.code = std::span(vertex_code),
			.num_uniform_buffers = 1,
			.entrypoint = "main",
		},
		.fragment = {
			.code = std::span(fragment_code),
			.num_samplers = 1,
			.entrypoint = "main"
		}
	});

	// Create default Material
	m_default_material = make_ref<Material>(m_default_shader);

	m_default_sampler = {
		.filter = TextureFilter::Linear,
		.wrap_x = TextureWrap::Repeat,
		.wrap_y = TextureWrap::Repeat,
	};

	clear();
}

Batcher::~Batcher() {
	if (m_gpu && RenderDevice::instance() == m_gpu) {
		m_gpu->dispose_shader(m_default_shader);
	}
}

void Batcher::clear() {
	m_vertex_count = 0;
	m_index_count = 0;
	m_mode = NORMAL_MODE;
	m_matrix = glm::mat3x2(1.0f);
	m_mesh_dirty = true;

	m_batch = Batch{
		.material = *m_default_material.get(),
		.blend = BlendMode::Premultiply,
		.texture = Handle<Texture>::null,
		.sampler = TextureSampler(),
		.offset = 0,
		.elements = 0
	};

	// clear stacks
	m_batches.clear();
	m_matrix_stack.clear();
	m_scissor_stack.clear();
	m_blend_stack.clear();
	m_material_stack.clear();
	m_sampler_stack.clear();
	m_mode_stack.clear();
}

void Batcher::upload() {
	if (m_mesh_dirty && m_index_count > 0 && m_vertex_count > 0) {
		m_mesh->clear();
		m_mesh->set_indices(m_index_buffer);
		m_mesh->set_vertices(m_vertex_buffer);
		m_mesh_dirty = false;
	}
}

void Batcher::render(const Target& target, const glm::mat4& matrix, Optional<Recti> scissor) {
	if ((m_batches.empty() && !m_batch.has_elements()) || m_vertex_count == 0)
		return;

	upload();

	auto render_batch = [&](const Batch& batch) {
		Material mat = batch.material;
		mat.vertex.set_uniform_buffer(matrix);
		if (!batch.texture.is_null()) {
			mat.fragment.samplers[0] = { batch.texture, batch.sampler };
		}

		DrawCommand cmd(const_cast<Target*>(&target), *m_mesh, mat);
		cmd.scissor = scissor.has_value() ? scissor : batch.scissor;
		cmd.blend_mode = batch.blend;
		cmd.index_offset = batch.offset * 3;
		cmd.index_count =  batch.elements * 3;

		if (cmd.index_count > 0)
			m_gpu->submit(cmd);
	};

	// Render all completed batches
	for (auto& batch : m_batches) {
		render_batch(batch);
	}

	// Render the current batch
	if (m_batch.has_elements())
		render_batch(m_batch);
}

void Batcher::render(const Target& target, Optional<Recti> viewport, Optional<Recti> scissor) {
    glm::vec2 size;

    // Determine the size for the projection matrix.
    // Use the viewport's dimensions if it exists, otherwise use the target's full size.
    if (viewport.has_value()) {
        size = { viewport->w, viewport->h };
    } else {
        // NOTE: This assumes your Target class has a method to get its size.
        // If not, you may need to pass the size in or get it from the RenderDevice.
        auto target_size = target.size();
        size = { target_size.x, target_size.y };
    }

    // Create a 2D orthographic projection matrix with a top-left origin.
    // This maps the coordinate system from (0,0) at the top-left to (width, height) at the bottom-right.
    glm::mat4 projection = glm::ortho(0.0f, size.x, size.y, 0.0f);

    // Call the main render function with the new matrix.
    render(target, projection, scissor);
}

void Batcher::set_texture(Handle<Texture> texture) {
	if (!m_batch.has_elements()) {
		m_batch.texture = texture;
	} else if (m_batch.texture != texture) {
		m_batches.push_back(m_batch);
		m_batch.texture = texture;
		m_batch.offset += m_batch.elements;
		m_batch.elements = 0;
	}
}

void Batcher::set_sampler(const TextureSampler& sampler) {
	if (!m_batch.has_elements() || m_batch.sampler == sampler) {
		m_batch.sampler = sampler;
	} else if (m_batch.sampler != sampler) {
		m_batches.push_back(m_batch);
		m_batch.sampler = sampler;
		m_batch.offset += m_batch.elements;
		m_batch.elements = 0;
	}
}

void Batcher::set_material(const Material& material) {
    if (!m_batch.has_elements()) {
		m_batch.material = material;
	} else if (m_batch.material != material) {
		m_batches.push_back(m_batch);
		m_batch.material = material;
		m_batch.offset += m_batch.elements;
		m_batch.elements = 0;
	}
}

void Batcher::set_scissor(const std::optional<Recti>& scissor) {
	if (!m_batch.has_elements()) {
		m_batch.scissor = scissor;
	} else if (m_batch.scissor != scissor) {
		m_batches.push_back(m_batch);
		m_batch.scissor = scissor;
		m_batch.offset += m_batch.elements;
		m_batch.elements = 0;
	}
}

void Batcher::set_blend(BlendMode blend) {
	if (!m_batch.has_elements()) {
		m_batch.blend = blend;
	} else if (m_batch.blend != blend) {
		m_batches.push_back(m_batch);
		m_batch.blend = blend;
		m_batch.offset += m_batch.elements;
		m_batch.elements = 0;
	}
}

void Batcher::push_matrix(const glm::mat3& matrix, bool relative) {
	m_matrix_stack.push_back(m_matrix);
	m_matrix = relative ? m_matrix * matrix : matrix;
}

glm::mat3 Batcher::pop_matrix() {
	m_matrix = m_matrix_stack.back();
	m_matrix_stack.pop_back();
	return m_matrix;
}

void Batcher::push_scissor(const Recti& scissor) {
	m_scissor_stack.push_back(m_batch.scissor);
	set_scissor(scissor);
}

void Batcher::pop_scissor() {
	set_scissor(m_scissor_stack.back());
	m_scissor_stack.pop_back();
}

void Batcher::push_blend(BlendMode blend) {
	m_blend_stack.push_back(m_batch.blend);
	set_blend(blend);
}

void Batcher::pop_blend() {
	set_blend(m_blend_stack.back());
	m_blend_stack.pop_back();
}

void Batcher::push_mode(Modes mode) {
	m_mode_stack.push_back(m_mode);
	switch (mode) {
		case Modes::Normal: m_mode = NORMAL_MODE; break;
		case Modes::Wash: m_mode = WASH_MODE; break;
		case Modes::Fill: m_mode = FILL_MODE; break;
	}
}

void Batcher::pop_mode() {
	m_mode = m_mode_stack.back();
	m_mode_stack.pop_back();
}

void Batcher::quad(const glm::vec2& v0, const glm::vec2& v1, const glm::vec2& v2, const glm::vec2& v3, const Color& color) {
	quad(v0, v1, v2, v3, color, color, color, color);
}

void Batcher::quad(const glm::vec2& v0, const glm::vec2& v1, const glm::vec2& v2, const glm::vec2& v3, const Color& c0, const Color& c1, const Color& c2, const Color& c3) {
	set_texture(m_gpu->default_texture()); // No texture for solid shapes
	std::span<BatcherVertex> vertices;
	std::span<u32> indices;
	u32 offset;

	request(4, 6, vertices, indices, offset);

	vertices[0] = { glm::vec2(m_matrix * glm::vec3(v0, 1.0f)), {}, c0, FILL_MODE };
	vertices[1] = { glm::vec2(m_matrix * glm::vec3(v1, 1.0f)), {}, c1, FILL_MODE };
	vertices[2] = { glm::vec2(m_matrix * glm::vec3(v2, 1.0f)), {}, c2, FILL_MODE };
	vertices[3] = { glm::vec2(m_matrix * glm::vec3(v3, 1.0f)), {}, c3, FILL_MODE };

	indices[0] = offset + 0;
	indices[1] = offset + 1;
	indices[2] = offset + 2;
	indices[3] = offset + 0;
	indices[4] = offset + 2;
	indices[5] = offset + 3;
}

void Batcher::quad(Handle<Texture> texture, const glm::vec2& v0, const glm::vec2& v1, const glm::vec2& v2, const glm::vec2& v3, const glm::vec2& t0, const glm::vec2& t1, const glm::vec2& t2, const glm::vec2& t3, const Color& color) {
	set_texture(texture);
	std::span<BatcherVertex> vertices;
	std::span<u32> indices;
	u32 offset;
	request(4, 6, vertices, indices, offset);

	vertices[0] = { glm::vec2(m_matrix * glm::vec3(v0, 1.0f)), t0, color, m_mode };
	vertices[1] = { glm::vec2(m_matrix * glm::vec3(v1, 1.0f)), t1, color, m_mode };
	vertices[2] = { glm::vec2(m_matrix * glm::vec3(v2, 1.0f)), t2, color, m_mode };
	vertices[3] = { glm::vec2(m_matrix * glm::vec3(v3, 1.0f)), t3, color, m_mode };

	indices[0] = offset + 0;
	indices[1] = offset + 1;
	indices[2] = offset + 2;
	indices[3] = offset + 0;
	indices[4] = offset + 2;
	indices[5] = offset + 3;
}

void Batcher::quad(Handle<Texture> texture, const glm::vec2& v0, const glm::vec2& v1, const glm::vec2& v2, const glm::vec2& v3, const glm::vec2& t0, const glm::vec2& t1, const glm::vec2& t2, const glm::vec2& t3, const Color& col0, const Color& col1, const Color& col2, const Color& col3) {
	set_texture(texture);
	std::span<BatcherVertex> vertices;
	std::span<u32> indices;
	u32 offset;
	request(4, 6, vertices, indices, offset);

	vertices[0] = { glm::vec2(m_matrix * glm::vec3(v0, 1.0f)), t0, col0, m_mode };
	vertices[1] = { glm::vec2(m_matrix * glm::vec3(v1, 1.0f)), t1, col1, m_mode };
	vertices[2] = { glm::vec2(m_matrix * glm::vec3(v2, 1.0f)), t2, col2, m_mode };
	vertices[3] = { glm::vec2(m_matrix * glm::vec3(v3, 1.0f)), t3, col3, m_mode };

	indices[0] = offset + 0;
	indices[1] = offset + 1;
	indices[2] = offset + 2;
	indices[3] = offset + 0;
	indices[4] = offset + 2;
	indices[5] = offset + 3;
}

void Batcher::quad_line(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c, const glm::vec2& d, float line_width, const Color& color) {
	Quad q(a, b, c, d);
	quad_line(q, line_width, color);
}

void Batcher::quad_line(Quad& q, float line_width, const Color& color) {
	auto off_ab = q.normal_ab() * line_width;
	auto off_bc = q.normal_bc() * line_width;
	auto off_cd = q.normal_cd() * line_width;
	auto off_da = q.normal_da() * line_width;

	auto aa = intersection(q.d() + off_da, q.a() + off_da, q.a() + off_ab, q.b() + off_ab);
	auto bb = intersection(q.a() + off_ab, q.b() + off_ab, q.b() + off_bc, q.c() + off_bc);
	auto cc = intersection(q.b() + off_bc, q.c() + off_bc, q.c() + off_cd, q.d() + off_cd);
	auto dd = intersection(q.c() + off_cd, q.d() + off_cd, q.d() + off_da, q.a() + off_da);

	quad(aa, q.a(), q.b(), bb, color);
	quad(bb, q.b(), q.c(), cc, color);
	quad(cc, q.c(), q.d(), dd, color);
	quad(dd, q.d(), q.a(), aa, color);
}

void Batcher::rect(const Rectf& rect, const Color& color) {
	quad(
		{ rect.x, rect.y },
		{ rect.x + rect.w, rect.y },
		{ rect.x + rect.w, rect.y + rect.h },
		{ rect.x, rect.y + rect.h },
		color
	);
}

void Batcher::rect(const glm::vec2& pos, const glm::vec2& size, Color color) {
	quad(
		pos,
		pos + glm::vec2(size.x, 0),
		pos + glm::vec2(size.x, size.y),
		pos + glm::vec2(0, size.y),
		color
	);
}

void Batcher::rect(float x, float y, float width, float height, Color color) {
	quad(
		{ x, y },
		{ x + width, y },
		{ x + width, y + height },
		{ x, y + height },
		color
	);
}

void Batcher::rect(const Rectf& rect, Color c0, Color c1, Color c2, Color c3) {
	quad(
		{ rect.x, rect.y },
		{ rect.x + rect.w, rect.y },
		{ rect.x + rect.w, rect.y + rect.h },
		{ rect.x, rect.y + rect.h },
		c0, c1, c2, c3
	);
}

void Batcher::rect(const glm::vec2& pos, const glm::vec2& size, Color c0, Color c1, Color c2, Color c3) {
	quad(
		pos,
		pos + glm::vec2(size.x, 0),
		pos + glm::vec2(size.x, size.y),
		pos + glm::vec2(0, size.y),
		c0, c1, c2, c3
	);
}

void Batcher::rect(float x, float y, float width, float height, Color c0, Color c1, Color c2, Color c3) {
	quad(
		{ x, y },
		{ x + width, y },
		{ x + width, y + height },
		{ x, y + height },
		c0, c1, c2, c3
	);
}

void Batcher::rect_line(const Rectf& r, float line_width, Color color) {
	if (line_width >= r.w / 2 || line_width >= r.h / 2) {
		rect(r, color);
	} else if (line_width > 0) {
		rect(r.x, r.y, r.w, line_width, color);
		rect(r.x, r.bottom() - line_width, r.w, line_width, color);
		rect(r.x, r.y + line_width, line_width, r.h - line_width * 2, color);
		rect(r.right() - line_width, r.y + line_width, line_width, r.h - line_width * 2, color);
	}
}

void Batcher::rect_dashed(Rectf r, float line_width, Color color, float dash_length, float dash_offset) {
	r = r.inflate(-line_width / 2);
	line_dashed(r.top_left(), r.top_right(), line_width, color, dash_length, dash_offset);
	line_dashed(r.top_right(), r.bottom_right(), line_width, color, dash_length, dash_offset);
	line_dashed(r.bottom_right(), r.bottom_left(), line_width, color, dash_length, dash_offset);
	line_dashed(r.bottom_left(), r.top_left(), line_width, color, dash_length, dash_offset);
}

void Batcher::line_dashed(const glm::vec2& from, const glm::vec2& to, float line_width, Color color, float dash_length, float offset_percent) {
    auto diff = to - from;
    float dist = glm::length(diff);

    // Handle edge cases to prevent errors
    if (dist < 0.0001f || dash_length <= 0.f) {
        return; // Nothing to draw or would cause an infinite loop
    }

    auto axis = diff / dist;
    auto perp = glm::vec2(axis.y, -axis.x) * (line_width * 0.5f);

    offset_percent = fmodf(offset_percent, 1.0f);
    if (offset_percent < 0.0f) {
        offset_percent += 1.0f;
    }

    auto start_d = dash_length * offset_percent * 2.f;
    if (start_d > dash_length) {
        start_d -= dash_length * 2.f;
    }

    // Loop and draw the dashes
    for (float d = start_d; d < dist; d += dash_length * 2.f) {
        auto a = from + axis * std::max(d, 0.f);
        auto b = from + axis * std::min(d + dash_length, dist);

        // Only draw if the segment is visible
        if ((d + dash_length) > 0.f && d < dist) {
            quad(a + perp, b + perp, b - perp, a - perp, color);
        }
    }
}

void Batcher::image(const SubTexture& subtex, Color color) {
	quad(
		subtex.texture->handle(),
		subtex.draw_coords[0], subtex.draw_coords[1], subtex.draw_coords[2], subtex.draw_coords[3],
		subtex.tex_coords[0], subtex.tex_coords[1], subtex.tex_coords[2], subtex.tex_coords[3],
		color
	);
}

void Batcher::image(const SubTexture& subtex, const glm::vec2& pos, Color color) {
	quad(
		subtex.texture->handle(),
		pos + subtex.draw_coords[0], pos + subtex.draw_coords[1], pos + subtex.draw_coords[2], pos + subtex.draw_coords[3],
		subtex.tex_coords[0], subtex.tex_coords[1], subtex.tex_coords[2], subtex.tex_coords[3],
		color
	);
}

void Batcher::image(const SubTexture& subtex, const glm::vec2& position, const glm::vec2& origin, const glm::vec2& scale, float rotation, const Color& color) {
	glm::mat3 transform = glm::mat3(1.0f);
	transform = glm::translate(transform, position);
	transform = glm::rotate(transform, rotation);
	transform = glm::scale(transform, scale);
	transform = glm::translate(transform, -origin);

	// Push the new transform onto the stack, applying it relative to the current matrix.
	push_matrix(transform, true);

	// Draw the quad. The quad function will automatically use the new combined matrix
	// to transform the vertices.
	quad(
		subtex.texture->handle(),
		subtex.draw_coords[0], subtex.draw_coords[1], subtex.draw_coords[2], subtex.draw_coords[3],
		subtex.tex_coords[0], subtex.tex_coords[1], subtex.tex_coords[2], subtex.tex_coords[3],
		color
	);

	// Restore the previous matrix from the stack to not affect subsequent draw calls.
	pop_matrix();
}

void Batcher::request(u32 vertex_append_count, u32 index_append_count, std::span<BatcherVertex>& out_vertices, std::span<u32>& out_indices, u32& out_vertex_offset) {
	out_vertex_offset = m_vertex_count;

	// Performance: Reserve capacity in larger chunks to avoid frequent reallocations.
	if (m_vertex_count + vertex_append_count > m_vertex_buffer.capacity()) {
		m_vertex_buffer.reserve(std::max(m_vertex_buffer.capacity() * 2, (size_t)m_vertex_count + vertex_append_count));
	}
	if (m_index_count + index_append_count > m_index_buffer.capacity()) {
		m_index_buffer.reserve(std::max(m_index_buffer.capacity() * 2, (size_t)m_index_count + index_append_count));
	}

	// Create spans pointing to the end of the existing data
	out_vertices = { m_vertex_buffer.data() + m_vertex_count, vertex_append_count };
	out_indices = { m_index_buffer.data() + m_index_count, index_append_count };

	// Note: We write past the "size" but within "capacity".
	// We will update the final size after writing is complete.
	m_vertex_buffer.resize(m_vertex_count + vertex_append_count);
	m_index_buffer.resize(m_index_count + index_append_count);

	// Update totals
	m_index_count += index_append_count;
	m_vertex_count += vertex_append_count;
	m_batch.elements += index_append_count / 3;
	m_mesh_dirty = true;
}

glm::vec2 Batcher::intersection(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& q0, const glm::vec2& q1) {
	auto aa = p1 - p0;
	auto bb = q0 - q1;
	auto cc = q0 - p0;
	auto t = (bb.x * cc.y - bb.y * cc.x) / (aa.y * bb.x - aa.x * bb.y);
	return { p0.x + t * (p1.x - p0.x), p0.y + t * (p1.y - p0.y) };
}
