#include "voxel_mesher_blocky.h"
#include "../../constants/cube_tables.h"
#include "../../storage/voxel_buffer.h"
#include "../../util/containers/span.h"
#include "../../util/godot/core/array.h"
#include "../../util/godot/core/packed_arrays.h"
#include "../../util/macros.h"
#include "../../util/math/conv.h"
#include "../../util/math/funcs.h"
// TODO GDX: String has no `operator+=`
#include "../../util/containers/container_funcs.h"
#include "../../util/godot/core/string.h"
#include "../../util/profiling.h"

using namespace zylann::godot;

namespace zylann::voxel {

// Utility functions
namespace {

inline bool contributes_to_ao(const VoxelBlockyLibraryBase::BakedData &lib, uint32_t voxel_id) {
	if (voxel_id < lib.models.size()) {
		const VoxelBlockyModel::BakedData &t = lib.models[voxel_id];
		return t.contributes_to_ao;
	}
	return true;
}

StdVector<int> &get_tls_index_offsets() {
	static thread_local StdVector<int> tls_index_offsets;
	return tls_index_offsets;
}

void copy(const VoxelBlockyFluid::Surface &src, Vector2f src_uv, VoxelBlockyModel::SideSurface &dst) {
	copy(src.positions, dst.positions);

	dst.uvs.resize(src.positions.size());
	fill(dst.uvs, src_uv);

	copy(src.indices, dst.indices);
	// TODO Aren't tangents always the same on sides too? Like normals?
	copy(src.tangents, dst.tangents);
}

void copy_positions_normals_tangents(
		const VoxelBlockyFluid::Surface &src,
		const Vector3f normal,
		const uint32_t p_material_id,
		const bool p_collision_enabled,
		VoxelBlockyModel::Surface &dst
) {
	copy(src.positions, dst.positions);

	dst.normals.resize(src.positions.size());
	fill(dst.normals, normal);
	// std::fill(dst.normals.begin(), dst.normals.end(), normal);

	// copy(src.uvs, dst.uvs);
	copy(src.indices, dst.indices);
	copy(src.tangents, dst.tangents);

	dst.material_id = p_material_id;
	dst.collision_enabled = p_collision_enabled;
}

static const VoxelBlockyFluid::FlowState g_min_corners_mask_to_flowstate[16] = {
	// 0123
	// ----
	// 0000
	VoxelBlockyFluid::FLOW_IDLE, // Impossible
	// 0001
	VoxelBlockyFluid::FLOW_DIAGONAL_POSITIVE_X_POSITIVE_Z,
	// 0010
	VoxelBlockyFluid::FLOW_DIAGONAL_NEGATIVE_X_POSITIVE_Z,
	// 0011
	VoxelBlockyFluid::FLOW_STRAIGHT_POSITIVE_Z,
	// 0100
	VoxelBlockyFluid::FLOW_DIAGONAL_NEGATIVE_X_NEGATIVE_Z,
	// 0101
	VoxelBlockyFluid::FLOW_IDLE, // Ambiguous
	// 0110
	VoxelBlockyFluid::FLOW_STRAIGHT_NEGATIVE_X,
	// 0111
	VoxelBlockyFluid::FLOW_DIAGONAL_NEGATIVE_X_POSITIVE_Z,
	// 1000
	VoxelBlockyFluid::FLOW_DIAGONAL_POSITIVE_X_NEGATIVE_Z,
	// 1001
	VoxelBlockyFluid::FLOW_STRAIGHT_POSITIVE_X,
	// 1010
	VoxelBlockyFluid::FLOW_IDLE, // Ambiguous
	// 1011
	VoxelBlockyFluid::FLOW_DIAGONAL_POSITIVE_X_POSITIVE_Z,
	// 1100
	VoxelBlockyFluid::FLOW_STRAIGHT_NEGATIVE_Z,
	// 1101
	VoxelBlockyFluid::FLOW_DIAGONAL_POSITIVE_X_NEGATIVE_Z,
	// 1110
	VoxelBlockyFluid::FLOW_DIAGONAL_NEGATIVE_X_NEGATIVE_Z,
	// 1111
	VoxelBlockyFluid::FLOW_IDLE,
};

VoxelBlockyFluid::FlowState get_fluid_flow_state_from_corner_levels(
		//    3-------2
		//   /|      /|        z
		//  / |     / |       /
		// 0-------1     x---o
		// |       |
		const FixedArray<uint8_t, 4> corner_levels
) {
	const uint8_t min_level = math::min(corner_levels[0], corner_levels[1], corner_levels[2], corner_levels[3]);
	const uint8_t mask = //
			(static_cast<uint8_t>(corner_levels[0] == min_level) << 3) | //
			(static_cast<uint8_t>(corner_levels[1] == min_level) << 2) | //
			(static_cast<uint8_t>(corner_levels[2] == min_level) << 1) | //
			(static_cast<uint8_t>(corner_levels[3] == min_level) << 0);
	return g_min_corners_mask_to_flowstate[mask];
}

FixedArray<uint8_t, 4> get_corner_levels_from_fluid_levels(
		//  8 7 6     z
		//  5 4 3     |
		//  2 1 0  x--o
		const FixedArray<uint8_t, 9> fluid_levels
) {
	//    3-------2
	//   /|      /|        z
	//  / |     / |       /
	// 0-------1     x---o
	// |       |
	FixedArray<uint8_t, 4> corner_levels;

	corner_levels[0] = math::max(fluid_levels[1], fluid_levels[2], fluid_levels[4], fluid_levels[5]);
	corner_levels[1] = math::max(fluid_levels[0], fluid_levels[1], fluid_levels[3], fluid_levels[4]);
	corner_levels[2] = math::max(fluid_levels[3], fluid_levels[4], fluid_levels[6], fluid_levels[7]);
	corner_levels[3] = math::max(fluid_levels[4], fluid_levels[5], fluid_levels[7], fluid_levels[8]);

	return corner_levels;
}

FixedArray<float, 4> get_corner_heights_from_corner_levels(
		const FixedArray<uint8_t, 4> corner_levels,
		const VoxelBlockyFluid::BakedData &fluid
) {
	struct LevelToHeight {
		const float max_level_inv;

		inline float operator()(uint8_t level) const {
			return math::lerp(
					VoxelBlockyFluid::BakedData::BOTTOM_HEIGHT,
					VoxelBlockyFluid::BakedData::TOP_HEIGHT,
					static_cast<float>(level) * max_level_inv
			);
		}
	};

	// TODO Disallow fluids with only one level
	const LevelToHeight level_to_height{ 1.f / static_cast<float>(fluid.max_level) };

	FixedArray<float, 4> heights;
	heights[0] = level_to_height(corner_levels[0]);
	heights[1] = level_to_height(corner_levels[1]);
	heights[2] = level_to_height(corner_levels[2]);
	heights[3] = level_to_height(corner_levels[3]);

	return heights;
}

FixedArray<FixedArray<VoxelBlockyModel::SideSurface, VoxelBlockyModel::MAX_SURFACES>, Cube::SIDE_COUNT> &
get_tls_fluid_sides_surfaces() {
	static thread_local FixedArray<
			FixedArray<VoxelBlockyModel::SideSurface, VoxelBlockyModel::MAX_SURFACES>,
			Cube::SIDE_COUNT>
			tls_fluid_sides;
	return tls_fluid_sides;
}

VoxelBlockyModel::Surface &get_tls_fluid_top() {
	static thread_local VoxelBlockyModel::Surface tls_fluid_top;
	return tls_fluid_top;
}

inline void transpose_quad_triangles(Span<int32_t> indices) {
	// Assumes triangles are like this:
	// 3---2
	// |   |  {0, 2, 1, 0, 3, 2} --> { 0, 3, 1, 1, 3, 2 }
	// 0---1
	indices[1] = indices[4];
	indices[3] = indices[2];
}

template <typename TModelID>
void generate_fluid_model(
		const VoxelBlockyModel::BakedData &voxel,
		const Span<const TModelID> type_buffer,
		const int voxel_index,
		const int y_jump_size,
		const int x_jump_size,
		const int z_jump_size,
		const VoxelBlockyLibraryBase::BakedData &library,
		Span<const VoxelBlockyModel::Surface> &out_model_surfaces,
		const FixedArray<FixedArray<VoxelBlockyModel::SideSurface, VoxelBlockyModel::MAX_SURFACES>, Cube::SIDE_COUNT> *
				&out_model_sides_surfaces,
		uint8_t &out_model_surface_count
) {
	const uint32_t top_voxel_id = type_buffer[voxel_index + y_jump_size];

	bool fluid_top_covered = false;

	if (library.has_model(top_voxel_id)) {
		const VoxelBlockyModel::BakedData &top_model = library.models[top_voxel_id];
		if (top_model.fluid_index == voxel.fluid_index) {
			fluid_top_covered = true;
		}
	}

	const VoxelBlockyFluid::BakedData &fluid = library.fluids[voxel.fluid_index];

	// Fluids have only one material
	static constexpr unsigned int surface_index = 0;

	// We re-use the same memory per thread for each meshed fluid voxel
	auto &fluid_sides = get_tls_fluid_sides_surfaces();
	auto &fluid_top_surface = get_tls_fluid_top();

	// TODO Optimize: maybe don't copy if not covered and reference instead?

	// UVs will be assigned differently than typical voxels. The shader is assumed to interpret them in order to render
	// a flowing animation. Vertex coordinates may be used as UVs instead.
	// UV.X = which axis the side is on (although it could already be deduced from normals?)
	// UV.Y = flow state (tells both direction or whether the fluid is idle)

	// Lateral sides
	// They always flow to the same direction

	copy(fluid.side_surfaces[Cube::SIDE_NEGATIVE_X],
		 Vector2f(math::AXIS_X, VoxelBlockyFluid::FLOW_STRAIGHT_POSITIVE_Z),
		 fluid_sides[Cube::SIDE_NEGATIVE_X][surface_index]);

	copy(fluid.side_surfaces[Cube::SIDE_POSITIVE_X],
		 Vector2f(math::AXIS_X, VoxelBlockyFluid::FLOW_STRAIGHT_POSITIVE_Z),
		 fluid_sides[Cube::SIDE_POSITIVE_X][surface_index]);

	copy(fluid.side_surfaces[Cube::SIDE_NEGATIVE_Z],
		 Vector2f(math::AXIS_Z, VoxelBlockyFluid::FLOW_STRAIGHT_POSITIVE_Z),
		 fluid_sides[Cube::SIDE_NEGATIVE_Z][surface_index]);

	copy(fluid.side_surfaces[Cube::SIDE_POSITIVE_Z],
		 Vector2f(math::AXIS_Z, VoxelBlockyFluid::FLOW_STRAIGHT_POSITIVE_Z),
		 fluid_sides[Cube::SIDE_POSITIVE_Z][surface_index]);

	// Bottom side
	// It is always idle

	copy(fluid.side_surfaces[Cube::SIDE_NEGATIVE_Y],
		 Vector2f(math::AXIS_Y, VoxelBlockyFluid::FLOW_IDLE),
		 fluid_sides[Cube::SIDE_NEGATIVE_Y][surface_index]);

	if (fluid_top_covered) {
		// No top side
		fluid_sides[Cube::SIDE_POSITIVE_Y][surface_index].clear();
		fluid_top_surface.clear();

	} else {
		copy_positions_normals_tangents(
				fluid.side_surfaces[Cube::SIDE_POSITIVE_Y],
				Vector3f(0.f, 1.f, 0.f),
				fluid.material_id,
				// TODO Option for collision on the fluid? Not sure if desired
				false,
				fluid_top_surface
		);

		// We'll potentially have to adjust corners of the model based on neighbor levels
		//  8 7 6     z
		//  5 4 3     |
		//  2 1 0  x--o
		FixedArray<uint8_t, 9> fluid_levels;
		uint32_t covered_neighbors = 0;
		const bool dip_when_flowing_down = fluid.dip_when_flowing_down;

		// TODO Optimize: could sample 4 neighbors first and if the max isn't the same as current level,
		// sample 4 diagonals too?
		unsigned int i = 0;
		for (int dz = -1; dz <= 1; ++dz) {
			for (int dx = -1; dx <= 1; ++dx) {
				const uint32_t nloc = voxel_index + dx * x_jump_size + dz * z_jump_size;
				const uint32_t nid = type_buffer[nloc];

				if (library.has_model(nid)) {
					const VoxelBlockyModel::BakedData &nm = library.models[nid];
					if (nm.fluid_index == voxel.fluid_index) {
						fluid_levels[i] = nm.fluid_level;

						// We don't test the current voxel, we know it's not covered
						if (i != 4) {
							const uint32_t anloc = nloc + y_jump_size;
							const uint32_t anid = type_buffer[anloc];
							if (anid != VoxelBlockyModel::AIR_ID && library.has_model(anid)) {
								const VoxelBlockyModel::BakedData &anm = library.models[anid];
								if (anm.fluid_index == voxel.fluid_index) {
									covered_neighbors |= (1 << i);
								}
							}
						}

						if (dip_when_flowing_down) {
							// When a non-covered fluid voxel is above an area in which it can flow down, fake its level
							// to be 0 (even if it isn't really) in order to create a steep slope.
							// Do this except on max level fluids, which can "sustain" themselves. If we don't do this,
							// lakes and oceans would end up looking lower than they should (assuming their surface is
							// covered in max level fluid).
							if (nm.fluid_level != fluid.max_level && (covered_neighbors & (1 << i)) == 0) {
								const uint32_t bnloc = nloc - y_jump_size;
								const uint32_t bnid = type_buffer[bnloc];
								if (bnid == VoxelBlockyModel::AIR_ID) {
									fluid_levels[i] = 0;
								} else if (library.has_model(bnid)) {
									const VoxelBlockyModel::BakedData &bnm = library.models[bnid];
									if (bnm.fluid_index == voxel.fluid_index) {
										fluid_levels[i] = 0;
									}
								}
							}
						}

					} else {
						fluid_levels[i] = 0;
					}
				} else {
					fluid_levels[i] = 0;
				}

				++i;
			}
		}

		// Adjust top corner heights to form slopes.

		const FixedArray<uint8_t, 4> corner_levels = get_corner_levels_from_fluid_levels(fluid_levels);
		const VoxelBlockyFluid::FlowState flow_state = get_fluid_flow_state_from_corner_levels(corner_levels);
		//    3-------2
		//   /|      /|        z
		//  / |     / |       /
		// 0-------1     x---o
		// |       |
		FixedArray<float, 4> corner_heights = get_corner_heights_from_corner_levels(corner_levels, fluid);

		//  8 7 6     z
		//  5 4 3     |
		//  2 1 0  x--o
		// Covered neighbors need to be considered at full height
		if ((covered_neighbors & 0b000'001'011) != 0) {
			corner_heights[1] = 1.f;
		}
		if ((covered_neighbors & 0b000'100'110) != 0) {
			corner_heights[0] = 1.f;
		}
		if ((covered_neighbors & 0b011'001'000) != 0) {
			corner_heights[2] = 1.f;
		}
		if ((covered_neighbors & 0b110'100'000) != 0) {
			corner_heights[3] = 1.f;
		}

		fluid_top_surface.uvs.resize(4);
		fill(fluid_top_surface.uvs, Vector2f(math::AXIS_Y, flow_state));

		// TODO Option to alter normals too so they are more "correct"? Not always needed tho?

		// For lateral sides, we assume top vertices are always the last 2, in
		// clockwise order relative to the top face
		{
			VoxelBlockyModel::SideSurface &side_surface = fluid_sides[Cube::SIDE_NEGATIVE_X][surface_index];
			side_surface.positions[2].y = corner_heights[2];
			side_surface.positions[3].y = corner_heights[1];
		}
		{
			VoxelBlockyModel::SideSurface &side_surface = fluid_sides[Cube::SIDE_POSITIVE_X][surface_index];
			side_surface.positions[2].y = corner_heights[0];
			side_surface.positions[3].y = corner_heights[3];
		}
		{
			VoxelBlockyModel::SideSurface &side_surface = fluid_sides[Cube::SIDE_NEGATIVE_Z][surface_index];
			side_surface.positions[2].y = corner_heights[1];
			side_surface.positions[3].y = corner_heights[0];
		}
		{
			VoxelBlockyModel::SideSurface &side_surface = fluid_sides[Cube::SIDE_POSITIVE_Z][surface_index];
			side_surface.positions[2].y = corner_heights[3];
			side_surface.positions[3].y = corner_heights[2];
		}
		// For the top side, we assume vertices are counter-clockwise, and the first is at (+x, -z)
		{
			fluid_top_surface.positions[0].y = corner_heights[0];
			fluid_top_surface.positions[1].y = corner_heights[1];
			fluid_top_surface.positions[2].y = corner_heights[2];
			fluid_top_surface.positions[3].y = corner_heights[3];
		}

		// We want the diagonal of the top quad's triangles to remain aligned with the flow
		if (flow_state == VoxelBlockyFluid::FLOW_DIAGONAL_POSITIVE_X_POSITIVE_Z ||
			flow_state == VoxelBlockyFluid::FLOW_DIAGONAL_NEGATIVE_X_NEGATIVE_Z) {
			transpose_quad_triangles(to_span(fluid_top_surface.indices));
		}
	}

	// Override model data with procedural data
	out_model_surface_count = 1;

	if (fluid_top_covered) {
		// Expected to be empty, but also provides material ID. Not great tho
		out_model_surfaces = to_span(voxel.model.surfaces);
	} else {
		out_model_surfaces = to_single_element_span(fluid_top_surface);
	}

	out_model_sides_surfaces = &fluid_sides;
}

} // namespace

void generate_preview_fluid_model(
		const VoxelBlockyModel::BakedData &model,
		const uint16_t model_id,
		const VoxelBlockyLibraryBase::BakedData &library,
		Span<const VoxelBlockyModel::Surface> &out_model_surfaces,
		const FixedArray<FixedArray<VoxelBlockyModel::SideSurface, VoxelBlockyModel::MAX_SURFACES>, Cube::SIDE_COUNT> *
				&out_model_sides_surfaces
) {
	ZN_ASSERT(model.fluid_index != VoxelBlockyModel::NULL_FLUID_INDEX);
	FixedArray<uint16_t, 3 * 3 * 3> id_buffer;
	fill(id_buffer, VoxelBlockyModel::AIR_ID);
	const int center_loc = Vector3iUtil::get_zxy_index(Vector3i(1, 1, 1), Vector3i(3, 3, 3));
	id_buffer[center_loc] = model_id;
	uint8_t unused_surface_count;
	generate_fluid_model<uint16_t>(
			model,
			to_span(id_buffer),
			center_loc,
			1,
			3,
			3 * 3,
			library,
			out_model_surfaces,
			out_model_sides_surfaces,
			unused_surface_count
	);
}

template <typename Type_T>
void generate_blocky_mesh(
		StdVector<VoxelMesherBlocky::Arrays> &out_arrays_per_material,
		VoxelMesher::Output::CollisionSurface *collision_surface,
		const Span<const Type_T> type_buffer,
		const Vector3i block_size,
		const VoxelBlockyLibraryBase::BakedData &library,
		const bool bake_occlusion,
		const float baked_occlusion_darkness
) {
	// TODO Optimization: not sure if this mandates a template function. There is so much more happening in this
	// function other than reading voxels, although reading is on the hottest path. It needs to be profiled. If
	// changing makes no difference, we could use a function pointer or switch inside instead to reduce executable size.

	ERR_FAIL_COND(
			block_size.x < static_cast<int>(2 * VoxelMesherBlocky::PADDING) ||
			block_size.y < static_cast<int>(2 * VoxelMesherBlocky::PADDING) ||
			block_size.z < static_cast<int>(2 * VoxelMesherBlocky::PADDING)
	);

	// Build lookup tables so to speed up voxel access.
	// These are values to add to an address in order to get given neighbor.

	const int row_size = block_size.y;
	const int deck_size = block_size.x * row_size;

	// Data must be padded, hence the off-by-one
	const Vector3i min = Vector3iUtil::create(VoxelMesherBlocky::PADDING);
	const Vector3i max = block_size - Vector3iUtil::create(VoxelMesherBlocky::PADDING);

	StdVector<int> &index_offsets = get_tls_index_offsets();
	index_offsets.clear();
	index_offsets.resize(out_arrays_per_material.size(), 0);

	int collision_surface_index_offset = 0;

	FixedArray<int, Cube::SIDE_COUNT> side_neighbor_lut;
	side_neighbor_lut[Cube::SIDE_LEFT] = row_size;
	side_neighbor_lut[Cube::SIDE_RIGHT] = -row_size;
	side_neighbor_lut[Cube::SIDE_BACK] = -deck_size;
	side_neighbor_lut[Cube::SIDE_FRONT] = deck_size;
	side_neighbor_lut[Cube::SIDE_BOTTOM] = -1;
	side_neighbor_lut[Cube::SIDE_TOP] = 1;

	FixedArray<int, Cube::EDGE_COUNT> edge_neighbor_lut;
	edge_neighbor_lut[Cube::EDGE_BOTTOM_BACK] =
			side_neighbor_lut[Cube::SIDE_BOTTOM] + side_neighbor_lut[Cube::SIDE_BACK];
	edge_neighbor_lut[Cube::EDGE_BOTTOM_FRONT] =
			side_neighbor_lut[Cube::SIDE_BOTTOM] + side_neighbor_lut[Cube::SIDE_FRONT];
	edge_neighbor_lut[Cube::EDGE_BOTTOM_LEFT] =
			side_neighbor_lut[Cube::SIDE_BOTTOM] + side_neighbor_lut[Cube::SIDE_LEFT];
	edge_neighbor_lut[Cube::EDGE_BOTTOM_RIGHT] =
			side_neighbor_lut[Cube::SIDE_BOTTOM] + side_neighbor_lut[Cube::SIDE_RIGHT];
	edge_neighbor_lut[Cube::EDGE_BACK_LEFT] = side_neighbor_lut[Cube::SIDE_BACK] + side_neighbor_lut[Cube::SIDE_LEFT];
	edge_neighbor_lut[Cube::EDGE_BACK_RIGHT] = side_neighbor_lut[Cube::SIDE_BACK] + side_neighbor_lut[Cube::SIDE_RIGHT];
	edge_neighbor_lut[Cube::EDGE_FRONT_LEFT] = side_neighbor_lut[Cube::SIDE_FRONT] + side_neighbor_lut[Cube::SIDE_LEFT];
	edge_neighbor_lut[Cube::EDGE_FRONT_RIGHT] =
			side_neighbor_lut[Cube::SIDE_FRONT] + side_neighbor_lut[Cube::SIDE_RIGHT];
	edge_neighbor_lut[Cube::EDGE_TOP_BACK] = side_neighbor_lut[Cube::SIDE_TOP] + side_neighbor_lut[Cube::SIDE_BACK];
	edge_neighbor_lut[Cube::EDGE_TOP_FRONT] = side_neighbor_lut[Cube::SIDE_TOP] + side_neighbor_lut[Cube::SIDE_FRONT];
	edge_neighbor_lut[Cube::EDGE_TOP_LEFT] = side_neighbor_lut[Cube::SIDE_TOP] + side_neighbor_lut[Cube::SIDE_LEFT];
	edge_neighbor_lut[Cube::EDGE_TOP_RIGHT] = side_neighbor_lut[Cube::SIDE_TOP] + side_neighbor_lut[Cube::SIDE_RIGHT];

	FixedArray<int, Cube::CORNER_COUNT> corner_neighbor_lut;

	corner_neighbor_lut[Cube::CORNER_BOTTOM_BACK_LEFT] = side_neighbor_lut[Cube::SIDE_BOTTOM] +
			side_neighbor_lut[Cube::SIDE_BACK] + side_neighbor_lut[Cube::SIDE_LEFT];

	corner_neighbor_lut[Cube::CORNER_BOTTOM_BACK_RIGHT] = side_neighbor_lut[Cube::SIDE_BOTTOM] +
			side_neighbor_lut[Cube::SIDE_BACK] + side_neighbor_lut[Cube::SIDE_RIGHT];

	corner_neighbor_lut[Cube::CORNER_BOTTOM_FRONT_RIGHT] = side_neighbor_lut[Cube::SIDE_BOTTOM] +
			side_neighbor_lut[Cube::SIDE_FRONT] + side_neighbor_lut[Cube::SIDE_RIGHT];

	corner_neighbor_lut[Cube::CORNER_BOTTOM_FRONT_LEFT] = side_neighbor_lut[Cube::SIDE_BOTTOM] +
			side_neighbor_lut[Cube::SIDE_FRONT] + side_neighbor_lut[Cube::SIDE_LEFT];

	corner_neighbor_lut[Cube::CORNER_TOP_BACK_LEFT] =
			side_neighbor_lut[Cube::SIDE_TOP] + side_neighbor_lut[Cube::SIDE_BACK] + side_neighbor_lut[Cube::SIDE_LEFT];

	corner_neighbor_lut[Cube::CORNER_TOP_BACK_RIGHT] = side_neighbor_lut[Cube::SIDE_TOP] +
			side_neighbor_lut[Cube::SIDE_BACK] + side_neighbor_lut[Cube::SIDE_RIGHT];

	corner_neighbor_lut[Cube::CORNER_TOP_FRONT_RIGHT] = side_neighbor_lut[Cube::SIDE_TOP] +
			side_neighbor_lut[Cube::SIDE_FRONT] + side_neighbor_lut[Cube::SIDE_RIGHT];

	corner_neighbor_lut[Cube::CORNER_TOP_FRONT_LEFT] = side_neighbor_lut[Cube::SIDE_TOP] +
			side_neighbor_lut[Cube::SIDE_FRONT] + side_neighbor_lut[Cube::SIDE_LEFT];

	// uint64_t time_prep = Time::get_singleton()->get_ticks_usec() - time_before;
	// time_before = Time::get_singleton()->get_ticks_usec();

	for (unsigned int z = min.z; z < (unsigned int)max.z; ++z) {
		for (unsigned int x = min.x; x < (unsigned int)max.x; ++x) {
			for (unsigned int y = min.y; y < (unsigned int)max.y; ++y) {
				// min and max are chosen such that you can visit 1 neighbor away from the current voxel without size
				// check

				const int voxel_index = y + x * row_size + z * deck_size;
				const int voxel_id = type_buffer[voxel_index];

				// TODO Don't assume air is 0?
				if (voxel_id == VoxelBlockyModel::AIR_ID || !library.has_model(voxel_id)) {
					continue;
				}

				const VoxelBlockyModel::BakedData &voxel = library.models[voxel_id];
				const VoxelBlockyModel::BakedData::Model &model = voxel.model;

				uint8_t model_surface_count = model.surface_count;

				Span<const VoxelBlockyModel::Surface> model_surfaces = to_span(model.surfaces);

				const FixedArray<
						FixedArray<VoxelBlockyModel::SideSurface, VoxelBlockyModel::MAX_SURFACES>,
						Cube::SIDE_COUNT> *model_sides_surfaces = &model.sides_surfaces;

				// Hybrid approach: extract cube faces and decimate those that aren't visible,
				// and still allow voxels to have geometry that is not a cube.

				if (voxel.fluid_index != VoxelBlockyModel::NULL_FLUID_INDEX) {
					generate_fluid_model(
							voxel,
							type_buffer,
							voxel_index,
							1,
							row_size,
							deck_size,
							library,
							model_surfaces,
							model_sides_surfaces,
							model_surface_count
					);
				}

				// Sides
				for (unsigned int side = 0; side < Cube::SIDE_COUNT; ++side) {
					if ((model.empty_sides_mask & (1 << side)) != 0) {
						// This side is empty
						continue;
					}

					// By default we render the whole side if we consider it visible
					const FixedArray<VoxelBlockyModel::SideSurface, VoxelBlockyModel::MAX_SURFACES> *side_surfaces =
							&((*model_sides_surfaces)[side]);

					const uint32_t neighbor_voxel_id = type_buffer[voxel_index + side_neighbor_lut[side]];

					// Invalid voxels are treated like air
					if (neighbor_voxel_id < library.models.size()) {
						const VoxelBlockyModel::BakedData &other_vt = library.models[neighbor_voxel_id];
						if (!is_face_visible_regardless_of_shape(voxel, other_vt)) {
							// Visibility depends on the shape
							if (!is_face_visible_according_to_shape(library, voxel, other_vt, side)) {
								// Completely occluded
								continue;
							}

							// Might be only partially visible
							if (voxel.cutout_sides_enabled) {
								const std::unordered_map<
										uint32_t,
										FixedArray<VoxelBlockyModel::SideSurface, VoxelBlockyModel::MAX_SURFACES>>
										&cutout_side_surfaces_by_neighbor_shape = model.cutout_side_surfaces[side];

								const unsigned int neighbor_shape_id =
										other_vt.model.side_pattern_indices[Cube::g_opposite_side[side]];

								// That's a hashmap lookup on a hot path. Cutting out sides like this should be used
								// sparsely if possible.
								// Unfortunately, use cases include certain water styles, which means oceans...
								// Eventually we should provide another approach for these
								auto it = cutout_side_surfaces_by_neighbor_shape.find(neighbor_shape_id);

								if (it != cutout_side_surfaces_by_neighbor_shape.end()) {
									// Use pre-cut side instead
									side_surfaces = &it->second;
								}
							}
						}
					}

					// The face is visible

					int8_t shaded_corner[8] = { 0 };

					if (bake_occlusion) {
						// Combinatory solution for
						// https://0fps.net/2013/07/03/ambient-occlusion-for-minecraft-like-worlds/ (inverted)
						//	function vertexAO(side1, side2, corner) {
						//	  if(side1 && side2) {
						//		return 0
						//	  }
						//	  return 3 - (side1 + side2 + corner)
						//	}

						for (unsigned int j = 0; j < 4; ++j) {
							const unsigned int edge = Cube::g_side_edges[side][j];
							const int edge_neighbor_id = type_buffer[voxel_index + edge_neighbor_lut[edge]];
							if (contributes_to_ao(library, edge_neighbor_id)) {
								++shaded_corner[Cube::g_edge_corners[edge][0]];
								++shaded_corner[Cube::g_edge_corners[edge][1]];
							}
						}
						for (unsigned int j = 0; j < 4; ++j) {
							const unsigned int corner = Cube::g_side_corners[side][j];
							if (shaded_corner[corner] == 2) {
								shaded_corner[corner] = 3;
							} else {
								const int corner_neigbor_id = type_buffer[voxel_index + corner_neighbor_lut[corner]];
								if (contributes_to_ao(library, corner_neigbor_id)) {
									++shaded_corner[corner];
								}
							}
						}
					}

					// Subtracting 1 because the data is padded
					const Vector3f pos(x - 1, y - 1, z - 1);

					// TODO Move this into a function
					for (unsigned int surface_index = 0; surface_index < model_surface_count; ++surface_index) {
						const VoxelBlockyModel::Surface &surface = model_surfaces[surface_index];

						VoxelMesherBlocky::Arrays &arrays = out_arrays_per_material[surface.material_id];

						ZN_ASSERT(surface.material_id >= 0 && surface.material_id < index_offsets.size());
						int &index_offset = index_offsets[surface.material_id];

						const VoxelBlockyModel::SideSurface &side_surface = (*side_surfaces)[surface_index];

						const StdVector<Vector3f> &side_positions = side_surface.positions;
						const unsigned int vertex_count = side_surface.positions.size();

						const StdVector<Vector2f> &side_uvs = side_surface.uvs;
						const StdVector<float> &side_tangents = side_surface.tangents;

						// Append vertices of the faces in one go, don't use push_back

						{
							const int append_index = arrays.positions.size();
							arrays.positions.resize(arrays.positions.size() + vertex_count);
							Vector3f *w = arrays.positions.data() + append_index;
							for (unsigned int i = 0; i < vertex_count; ++i) {
								w[i] = side_positions[i] + pos;
							}
						}

						{
							const int append_index = arrays.uvs.size();
							arrays.uvs.resize(arrays.uvs.size() + vertex_count);
							memcpy(arrays.uvs.data() + append_index, side_uvs.data(), vertex_count * sizeof(Vector2f));
						}

						if (side_tangents.size() > 0) {
							const int append_index = arrays.tangents.size();
							arrays.tangents.resize(arrays.tangents.size() + vertex_count * 4);
							memcpy(arrays.tangents.data() + append_index,
								   side_tangents.data(),
								   (vertex_count * 4) * sizeof(float));
						}

						{
							const int append_index = arrays.normals.size();
							arrays.normals.resize(arrays.normals.size() + vertex_count);
							Vector3f *w = arrays.normals.data() + append_index;
							for (unsigned int i = 0; i < vertex_count; ++i) {
								w[i] = to_vec3f(Cube::g_side_normals[side]);
							}
						}

						{
							const int append_index = arrays.colors.size();
							arrays.colors.resize(arrays.colors.size() + vertex_count);
							Color *w = arrays.colors.data() + append_index;
							const Color modulate_color = voxel.color;

							if (bake_occlusion) {
								for (unsigned int i = 0; i < vertex_count; ++i) {
									const Vector3f vertex_pos = side_positions[i];

									// General purpose occlusion colouring.
									// TODO Optimize for cubes
									// TODO Fix occlusion inconsistency caused by triangles orientation? Not sure if
									// worth it
									float shade = 0;
									for (unsigned int j = 0; j < 4; ++j) {
										unsigned int corner = Cube::g_side_corners[side][j];
										if (shaded_corner[corner] != 0) {
											float s = baked_occlusion_darkness *
													static_cast<float>(shaded_corner[corner]);
											// float k = 1.f - Cube::g_corner_position[corner].distance_to(v);
											float k = 1.f -
													math::distance_squared(Cube::g_corner_position[corner], vertex_pos);
											if (k < 0.0) {
												k = 0.0;
											}
											s *= k;
											if (s > shade) {
												shade = s;
											}
										}
									}
									const float gs = 1.0 - shade;
									w[i] = Color(gs, gs, gs) * modulate_color;
								}

							} else {
								for (unsigned int i = 0; i < vertex_count; ++i) {
									w[i] = modulate_color;
								}
							}
						}

						const StdVector<int> &side_indices = side_surface.indices;
						const unsigned int index_count = side_indices.size();

						{
							int i = arrays.indices.size();
							arrays.indices.resize(arrays.indices.size() + index_count);
							int *w = arrays.indices.data();
							for (unsigned int j = 0; j < index_count; ++j) {
								w[i++] = index_offset + side_indices[j];
							}
						}

						if (collision_surface != nullptr && surface.collision_enabled) {
							StdVector<Vector3f> &dst_positions = collision_surface->positions;
							StdVector<int> &dst_indices = collision_surface->indices;

							{
								const unsigned int append_index = dst_positions.size();
								dst_positions.resize(dst_positions.size() + vertex_count);
								Vector3f *w = dst_positions.data() + append_index;
								for (unsigned int i = 0; i < vertex_count; ++i) {
									w[i] = side_positions[i] + pos;
								}
							}

							{
								int i = dst_indices.size();
								dst_indices.resize(dst_indices.size() + index_count);
								int *w = dst_indices.data();
								for (unsigned int j = 0; j < index_count; ++j) {
									w[i++] = collision_surface_index_offset + side_indices[j];
								}
							}

							collision_surface_index_offset += vertex_count;
						}

						index_offset += vertex_count;
					}
				}

				// Inside
				for (unsigned int surface_index = 0; surface_index < model_surface_count; ++surface_index) {
					const VoxelBlockyModel::Surface &surface = model_surfaces[surface_index];
					if (surface.positions.size() == 0) {
						continue;
					}
					// TODO Get rid of push_backs

					VoxelMesherBlocky::Arrays &arrays = out_arrays_per_material[surface.material_id];

					ZN_ASSERT(surface.material_id >= 0 && surface.material_id < index_offsets.size());
					int &index_offset = index_offsets[surface.material_id];

					const StdVector<Vector3f> &positions = surface.positions;
					const unsigned int vertex_count = positions.size();
					const Color modulate_color = voxel.color;

					const StdVector<Vector3f> &normals = surface.normals;
					const StdVector<Vector2f> &uvs = surface.uvs;
					const StdVector<float> &tangents = surface.tangents;

					const Vector3f pos(x - 1, y - 1, z - 1);

					if (tangents.size() > 0) {
						const int append_index = arrays.tangents.size();
						arrays.tangents.resize(arrays.tangents.size() + vertex_count * 4);
						memcpy(arrays.tangents.data() + append_index,
							   tangents.data(),
							   (vertex_count * 4) * sizeof(float));
					}

					for (unsigned int i = 0; i < vertex_count; ++i) {
						arrays.normals.push_back(normals[i]);
						arrays.uvs.push_back(uvs[i]);
						arrays.positions.push_back(positions[i] + pos);
						// TODO handle ambient occlusion on inner parts
						arrays.colors.push_back(modulate_color);
					}

					const StdVector<int> &indices = surface.indices;
					const unsigned int index_count = indices.size();

					for (unsigned int i = 0; i < index_count; ++i) {
						arrays.indices.push_back(index_offset + indices[i]);
					}

					if (collision_surface != nullptr && surface.collision_enabled) {
						StdVector<Vector3f> &dst_positions = collision_surface->positions;
						StdVector<int> &dst_indices = collision_surface->indices;

						for (unsigned int i = 0; i < vertex_count; ++i) {
							dst_positions.push_back(positions[i] + pos);
						}
						for (unsigned int i = 0; i < index_count; ++i) {
							dst_indices.push_back(collision_surface_index_offset + indices[i]);
						}

						collision_surface_index_offset += vertex_count;
					}

					index_offset += vertex_count;
				}
			}
		}
	}
}

Vector3f side_to_block_coordinates(const Vector3f v, const VoxelBlockyModel::Side side) {
	switch (side) {
		case VoxelBlockyModel::SIDE_NEGATIVE_X:
		case VoxelBlockyModel::SIDE_POSITIVE_X:
			return v.zyx();
		case VoxelBlockyModel::SIDE_NEGATIVE_Y:
		case VoxelBlockyModel::SIDE_POSITIVE_Y:
			// return v.zxy();
			return v.yzx();
		case VoxelBlockyModel::SIDE_NEGATIVE_Z:
		case VoxelBlockyModel::SIDE_POSITIVE_Z:
			return v;
		default:
			ZN_CRASH();
			return v;
	}
}

int get_side_sign(const VoxelBlockyModel::Side side) {
	switch (side) {
		case VoxelBlockyModel::SIDE_NEGATIVE_X:
		case VoxelBlockyModel::SIDE_NEGATIVE_Y:
		case VoxelBlockyModel::SIDE_NEGATIVE_Z:
			return -1;
		case VoxelBlockyModel::SIDE_POSITIVE_X:
		case VoxelBlockyModel::SIDE_POSITIVE_Y:
		case VoxelBlockyModel::SIDE_POSITIVE_Z:
			return 1;
		default:
			ZN_CRASH();
			return 1;
	}
}

// Adds extra voxel side geometry on the sides of the chunk for every voxel exposed to air. This creates
// "seams" that hide LOD cracks when meshes of different LOD are put next to each other.
// This method doesn't require to access voxels of the child LOD. The downside is that it won't always hide all the
// cracks, but the assumption is that it will do most of the time.
// AO is not handled, and probably doesn't need to be
template <typename TModelID>
void append_side_seams(
		Span<const TModelID> buffer,
		const Vector3T<int> jump,
		const int z, // Coordinate of the first or last voxel (not within the padded region)
		const int size_x,
		const int size_y,
		const VoxelBlockyModel::Side side,
		const VoxelBlockyLibraryBase::BakedData &library,
		StdVector<VoxelMesherBlocky::Arrays> &out_arrays_per_material
) {
	constexpr TModelID AIR = 0;
	constexpr int pad = 1;

	const int z_base = z * jump.z;
	const int side_sign = get_side_sign(side);

	// Buffers sent to chunk meshing have outer and inner voxels.
	// Inner voxels are those that are actually being meshed.
	// Outer voxels are not made part of the final mesh, but they exist to know how to occlude sides of inner voxels
	// touching them.

	// For each outer voxel on the side of the chunk (using side-relative coordinates)
	for (int x = pad; x < size_x - pad; ++x) {
		for (int y = pad; y < size_y - pad; ++y) {
			const int buffer_index = x * jump.x + y * jump.y + z_base;
			const int v = buffer[buffer_index];

			if (v == AIR) {
				continue;
			}

			// Check if the voxel is exposed to air

			const int nv0 = buffer[buffer_index - jump.x];
			const int nv1 = buffer[buffer_index + jump.x];
			const int nv2 = buffer[buffer_index - jump.y];
			const int nv3 = buffer[buffer_index + jump.y];

			if (nv0 != AIR && nv1 != AIR && nv2 != AIR && nv3 != AIR) {
				continue;
			}

			// Check if the outer voxel occludes an inner voxel
			// (this check is not actually accurate, maybe we'd have to do a full occlusion check using the library?)

			const int nv4 = buffer[buffer_index - side_sign * jump.z];
			if (nv4 == AIR) {
				continue;
			}

			// If it does, add geometry for the side of that inner voxel

			const Vector3f pos = side_to_block_coordinates(Vector3f(x - pad, y - pad, z - (side_sign + 1)), side);

			const VoxelBlockyModel::BakedData &voxel_baked_data = library.models[nv4];
			const VoxelBlockyModel::BakedData::Model &model = voxel_baked_data.model;

			const FixedArray<VoxelBlockyModel::SideSurface, VoxelBlockyModel::MAX_SURFACES> &side_surfaces =
					model.sides_surfaces[side];

			for (unsigned int surface_index = 0; surface_index < model.surface_count; ++surface_index) {
				const VoxelBlockyModel::Surface &surface = model.surfaces[surface_index];
				VoxelMesherBlocky::Arrays &arrays = out_arrays_per_material[surface.material_id];

				const VoxelBlockyModel::SideSurface &side_surface = side_surfaces[side];
				const unsigned int vertex_count = side_surface.positions.size();

				// TODO The following code is pretty much the same as the main meshing function.
				// We should put it in common once blocky mesher features are merged (blocky fluids, shadows occluders).
				// The baked occlusion part should be separated to run on top of color modulate.
				// Index offsets might not need a vector after all.

				const unsigned int index_offset = arrays.positions.size();

				{
					const unsigned int append_index = arrays.positions.size();
					arrays.positions.resize(arrays.positions.size() + vertex_count);
					Vector3f *w = arrays.positions.data() + append_index;
					for (unsigned int i = 0; i < vertex_count; ++i) {
						w[i] = side_surface.positions[i] + pos;
					}
				}

				{
					const unsigned int append_index = arrays.uvs.size();
					arrays.uvs.resize(arrays.uvs.size() + vertex_count);
					memcpy(arrays.uvs.data() + append_index, side_surface.uvs.data(), vertex_count * sizeof(Vector2f));
				}

				if (side_surface.tangents.size() > 0) {
					const unsigned int append_index = arrays.tangents.size();
					arrays.tangents.resize(arrays.tangents.size() + vertex_count * 4);
					memcpy(arrays.tangents.data() + append_index,
						   side_surface.tangents.data(),
						   (vertex_count * 4) * sizeof(float));
				}

				{
					const int append_index = arrays.normals.size();
					arrays.normals.resize(arrays.normals.size() + vertex_count);
					Vector3f *w = arrays.normals.data() + append_index;
					for (unsigned int i = 0; i < vertex_count; ++i) {
						w[i] = to_vec3f(Cube::g_side_normals[side]);
					}
				}

				{
					const int append_index = arrays.colors.size();
					arrays.colors.resize(arrays.colors.size() + vertex_count);
					Color *w = arrays.colors.data() + append_index;
					for (unsigned int i = 0; i < vertex_count; ++i) {
						w[i] = voxel_baked_data.color;
					}
				}

				{
					const unsigned int index_count = side_surface.indices.size();
					unsigned int i = arrays.indices.size();
					arrays.indices.resize(arrays.indices.size() + index_count);
					int *w = arrays.indices.data();
					for (unsigned int j = 0; j < index_count; ++j) {
						w[i++] = index_offset + side_surface.indices[j];
					}
				}
			}
		}
	}
}

template <typename TModelID>
void append_seams(
		Span<const TModelID> buffer,
		const Vector3i size,
		StdVector<VoxelMesherBlocky::Arrays> &out_arrays_per_material,
		const VoxelBlockyLibraryBase::BakedData &library
) {
	ZN_PROFILE_SCOPE();

	const Vector3T<int> jump(size.y, 1, size.x * size.y);

	// Shortcuts
	StdVector<VoxelMesherBlocky::Arrays> &out = out_arrays_per_material;
	constexpr VoxelBlockyModel::Side NEGATIVE_X = VoxelBlockyModel::SIDE_NEGATIVE_X;
	constexpr VoxelBlockyModel::Side POSITIVE_X = VoxelBlockyModel::SIDE_POSITIVE_X;
	constexpr VoxelBlockyModel::Side NEGATIVE_Y = VoxelBlockyModel::SIDE_NEGATIVE_Y;
	constexpr VoxelBlockyModel::Side POSITIVE_Y = VoxelBlockyModel::SIDE_POSITIVE_Y;
	constexpr VoxelBlockyModel::Side NEGATIVE_Z = VoxelBlockyModel::SIDE_NEGATIVE_Z;
	constexpr VoxelBlockyModel::Side POSITIVE_Z = VoxelBlockyModel::SIDE_POSITIVE_Z;

	append_side_seams(buffer, jump.xyz(), 0, size.x, size.y, NEGATIVE_Z, library, out);
	append_side_seams(buffer, jump.xyz(), (size.z - 1), size.x, size.y, POSITIVE_Z, library, out);
	append_side_seams(buffer, jump.zyx(), 0, size.z, size.y, NEGATIVE_X, library, out);
	append_side_seams(buffer, jump.zyx(), (size.x - 1), size.z, size.y, POSITIVE_X, library, out);
	append_side_seams(buffer, jump.zxy(), 0, size.z, size.x, NEGATIVE_Y, library, out);
	append_side_seams(buffer, jump.zxy(), (size.y - 1), size.z, size.x, POSITIVE_Y, library, out);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

VoxelMesherBlocky::VoxelMesherBlocky() {
	set_padding(PADDING, PADDING);
}

VoxelMesherBlocky::~VoxelMesherBlocky() {}

VoxelMesherBlocky::Cache &VoxelMesherBlocky::get_tls_cache() {
	thread_local Cache cache;
	return cache;
}

void VoxelMesherBlocky::set_library(Ref<VoxelBlockyLibraryBase> library) {
	RWLockWrite wlock(_parameters_lock);
	_parameters.library = library;
}

Ref<VoxelBlockyLibraryBase> VoxelMesherBlocky::get_library() const {
	RWLockRead rlock(_parameters_lock);
	return _parameters.library;
}

void VoxelMesherBlocky::set_occlusion_darkness(float darkness) {
	RWLockWrite wlock(_parameters_lock);
	_parameters.baked_occlusion_darkness = math::clamp(darkness, 0.0f, 1.0f);
}

float VoxelMesherBlocky::get_occlusion_darkness() const {
	RWLockRead rlock(_parameters_lock);
	return _parameters.baked_occlusion_darkness;
}

void VoxelMesherBlocky::set_occlusion_enabled(bool enable) {
	RWLockWrite wlock(_parameters_lock);
	_parameters.bake_occlusion = enable;
}

bool VoxelMesherBlocky::get_occlusion_enabled() const {
	RWLockRead rlock(_parameters_lock);
	return _parameters.bake_occlusion;
}

void VoxelMesherBlocky::build(VoxelMesher::Output &output, const VoxelMesher::Input &input) {
	const VoxelBuffer::ChannelId channel = VoxelBuffer::CHANNEL_TYPE;
	Parameters params;
	{
		RWLockRead rlock(_parameters_lock);
		params = _parameters;
	}

	if (params.library.is_null()) {
		// This may be a configuration warning, the mesh will be left empty.
		// If it was an error it would spam unnecessarily in the editor as users set things up.
		return;
	}
	// ERR_FAIL_COND(params.library.is_null());

	Cache &cache = get_tls_cache();

	StdVector<Arrays> &arrays_per_material = cache.arrays_per_material;
	for (unsigned int i = 0; i < arrays_per_material.size(); ++i) {
		Arrays &a = arrays_per_material[i];
		a.clear();
	}

	float baked_occlusion_darkness = 0;
	if (params.bake_occlusion) {
		baked_occlusion_darkness = params.baked_occlusion_darkness / 3.0f;
	}

	// The technique is Culled faces.
	// Could be improved with greedy meshing: https://0fps.net/2012/06/30/meshing-in-a-minecraft-game/
	// However I don't feel it's worth it yet:
	// - Not so much gain for organic worlds with lots of texture variations
	// - Works well with cubes but not with any shape
	// - Slower
	// => Could be implemented in a separate class?

	const VoxelBuffer &voxels = input.voxels;

	// Iterate 3D padded data to extract voxel faces.
	// This is the most intensive job in this class, so all required data should be as fit as possible.

	// The buffer we receive MUST be dense (i.e not compressed, and channels allocated).
	// That means we can use raw pointers to voxel data inside instead of using the higher-level getters,
	// and then save a lot of time.

	if (voxels.get_channel_compression(channel) == VoxelBuffer::COMPRESSION_UNIFORM) {
		// All voxels have the same type.
		// If it's all air, nothing to do. If it's all cubes, nothing to do either.
		// TODO Handle edge case of uniform block with non-cubic voxels!
		// If the type of voxel still produces geometry in this situation (which is an absurd use case but not an
		// error), decompress into a backing array to still allow the use of the same algorithm.
		return;

	} else if (voxels.get_channel_compression(channel) != VoxelBuffer::COMPRESSION_NONE) {
		// No other form of compression is allowed
		ERR_PRINT("VoxelMesherBlocky received unsupported voxel compression");
		return;
	}

	Span<const uint8_t> raw_channel;
	if (!voxels.get_channel_as_bytes_read_only(channel, raw_channel)) {
		// Case supposedly handled before...
		ERR_PRINT("Something wrong happened");
		return;
	}

	const Vector3i block_size = voxels.get_size();
	const VoxelBuffer::Depth channel_depth = voxels.get_channel_depth(channel);

	VoxelMesher::Output::CollisionSurface *collision_surface = nullptr;
	if (input.collision_hint) {
		collision_surface = &output.collision_surface;
	}

	unsigned int material_count = 0;
	{
		// We can only access baked data. Only this data is made for multithreaded access.
		RWLockRead lock(params.library->get_baked_data_rw_lock());
		const VoxelBlockyLibraryBase::BakedData &library_baked_data = params.library->get_baked_data();

		material_count = library_baked_data.indexed_materials_count;

		if (arrays_per_material.size() < material_count) {
			arrays_per_material.resize(material_count);
		}

		switch (channel_depth) {
			case VoxelBuffer::DEPTH_8_BIT:
				generate_blocky_mesh( //
						arrays_per_material, //
						collision_surface, //
						raw_channel, //
						block_size, //
						library_baked_data, //
						params.bake_occlusion, //
						baked_occlusion_darkness //
				);
				if (input.lod_index > 0) {
					append_seams(raw_channel, block_size, arrays_per_material, library_baked_data);
				}
				break;

			case VoxelBuffer::DEPTH_16_BIT: {
				Span<const uint16_t> model_ids = raw_channel.reinterpret_cast_to<const uint16_t>();
				generate_blocky_mesh(
						arrays_per_material,
						collision_surface,
						model_ids,
						block_size,
						library_baked_data,
						params.bake_occlusion,
						baked_occlusion_darkness
				);
				if (input.lod_index > 0) {
					append_seams(model_ids, block_size, arrays_per_material, library_baked_data);
				}
			} break;

			default:
				ERR_PRINT("Unsupported voxel depth");
				return;
		}
	}

	if (input.lod_index > 0) {
		// Might not look good, but at least it's something
		const float lod_scale = 1 << input.lod_index;
		for (Arrays &arrays : arrays_per_material) {
			for (Vector3f &p : arrays.positions) {
				p = p * lod_scale;
			}
		}
		if (collision_surface != nullptr) {
			for (Vector3f &p : collision_surface->positions) {
				p = p * lod_scale;
			}
		}
	}

	// TODO Optimization: we could return a single byte array and use Mesh::add_surface down the line?
	// That API does not seem to exist yet though.

	for (unsigned int material_index = 0; material_index < material_count; ++material_index) {
		const Arrays &arrays = arrays_per_material[material_index];

		if (arrays.positions.size() != 0) {
			Array mesh_arrays;
			mesh_arrays.resize(Mesh::ARRAY_MAX);

			{
				PackedVector3Array positions;
				PackedVector2Array uvs;
				PackedVector3Array normals;
				PackedColorArray colors;
				PackedInt32Array indices;

				copy_to(positions, arrays.positions);
				copy_to(uvs, arrays.uvs);
				copy_to(normals, arrays.normals);
				copy_to(colors, arrays.colors);
				copy_to(indices, arrays.indices);

				mesh_arrays[Mesh::ARRAY_VERTEX] = positions;
				mesh_arrays[Mesh::ARRAY_TEX_UV] = uvs;
				mesh_arrays[Mesh::ARRAY_NORMAL] = normals;
				mesh_arrays[Mesh::ARRAY_COLOR] = colors;
				mesh_arrays[Mesh::ARRAY_INDEX] = indices;

				if (arrays.tangents.size() > 0) {
					PackedFloat32Array tangents;
					copy_to(tangents, arrays.tangents);
					mesh_arrays[Mesh::ARRAY_TANGENT] = tangents;
				}
			}

			output.surfaces.push_back(Output::Surface());
			Output::Surface &surface = output.surfaces.back();
			surface.arrays = mesh_arrays;
			surface.material_index = material_index;
		}
		//  else {
		// 	// Empty
		// 	output.surfaces.push_back(Output::Surface());
		// }
	}

	output.primitive_type = Mesh::PRIMITIVE_TRIANGLES;
}

Ref<Resource> VoxelMesherBlocky::duplicate(bool p_subresources) const {
	Parameters params;
	{
		RWLockRead rlock(_parameters_lock);
		params = _parameters;
	}

	if (p_subresources && params.library.is_valid()) {
		params.library = params.library->duplicate(true);
	}

	Ref<VoxelMesherBlocky> c;
	c.instantiate();
	c->_parameters = params;
	return c;
}

int VoxelMesherBlocky::get_used_channels_mask() const {
	return (1 << VoxelBuffer::CHANNEL_TYPE);
}

Ref<Material> VoxelMesherBlocky::get_material_by_index(unsigned int index) const {
	Ref<VoxelBlockyLibraryBase> lib = get_library();
	if (lib.is_null()) {
		return Ref<Material>();
	}
	return lib->get_material_by_index(index);
}

unsigned int VoxelMesherBlocky::get_material_index_count() const {
	Ref<VoxelBlockyLibraryBase> lib = get_library();
	if (lib.is_null()) {
		return 0;
	}
	return lib->get_material_index_count();
}

#ifdef TOOLS_ENABLED

void VoxelMesherBlocky::get_configuration_warnings(PackedStringArray &out_warnings) const {
	Ref<VoxelBlockyLibraryBase> library = get_library();

	if (library.is_null()) {
		out_warnings.append(String(ZN_TTR("{0} has no {1} assigned."))
									.format(
											varray(VoxelMesherBlocky::get_class_static(),
												   VoxelBlockyLibraryBase::get_class_static())
									));
		return;
	}

	const VoxelBlockyLibraryBase::BakedData &baked_data = library->get_baked_data();
	RWLockRead rlock(library->get_baked_data_rw_lock());

	if (baked_data.models.size() == 0) {
		out_warnings.append(String(ZN_TTR("The {0} assigned to {1} has no baked models."))
									.format(varray(library->get_class(), VoxelMesherBlocky::get_class_static())));
		return;
	}

	library->get_configuration_warnings(out_warnings);
}

#endif // TOOLS_ENABLED

void VoxelMesherBlocky::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_library", "voxel_library"), &VoxelMesherBlocky::set_library);
	ClassDB::bind_method(D_METHOD("get_library"), &VoxelMesherBlocky::get_library);

	ClassDB::bind_method(D_METHOD("set_occlusion_enabled", "enable"), &VoxelMesherBlocky::set_occlusion_enabled);
	ClassDB::bind_method(D_METHOD("get_occlusion_enabled"), &VoxelMesherBlocky::get_occlusion_enabled);

	ClassDB::bind_method(D_METHOD("set_occlusion_darkness", "value"), &VoxelMesherBlocky::set_occlusion_darkness);
	ClassDB::bind_method(D_METHOD("get_occlusion_darkness"), &VoxelMesherBlocky::get_occlusion_darkness);

	ADD_PROPERTY(
			PropertyInfo(
					Variant::OBJECT,
					"library",
					PROPERTY_HINT_RESOURCE_TYPE,
					VoxelBlockyLibraryBase::get_class_static(),
					PROPERTY_USAGE_DEFAULT
					// Sadly we can't use this hint because the property type is abstract... can't just choose a
					// default child class. This hint becomes less and less useful everytime I come across it...
					//| PROPERTY_USAGE_EDITOR_INSTANTIATE_OBJECT
			),
			"set_library",
			"get_library"
	);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "occlusion_enabled"), "set_occlusion_enabled", "get_occlusion_enabled");
	ADD_PROPERTY(
			PropertyInfo(Variant::FLOAT, "occlusion_darkness", PROPERTY_HINT_RANGE, "0,1,0.01"),
			"set_occlusion_darkness",
			"get_occlusion_darkness"
	);
}

} // namespace zylann::voxel
