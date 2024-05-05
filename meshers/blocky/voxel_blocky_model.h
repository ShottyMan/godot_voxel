#ifndef VOXEL_BLOCKY_MODEL_H
#define VOXEL_BLOCKY_MODEL_H

#include "../../constants/cube_tables.h"
#include "../../util/containers/fixed_array.h"
#include "../../util/containers/std_vector.h"
#include "../../util/godot/classes/material.h"
#include "../../util/godot/classes/mesh.h"
#include "../../util/macros.h"
#include "../../util/math/ortho_basis.h"
#include "../../util/math/vector2f.h"
#include "../../util/math/vector3f.h"

namespace zylann::voxel {

namespace blocky {
struct ModelBakingContext;
}

// TODO Add preview in inspector showing collision boxes

// Visuals and collisions corresponding to a specific voxel value/state, for use with `VoxelMesherBlocky`.
// A voxel can be a simple coloured cube, or a more complex model.
class VoxelBlockyModel : public Resource {
	GDCLASS(VoxelBlockyModel, Resource)

public:
	// Convention to mean "nothing".
	// Don't assign a non-empty model at this index.
	static const uint16_t AIR_ID = 0;
	static const uint8_t NULL_FLUID_INDEX = 255;
	static constexpr uint32_t MAX_SURFACES = 2;

	struct SideSurface {
		StdVector<Vector3f> positions;
		StdVector<Vector2f> uvs;
		StdVector<int> indices;
		StdVector<float> tangents;
		// Normals aren't stored because they are assumed to be the same for the whole side

		void clear() {
			positions.clear();
			uvs.clear();
			indices.clear();
			tangents.clear();
		}
	};

	struct Surface {
		// Inside part of the model.
		StdVector<Vector3f> positions;
		StdVector<Vector3f> normals;
		StdVector<Vector2f> uvs;
		StdVector<int> indices;
		StdVector<float> tangents;

		uint32_t material_id = 0;
		bool collision_enabled = true;

		void clear() {
			positions.clear();
			normals.clear();
			uvs.clear();
			indices.clear();
			tangents.clear();
		}
	};

	// Plain data strictly used by the mesher.
	// It becomes distinct because it's going to be used in a multithread environment,
	// while the configuration that produced the data can be changed by the user at any time.
	// Also, it is lighter than Godot resources.
	struct BakedData {
		struct Model {
			// A model can have up to 2 materials.
			// If more is needed or profiling tells better, we could change it to a vector?
			FixedArray<Surface, MAX_SURFACES> surfaces;
			// Model sides: they are separated because this way we can occlude them easily.
			FixedArray<FixedArray<SideSurface, MAX_SURFACES>, Cube::SIDE_COUNT> sides_surfaces;
			unsigned int surface_count = 0;
			// Cached information to check this case early
			uint8_t empty_sides_mask = 0;

			// Tells what is the "shape" of each side in order to cull them quickly when in contact with neighbors.
			// Side patterns are still determined based on a combination of all surfaces.
			FixedArray<uint32_t, Cube::SIDE_COUNT> side_pattern_indices;
			// Side culling is all or nothing.
			// If we want to support partial culling with baked models (needed if you do fluids with "staircase"
			// models), we would need another lookup table that given two side patterns, outputs alternate geometry data
			// that is pre-cut. This would require a lot more data and precomputations though, and the cases in
			// which this is needed could make use of different approaches such as procedural generation of the
			// geometry.

			// [side][neighbor_shape_id] => pre-cut SideSurfaces
			// Surface to attempt using when a side passes the visibility test and cutout is enabled.
			// If the SideSurface from this container is empty or not found, fallback on full surface
			FixedArray<std::unordered_map<uint32_t, FixedArray<SideSurface, MAX_SURFACES>>, Cube::SIDE_COUNT>
					cutout_side_surfaces;
			// TODO ^ Make it UniquePtr? That array takes space for what is essentially a niche feature

			void clear() {
				for (Surface &surface : surfaces) {
					surface.clear();
				}
				for (FixedArray<SideSurface, MAX_SURFACES> &side_surfaces : sides_surfaces) {
					for (SideSurface &side_surface : side_surfaces) {
						side_surface.clear();
					}
				}
			}
		};

		Model model;
		Color color;
		uint8_t transparency_index;
		bool culls_neighbors;
		bool contributes_to_ao;
		bool empty = true;
		bool is_random_tickable;
		bool is_transparent;
		bool cutout_sides_enabled = false;
		uint8_t fluid_index = NULL_FLUID_INDEX;
		uint8_t fluid_level;

		uint32_t box_collision_mask;
		StdVector<AABB> box_collision_aabbs;

		inline void clear() {
			model.clear();
			empty = true;
		}
	};

	VoxelBlockyModel();

	enum Side {
		SIDE_NEGATIVE_X = Cube::SIDE_NEGATIVE_X,
		SIDE_POSITIVE_X = Cube::SIDE_POSITIVE_X,
		SIDE_NEGATIVE_Y = Cube::SIDE_NEGATIVE_Y,
		SIDE_POSITIVE_Y = Cube::SIDE_POSITIVE_Y,
		SIDE_NEGATIVE_Z = Cube::SIDE_NEGATIVE_Z,
		SIDE_POSITIVE_Z = Cube::SIDE_POSITIVE_Z,
		SIDE_COUNT = Cube::SIDE_COUNT
	};

	// Properties

	void set_color(Color color);
	_FORCE_INLINE_ Color get_color() const {
		return _color;
	}

	void set_material_override(int index, Ref<Material> material);
	Ref<Material> get_material_override(int index) const;

	void set_mesh_collision_enabled(int surface_index, bool enabled);
	bool is_mesh_collision_enabled(int surface_index) const;

	// TODO Might become obsoleted by transparency index
	void set_transparent(bool t = true);
	_FORCE_INLINE_ bool is_transparent() const {
		return _transparency_index != 0;
	}

	void set_transparency_index(int i);
	int get_transparency_index() const {
		return _transparency_index;
	}

	void set_culls_neighbors(bool cn);
	bool get_culls_neighbors() const {
		return _culls_neighbors;
	}

	void set_collision_mask(uint32_t mask);
	inline uint32_t get_collision_mask() const {
		return _collision_mask;
	}

	unsigned int get_collision_aabb_count() const;
	void set_collision_aabb(unsigned int i, AABB aabb);
	void set_collision_aabbs(Span<const AABB> aabbs);

	void set_random_tickable(bool rt);
	bool is_random_tickable() const;

#ifdef TOOLS_ENABLED
	virtual void get_configuration_warnings(PackedStringArray &out_warnings) const;
#endif

	//------------------------------------------
	// Properties for internal usage only

	virtual bool is_empty() const;

	virtual void bake(blocky::ModelBakingContext &ctx) const;

	Span<const AABB> get_collision_aabbs() const {
		return to_span(_collision_aabbs);
	}

	struct LegacyProperties {
		enum GeometryType { GEOMETRY_NONE, GEOMETRY_CUBE, GEOMETRY_MESH };

		bool found = false;
		FixedArray<Vector2f, Cube::SIDE_COUNT> cube_tiles;
		GeometryType geometry_type = GEOMETRY_NONE;
		StringName name;
		int id = -1;
		Ref<Mesh> custom_mesh;
	};

	const LegacyProperties &get_legacy_properties() const {
		return _legacy_properties;
	}

	void copy_base_properties_from(const VoxelBlockyModel &src);

	virtual Ref<Mesh> get_preview_mesh() const;

	virtual void rotate_90(math::Axis axis, bool clockwise);
	virtual void rotate_ortho(math::OrthoBasis ortho_basis);

	static Ref<Mesh> make_mesh_from_baked_data(const BakedData &baked_data, bool tangents_enabled);

	static Ref<Mesh> make_mesh_from_baked_data(
			Span<const Surface> inner_surfaces,
			Span<const FixedArray<SideSurface, MAX_SURFACES>> sides_surfaces,
			const Color model_color,
			const bool tangents_enabled
	);

protected:
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;

	void set_surface_count(unsigned int new_count);

	void rotate_collision_boxes_90(math::Axis axis, bool clockwise);
	void rotate_collision_boxes_ortho(math::OrthoBasis ortho_basis);

private:
	static void _bind_methods();

	TypedArray<AABB> _b_get_collision_aabbs() const;
	void _b_set_collision_aabbs(TypedArray<AABB> array);
	void _b_rotate_90(Vector3i::Axis axis, bool clockwise);

	// Properties

	struct SurfaceParams {
		// If assigned, these materials override those present on the mesh itself.
		Ref<Material> material_override;
		// If true and classic mesh physics are enabled, the surface will be present in the collider.
		bool collision_enabled = true;
	};

	FixedArray<SurfaceParams, MAX_SURFACES> _surface_params;

protected:
	unsigned int _surface_count = 0;

	// Used for AABB physics only, not classic physics
	StdVector<AABB> _collision_aabbs;
	uint32_t _collision_mask = 1;

private:
	// If two neighboring voxels are supposed to occlude their shared face,
	// this index decides wether or not it should happen. Equal indexes culls the face, different indexes doesn't.
	uint8_t _transparency_index = 0;
	// If enabled, this voxel culls the faces of its neighbors. Disabling
	// can be useful for denser transparent voxels, such as foliage.
	bool _culls_neighbors = true;
	bool _random_tickable = false;

	Color _color;

	LegacyProperties _legacy_properties;
};

inline bool is_empty(const FixedArray<VoxelBlockyModel::SideSurface, VoxelBlockyModel::MAX_SURFACES> &surfaces) {
	for (const VoxelBlockyModel::SideSurface &surface : surfaces) {
		if (surface.indices.size() > 0) {
			return false;
		}
	}
	return true;
}

} // namespace zylann::voxel

VARIANT_ENUM_CAST(zylann::voxel::VoxelBlockyModel::Side)

#endif // VOXEL_BLOCKY_MODEL_H
