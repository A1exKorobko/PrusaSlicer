// Include GLGizmoBase.hpp before I18N.hpp as it includes some libigl code, which overrides our localization "L" macro.
#include "GLGizmoMeasure.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectManipulation.hpp"


#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/MeasureUtils.hpp"

#include <imgui/imgui_internal.h>

#include <numeric>

#include <GL/glew.h>

#include <wx/clipbrd.h>

namespace Slic3r {
namespace GUI {

static const Slic3r::ColorRGBA SELECTED_1ST_COLOR   = { 0.25f, 0.75f, 0.75f, 1.0f };
static const Slic3r::ColorRGBA SELECTED_2ND_COLOR   = { 0.75f, 0.25f, 0.75f, 1.0f };

static const int POINT_ID         = 100;
static const int EDGE_ID          = 200;
static const int CIRCLE_ID        = 300;
static const int PLANE_ID         = 400;
static const int SELECTION_1_ID   = 501;
static const int SELECTION_2_ID   = 502;

static const float TRIANGLE_BASE = 10.0f;
static const float TRIANGLE_HEIGHT = TRIANGLE_BASE * 1.618033f;

static const std::string CTRL_STR =
#ifdef __APPLE__
"⌘"
#else
"Ctrl"
#endif //__APPLE__
;

static std::string format_double(double value)
{
    char buf[1024];
    sprintf(buf, "%.3f", value);
    return std::string(buf);
}

static std::string format_vec3(const Vec3d& v)
{
    char buf[1024];
    sprintf(buf, "X: %.3f, Y: %.3f, Z: %.3f", v.x(), v.y(), v.z());
    return std::string(buf);
}

static std::string surface_feature_type_as_string(Measure::SurfaceFeatureType type)
{
    switch (type)
    {
    default:
    case Measure::SurfaceFeatureType::Undef:  { return _u8L("No feature"); }
    case Measure::SurfaceFeatureType::Point:  { return _u8L("Vertex"); }
    case Measure::SurfaceFeatureType::Edge:   { return _u8L("Edge"); }
    case Measure::SurfaceFeatureType::Circle: { return _u8L("Circle"); }
    case Measure::SurfaceFeatureType::Plane:  { return _u8L("Plane"); }
    }
}

static std::string point_on_feature_type_as_string(Measure::SurfaceFeatureType type, int hover_id)
{
    std::string ret;
    switch (type) {
    case Measure::SurfaceFeatureType::Point:  { ret = _u8L("Vertex"); break; }
    case Measure::SurfaceFeatureType::Edge:   { ret = _u8L("Point on edge"); break; }
    case Measure::SurfaceFeatureType::Circle: { ret = _u8L("Point on circle"); break; }
    case Measure::SurfaceFeatureType::Plane:  { ret = _u8L("Point on plane"); break; }
    default:                                  { assert(false); break; }
    }
    return ret;
}

static std::string center_on_feature_type_as_string(Measure::SurfaceFeatureType type)
{
    std::string ret;
    switch (type) {
    case Measure::SurfaceFeatureType::Edge:   { ret = _u8L("Center of edge"); break; }
    case Measure::SurfaceFeatureType::Circle: { ret = _u8L("Center of circle"); break; }
    default:                                  { assert(false); break; }
    }
    return ret;
}

static GLModel::Geometry init_plane_data(const indexed_triangle_set& its, const std::vector<std::vector<int>>& planes_triangles, int idx)
{
    assert(0 <= idx && idx < (int)planes_triangles.size());
    const std::vector<int>& triangle_indices = planes_triangles[idx];

    GLModel::Geometry init_data;
    init_data.format = { GUI::GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    unsigned int i = 0;
    for (int idx : triangle_indices) {
        const Vec3f& v0 = its.vertices[its.indices[idx][0]];
        const Vec3f& v1 = its.vertices[its.indices[idx][1]];
        const Vec3f& v2 = its.vertices[its.indices[idx][2]];

        const Vec3f n = (v1 - v0).cross(v2 - v0).normalized();
        init_data.add_vertex(v0, n);
        init_data.add_vertex(v1, n);
        init_data.add_vertex(v2, n);
        init_data.add_triangle(i, i + 1, i + 2);
        i += 3;
    }

    return init_data;
}

static GLModel::Geometry init_torus_data(unsigned int primary_resolution, unsigned int secondary_resolution, const Vec3f& center,
    float radius, float thickness, const Vec3f& model_axis, const Transform3f& world_trafo)
{
    const unsigned int torus_sector_count = std::max<unsigned int>(4, primary_resolution);
    const unsigned int section_sector_count = std::max<unsigned int>(4, secondary_resolution);
    const float torus_sector_step = 2.0f * float(M_PI) / float(torus_sector_count);
    const float section_sector_step = 2.0f * float(M_PI) / float(section_sector_count);

    GLModel::Geometry data;
    data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    data.reserve_vertices(torus_sector_count * section_sector_count);
    data.reserve_indices(torus_sector_count * section_sector_count * 2 * 3);

    // vertices
    const Transform3f local_to_world_matrix = world_trafo * Geometry::translation_transform(center.cast<double>()).cast<float>() *
        Eigen::Quaternion<float>::FromTwoVectors(Vec3f::UnitZ(), model_axis);
    for (unsigned int i = 0; i < torus_sector_count; ++i) {
        const float section_angle = torus_sector_step * i;
        const Vec3f radius_dir(std::cos(section_angle), std::sin(section_angle), 0.0f);
        const Vec3f local_section_center = radius * radius_dir;
        const Vec3f world_section_center = local_to_world_matrix * local_section_center;
        const Vec3f local_section_normal = local_section_center.normalized().cross(Vec3f::UnitZ()).normalized();
        const Vec3f world_section_normal = (Vec3f)(local_to_world_matrix.matrix().block(0, 0, 3, 3) * local_section_normal).normalized();
        const Vec3f base_v = thickness * radius_dir;
        for (unsigned int j = 0; j < section_sector_count; ++j) {
            const Vec3f v = Eigen::AngleAxisf(section_sector_step * j, world_section_normal) * base_v;
            data.add_vertex(world_section_center + v, (Vec3f)v.normalized());
        }
    }

    // triangles
    for (unsigned int i = 0; i < torus_sector_count; ++i) {
        const unsigned int ii = i * section_sector_count;
        const unsigned int ii_next = ((i + 1) % torus_sector_count) * section_sector_count;
        for (unsigned int j = 0; j < section_sector_count; ++j) {
            const unsigned int j_next = (j + 1) % section_sector_count;
            const unsigned int i0 = ii + j;
            const unsigned int i1 = ii_next + j;
            const unsigned int i2 = ii_next + j_next;
            const unsigned int i3 = ii + j_next;
            data.add_triangle(i0, i1, i2);
            data.add_triangle(i0, i2, i3);
        }
    }

    return data;
}

class TransformHelper
{
    struct Cache
    {
        std::array<int, 4> viewport;
        Matrix4d ndc_to_ss_matrix;
        Transform3d ndc_to_ss_matrix_inverse;
    };

    static Cache s_cache;

public:
    static Vec3d model_to_world(const Vec3d& model, const Transform3d& world_matrix) {
        return world_matrix * model;
    }

    static Vec4d world_to_clip(const Vec3d& world, const Matrix4d& projection_view_matrix) {
        return projection_view_matrix * Vec4d(world.x(), world.y(), world.z(), 1.0);
    }

    static Vec3d clip_to_ndc(const Vec4d& clip) {
        return Vec3d(clip.x(), clip.y(), clip.z()) / clip.w();
    }

    static Vec2d ndc_to_ss(const Vec3d& ndc, const std::array<int, 4>& viewport) {
        const double half_w = 0.5 * double(viewport[2]);
        const double half_h = 0.5 * double(viewport[3]);
        return { half_w * ndc.x() + double(viewport[0]) + half_w, half_h * ndc.y() + double(viewport[1]) + half_h };
    };

    static Vec4d model_to_clip(const Vec3d& model, const Transform3d& world_matrix, const Matrix4d& projection_view_matrix) {
        return world_to_clip(model_to_world(model, world_matrix), projection_view_matrix);
    }

    static Vec3d model_to_ndc(const Vec3d& model, const Transform3d& world_matrix, const Matrix4d& projection_view_matrix) {
        return clip_to_ndc(world_to_clip(model_to_world(model, world_matrix), projection_view_matrix));
    }

    static Vec2d model_to_ss(const Vec3d& model, const Transform3d& world_matrix, const Matrix4d& projection_view_matrix, const std::array<int, 4>& viewport) {
        return ndc_to_ss(clip_to_ndc(world_to_clip(model_to_world(model, world_matrix), projection_view_matrix)), viewport);
    }

    static Vec2d world_to_ss(const Vec3d& world, const Matrix4d& projection_view_matrix, const std::array<int, 4>& viewport) {
        return ndc_to_ss(clip_to_ndc(world_to_clip(world, projection_view_matrix)), viewport);
    }

    static const Matrix4d& ndc_to_ss_matrix(const std::array<int, 4>& viewport) {
        update(viewport);
        return s_cache.ndc_to_ss_matrix;
    }

    static const Transform3d ndc_to_ss_matrix_inverse(const std::array<int, 4>& viewport) {
        update(viewport);
        return s_cache.ndc_to_ss_matrix_inverse;
    }

private:
    static void update(const std::array<int, 4>& viewport) {
        if (s_cache.viewport == viewport)
            return;

        const double half_w = 0.5 * double(viewport[2]);
        const double half_h = 0.5 * double(viewport[3]);
        s_cache.ndc_to_ss_matrix << half_w, 0.0, 0.0, double(viewport[0]) + half_w,
            0.0, half_h, 0.0, double(viewport[1]) + half_h,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0;

        s_cache.ndc_to_ss_matrix_inverse = s_cache.ndc_to_ss_matrix.inverse();
        s_cache.viewport = viewport;
    }
};

TransformHelper::Cache TransformHelper::s_cache = { { 0, 0, 0, 0 }, Matrix4d::Identity(), Transform3d::Identity() };

GLGizmoMeasure::GLGizmoMeasure(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
: GLGizmoBase(parent, icon_filename, sprite_id)
{
    GLModel::Geometry sphere_geometry = smooth_sphere(16, 7.5f);
    m_sphere.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(sphere_geometry.get_as_indexed_triangle_set()));
    m_sphere.model.init_from(std::move(sphere_geometry));

    GLModel::Geometry cylinder_geometry = smooth_cylinder(16, 5.0f, 1.0f);
    m_cylinder.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(cylinder_geometry.get_as_indexed_triangle_set()));
    m_cylinder.model.init_from(std::move(cylinder_geometry));
}

bool GLGizmoMeasure::on_mouse(const wxMouseEvent &mouse_event)
{
    m_mouse_pos = { double(mouse_event.GetX()), double(mouse_event.GetY()) };

    if (mouse_event.Moving()) {
        // only for sure 
        m_mouse_left_down = false;
        return false;
    }
    else if (mouse_event.Dragging()) {
        // Enable/Disable panning/rotating the 3D scene 
        // Ctrl is pressed or the mouse is not hovering a selected volume
        bool unlock_dragging = mouse_event.CmdDown() || (m_hover_id == -1 && !m_parent.get_selection().contains_volume(m_parent.get_first_hover_volume_idx()));
        // mode is not center selection or mouse is not hovering a center
        unlock_dragging &= !mouse_event.ShiftDown() || (m_hover_id != SELECTION_1_ID && m_hover_id != SELECTION_2_ID && m_hover_id != POINT_ID);
        return !unlock_dragging;
    }
    else if (mouse_event.LeftDown()) {
        // let the event pass through to allow panning/rotating the 3D scene
        if ((m_mode != EMode::CenterSelection && mouse_event.CmdDown()) ||
            (m_mode == EMode::CenterSelection && m_hover_id != SELECTION_1_ID && m_hover_id != SELECTION_2_ID && m_hover_id != POINT_ID)) {
            return false;
        }

        if (m_hover_id != -1) {
            SelectedFeatures selected_features_old = m_selected_features;
            m_mouse_left_down = true;
            
            auto item_from_feature = [this]() {
                SelectedFeatures::Item item;
                if (m_hover_id == SELECTION_1_ID && m_selected_features.first.feature.has_value())
                    item = m_selected_features.first;
                else if (m_hover_id == SELECTION_2_ID && m_selected_features.second.feature.has_value())
                    item = m_selected_features.second;
                else {
                    switch (m_mode)
                    {
                    case EMode::FeatureSelection:
                    {
                        item = { surface_feature_type_as_string(m_curr_feature->get_type()), m_curr_feature };
                        break;
                    }
                    case EMode::PointSelection:
                    {
                        item = { point_on_feature_type_as_string(m_curr_feature->get_type(), m_hover_id), Measure::SurfaceFeature(*m_curr_point_on_feature_position) };
                        break;
                    }
                    case EMode::CenterSelection:
                    {
                        Vec3d position;
                        switch (m_curr_feature->get_type())
                        {
                        case Measure::SurfaceFeatureType::Circle:
                        {
                            position = std::get<0>(m_curr_feature->get_circle());
                            break;
                        }
                        case Measure::SurfaceFeatureType::Edge:
                        {
                            assert(m_curr_feature->get_extra_point().has_value());
                            position = *m_curr_feature->get_extra_point();
                            break;
                        }
                        default: { assert(false); break; }
                        }

                        item = { center_on_feature_type_as_string(m_curr_feature->get_type()), Measure::SurfaceFeature(position) };
                        break;
                    }
                    }
                }
                return item;
            };

            if (m_selected_features.first.feature.has_value()) {
                auto it = std::find_if(m_selection_raycasters.begin(), m_selection_raycasters.end(),
                    [](std::shared_ptr<SceneRaycasterItem> item) { return SceneRaycaster::decode_id(SceneRaycaster::EType::Gizmo, item->get_id()) == SELECTION_2_ID; });
                if (it != m_selection_raycasters.end())
                    m_selection_raycasters.erase(it);
                m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, SELECTION_2_ID);

                const SelectedFeatures::Item item = item_from_feature();
                if (m_selected_features.first != item) {
                    if (m_selected_features.second == item)
                        m_selected_features.second.reset();
                    else {
                        m_selected_features.second = item;
                        if (m_mode == EMode::PointSelection || m_mode == EMode::CenterSelection)
                            m_selection_raycasters.push_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, SELECTION_2_ID, *m_sphere.mesh_raycaster));
                        if (m_mode == EMode::CenterSelection) {
                            // Fake ctrl up event to exit the center selection mode
                            gizmo_event(SLAGizmoEventType::CtrlUp, Vec2d::Zero(), true, false, false);
                            // increase counter to avoid that keeping the ctrl key pressed triggers a ctrl down event
                            m_ctrl_kar_filter.increase_count();
                        }
                    }
                }
                else {
                    if (!m_selected_features.second.feature.has_value())
                        m_selected_features.first.reset();
                    else {
                        m_selected_features.first = m_selected_features.second;
                        m_selected_features.second.reset();
                    }
                }
            }
            else {
                const SelectedFeatures::Item item = item_from_feature();
                m_selected_features.first = item;
                if (m_mode == EMode::PointSelection || m_mode == EMode::CenterSelection)
                    m_selection_raycasters.push_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, SELECTION_1_ID, *m_sphere.mesh_raycaster));
                if (m_mode == EMode::CenterSelection) {
                    // Fake ctrl up event to exit the center selection mode
                    gizmo_event(SLAGizmoEventType::CtrlUp, Vec2d::Zero(), true, false, false);
                    // increase counter to avoid that keeping the ctrl key pressed triggers a ctrl down event
                    m_ctrl_kar_filter.increase_count();
                }
            }

            if (m_selected_features != selected_features_old && m_selected_features.second.feature.has_value())
                m_measurement_result = Measure::get_measurement(*m_selected_features.first.feature, *m_selected_features.second.feature, m_measuring.get());

            m_imgui->set_requires_extra_frame();

            return true;
        }
        else
            // if the mouse pointer is on any volume, filter out the event to prevent the user to move it
            // equivalent tp: return (m_parent.get_first_hover_volume_idx() != -1);
            return m_curr_feature.has_value();

        // fix: prevent restart gizmo when reselect object
        // take responsibility for left up
        if (m_parent.get_first_hover_volume_idx() >= 0)
            m_mouse_left_down = true;
    }
    else if (mouse_event.LeftUp()) {
        if (m_mouse_left_down) {
            // responsible for mouse left up after selecting plane
            m_mouse_left_down = false;
            return true;
        }
        if (m_hover_id == -1 && !m_parent.is_mouse_dragging())
            // avoid closing the gizmo if the user clicks outside of any volume
            return true;
    }
    else if (mouse_event.RightDown()) {
        // let the event pass through to allow panning/rotating the 3D scene
        if ((m_mode != EMode::CenterSelection && mouse_event.CmdDown()) || (m_mode == EMode::CenterSelection && m_hover_id != SELECTION_1_ID && m_hover_id != SELECTION_2_ID)) {
            return false;
        }
    }
    else if (mouse_event.Leaving())
        m_mouse_left_down = false;

    return false;
}

void GLGizmoMeasure::data_changed()
{
    m_parent.toggle_sla_auxiliaries_visibility(false, nullptr, -1);

    update_if_needed();

    m_last_inv_zoom = 0.0f;
    m_last_plane_idx = -1;
    if (m_pending_scale) {
        m_measurement_result = Measure::get_measurement(*m_selected_features.first.feature, *m_selected_features.second.feature, m_measuring.get());
        m_pending_scale = false;
    }
    else
        m_selected_features.reset();
    m_selection_raycasters.clear();
    m_editing_distance = false;
    m_is_editing_distance_first_frame = true;
}

static bool feature_has_center(std::optional<Measure::SurfaceFeature> feature)
{
    return feature.has_value() ?
        (feature->get_type() == Measure::SurfaceFeatureType::Circle || (feature->get_type() == Measure::SurfaceFeatureType::Edge && feature->get_extra_point().has_value()))
        : false;
}

bool GLGizmoMeasure::gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    auto activate_center_selection = [this, shift_down, control_down](SLAGizmoEventType action) {
        bool ret = false;
        switch (action)
        {
        case SLAGizmoEventType::CtrlDown:  { ret = shift_down && feature_has_center(m_curr_feature); break; }
        case SLAGizmoEventType::ShiftDown: { ret = control_down && feature_has_center(m_curr_feature); break; }
        default:                           { break; }
        }
        return ret;
    };

    if (action == SLAGizmoEventType::ShiftDown) {
        if (m_shift_kar_filter.is_first()) {
            m_mode = activate_center_selection(SLAGizmoEventType::ShiftDown) ? EMode::CenterSelection : EMode::PointSelection;
            disable_scene_raycasters();
        }
        m_shift_kar_filter.increase_count();
    }
    else if (action == SLAGizmoEventType::ShiftUp) {
        m_shift_kar_filter.reset_count();
        m_mode = EMode::FeatureSelection;
        restore_scene_raycasters_state();
    }
    else if (action == SLAGizmoEventType::CtrlDown) {
        if (m_ctrl_kar_filter.is_first()) {
            if (activate_center_selection(SLAGizmoEventType::CtrlDown)) {
                m_mode = EMode::CenterSelection;
                disable_scene_raycasters();
            }
        }
        m_ctrl_kar_filter.increase_count();
    }
    else if (action == SLAGizmoEventType::CtrlUp) {
        m_ctrl_kar_filter.reset_count();
        m_mode = control_down ? EMode::PointSelection : EMode::FeatureSelection;
        restore_scene_raycasters_state();
    }
    else if (action == SLAGizmoEventType::Delete) {
        m_selected_features.reset();
        m_selection_raycasters.clear();
        m_parent.request_extra_frame();
    }
    else if (action == SLAGizmoEventType::Escape) {
        if (!m_selected_features.first.feature.has_value())
            return false;
        else {
          if (m_selected_features.second.feature.has_value())
              m_selected_features.second.feature.reset();
          else
              m_selected_features.first.feature.reset();
        }
    }

    return true;
}

bool GLGizmoMeasure::on_init()
{
    m_shortcut_key = WXK_CONTROL_U;
    return true;
}

void GLGizmoMeasure::on_set_state()
{
    if (m_state == Off) {
        m_parent.toggle_sla_auxiliaries_visibility(true, nullptr, -1);
        m_ctrl_kar_filter.reset_count();
        m_shift_kar_filter.reset_count();
        m_curr_feature.reset();
        m_curr_point_on_feature_position.reset();
        restore_scene_raycasters_state();
        m_editing_distance = false;
        m_is_editing_distance_first_frame = true;
        m_measuring.reset();
        m_raycaster.reset();
    }
    else {
        m_mode = EMode::FeatureSelection;
        // store current state of scene raycaster for later use
        m_scene_raycasters.clear();
        auto scene_raycasters = m_parent.get_raycasters_for_picking(SceneRaycaster::EType::Volume);
        if (scene_raycasters != nullptr) {
            m_scene_raycasters.reserve(scene_raycasters->size());
            for (auto r : *scene_raycasters) {
                SceneRaycasterState state = { r, r->is_active() };
                m_scene_raycasters.emplace_back(state);
            }
        }
    }
}

std::string GLGizmoMeasure::on_get_name() const
{
    return _u8L("Measure");
}

bool GLGizmoMeasure::on_is_activable() const
{
    const Selection& selection = m_parent.get_selection();
    bool res = (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA) ?
        selection.is_single_full_instance() :
        selection.is_single_full_instance() || selection.is_single_volume() || selection.is_single_modifier();
    if (res)
        res &= !selection.contains_sinking_volumes();

    return res;
}

void GLGizmoMeasure::on_render()
{
#if ENABLE_MEASURE_GIZMO_DEBUG
    render_debug_dialog();
#endif // ENABLE_MEASURE_GIZMO_DEBUG

//    // do not render if the user is panning/rotating the 3d scene
//    if (m_parent.is_mouse_dragging())
//        return;

    const Selection& selection = m_parent.get_selection();

    update_if_needed();

    const Camera& camera = wxGetApp().plater()->get_camera();
    const float inv_zoom = (float)camera.get_inv_zoom();

    Vec3f position_on_model;
    Vec3f normal_on_model;
    size_t model_facet_idx;
    const bool mouse_on_object = m_raycaster->unproject_on_mesh(m_mouse_pos, Transform3d::Identity(), camera, position_on_model, normal_on_model, nullptr, &model_facet_idx);
    const bool is_hovering_on_feature = (m_mode == EMode::PointSelection || m_mode == EMode::CenterSelection) && m_hover_id != -1;

    auto update_circle = [this, inv_zoom]() {
        if (m_last_inv_zoom != inv_zoom || m_last_circle != m_curr_feature) {
            m_last_inv_zoom = inv_zoom;
            m_last_circle = m_curr_feature;
            m_circle.reset();
            const auto [center, radius, normal] = m_curr_feature->get_circle();
            GLModel::Geometry circle_geometry = init_torus_data(64, 16, center.cast<float>(), float(radius), 5.0f * inv_zoom, normal.cast<float>(), Transform3f::Identity());
            m_circle.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(circle_geometry.get_as_indexed_triangle_set()));
            m_circle.model.init_from(std::move(circle_geometry));
            return true;
        }
        return false;
    };

    if (m_mode == EMode::FeatureSelection || m_mode == EMode::PointSelection) {
        if ((m_hover_id == SELECTION_1_ID && boost::algorithm::istarts_with(m_selected_features.first.source, _u8L("Center"))) ||
            (m_hover_id == SELECTION_2_ID && boost::algorithm::istarts_with(m_selected_features.second.source, _u8L("Center")))) {
            // Skip feature detection if hovering on a selected center
            m_curr_feature.reset();
            m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, POINT_ID);
            m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, EDGE_ID);
            m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, PLANE_ID);
            m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, CIRCLE_ID);
            m_curr_point_on_feature_position.reset();
        }
        else {
            std::optional<Measure::SurfaceFeature> curr_feature = wxGetMouseState().LeftIsDown() ? m_curr_feature :
                mouse_on_object ? m_measuring->get_feature(model_facet_idx, position_on_model.cast<double>()) : std::nullopt;

            if (m_curr_feature != curr_feature ||
                (curr_feature.has_value() && curr_feature->get_type() == Measure::SurfaceFeatureType::Circle && (m_curr_feature != curr_feature || m_last_inv_zoom != inv_zoom))) {
                m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, POINT_ID);
                m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, EDGE_ID);
                m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, PLANE_ID);
                m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, CIRCLE_ID);
                m_raycasters.clear();
                m_curr_feature = curr_feature;
                if (!m_curr_feature.has_value())
                    return;

                switch (m_curr_feature->get_type()) {
                default: { assert(false); break; }
                case Measure::SurfaceFeatureType::Point:
                {
                    m_raycasters.insert({ POINT_ID, m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, POINT_ID, *m_sphere.mesh_raycaster) });
                    break;
                }
                case Measure::SurfaceFeatureType::Edge:
                {
                    m_raycasters.insert({ EDGE_ID, m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, EDGE_ID, *m_cylinder.mesh_raycaster) });
                    if (m_curr_feature->get_extra_point().has_value())
                        m_raycasters.insert({ POINT_ID, m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, POINT_ID, *m_sphere.mesh_raycaster) });
                    break;
                }
                case Measure::SurfaceFeatureType::Circle:
                {
                    update_circle();
                    m_raycasters.insert({ CIRCLE_ID, m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CIRCLE_ID, *m_circle.mesh_raycaster) });
                    m_raycasters.insert({ POINT_ID, m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, POINT_ID, *m_sphere.mesh_raycaster) });
                    break;
                }
                case Measure::SurfaceFeatureType::Plane:
                {
                    const auto [idx, normal, point] = m_curr_feature->get_plane();
                    if (m_last_plane_idx != idx) {
                        m_last_plane_idx = idx;
                        const indexed_triangle_set& its = m_measuring->get_mesh().its;
                        const std::vector<std::vector<int>> planes_triangles = m_measuring->get_planes_triangle_indices();
                        GLModel::Geometry init_data = init_plane_data(its, planes_triangles, idx);
                        m_plane.reset();
                        m_plane.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(init_data.get_as_indexed_triangle_set()));
                        m_plane.model.init_from(std::move(init_data));
                    }

                    m_raycasters.insert({ PLANE_ID, m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, PLANE_ID, *m_plane.mesh_raycaster) });
                    break;
                }
                }
            }
        }
    }

    if (m_mode != EMode::PointSelection)
        m_curr_point_on_feature_position.reset();
    else if (is_hovering_on_feature) {
        auto position_on_feature = [this](int feature_type_id, const Camera& camera, std::function<Vec3f(const Vec3f&)> callback = nullptr) -> Vec3d {
            auto it = m_raycasters.find(feature_type_id);
            if (it != m_raycasters.end() && it->second != nullptr) {
                Vec3f p;
                Vec3f n;
                const Transform3d& trafo = it->second->get_transform();
                bool res = it->second->get_raycaster()->closest_hit(m_mouse_pos, trafo, camera, p, n);
                if (res) {
                    if (callback)
                        p = callback(p);
                    return trafo * p.cast<double>();
                }
            }
            return Vec3d(DBL_MAX, DBL_MAX, DBL_MAX);
        };

        if (m_curr_feature.has_value()) {
            switch (m_curr_feature->get_type())
            {
            default: { assert(false); break; }
            case Measure::SurfaceFeatureType::Point:
            {
                m_curr_point_on_feature_position = m_curr_feature->get_point();
                break;
            }
            case Measure::SurfaceFeatureType::Edge:
            {
                const std::optional<Vec3d> extra = m_curr_feature->get_extra_point();
                if (extra.has_value() && m_hover_id == POINT_ID)
                    m_curr_point_on_feature_position = *extra;
                else {
                    const Vec3d pos = position_on_feature(EDGE_ID, camera, [](const Vec3f& v) { return Vec3f(0.0f, 0.0f, v.z()); });
                    if (!pos.isApprox(Vec3d(DBL_MAX, DBL_MAX, DBL_MAX)))
                        m_curr_point_on_feature_position = pos;
                }
                break;
            }
            case Measure::SurfaceFeatureType::Plane:
            {
                m_curr_point_on_feature_position = position_on_feature(PLANE_ID, camera);
                break;
            }
            case Measure::SurfaceFeatureType::Circle:
            {
                const auto [center, radius, normal] = m_curr_feature->get_circle();
                if (m_hover_id == POINT_ID)
                    m_curr_point_on_feature_position = center;
                else {
                    const Vec3d world_pof = position_on_feature(CIRCLE_ID, camera, [](const Vec3f& v) { return v; });
                    const Eigen::Hyperplane<double, 3> plane(normal, center);
                    const Transform3d local_to_model_matrix = Geometry::translation_transform(center) * Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitZ(), normal);
                    const Vec3d local_proj = local_to_model_matrix.inverse() * plane.projection(world_pof);
                    double angle = std::atan2(local_proj.y(), local_proj.x());
                    if (angle < 0.0)
                        angle += 2.0 * double(M_PI);

                    const Vec3d local_pos = radius * Vec3d(std::cos(angle), std::sin(angle), 0.0);
                    m_curr_point_on_feature_position = local_to_model_matrix * local_pos;
                }
                break;
            }
            }
        }
    }
    else {
        m_curr_point_on_feature_position.reset();
        if (m_curr_feature.has_value() && m_curr_feature->get_type() == Measure::SurfaceFeatureType::Circle) {
            if (update_circle()) {
                m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, CIRCLE_ID);
                auto it = m_raycasters.find(CIRCLE_ID);
                if (it != m_raycasters.end())
                    m_raycasters.erase(it);
                m_raycasters.insert({ CIRCLE_ID, m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CIRCLE_ID, *m_circle.mesh_raycaster) });
            }
        }
    }

    if (!m_curr_feature.has_value() && !m_selected_features.first.feature.has_value())
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;

    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));
    const bool old_cullface = ::glIsEnabled(GL_CULL_FACE);
    glsafe(::glDisable(GL_CULL_FACE));

    const Transform3d& view_matrix = camera.get_view_matrix();

    auto set_matrix_uniforms = [shader, &view_matrix](const Transform3d& model_matrix) {
        const Transform3d view_model_matrix = view_matrix * model_matrix;
        shader->set_uniform("view_model_matrix", view_model_matrix);
        const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal_matrix);
    };

    auto set_emission_uniform = [shader](const ColorRGBA& color, bool hover) {
        shader->set_uniform("emission_factor", (color == GLVolume::SELECTED_COLOR) ? 0.0f :
            hover ? 0.5f : 0.25f);
    };

    auto render_feature = [this, set_matrix_uniforms, set_emission_uniform](const Measure::SurfaceFeature& feature, const std::vector<ColorRGBA>& colors,
        float inv_zoom, bool hover, bool update_raycasters_transform) {
            switch (feature.get_type())
            {
            default: { assert(false); break; }
            case Measure::SurfaceFeatureType::Point:
            {
                const Transform3d feature_matrix = Geometry::translation_transform(feature.get_point()) * Geometry::scale_transform(inv_zoom);
                set_matrix_uniforms(feature_matrix);
                set_emission_uniform(colors.front(), hover);
                m_sphere.model.set_color(colors.front());
                m_sphere.model.render();
                if (update_raycasters_transform) {
                    auto it = m_raycasters.find(POINT_ID);
                    if (it != m_raycasters.end() && it->second != nullptr)
                        it->second->set_transform(feature_matrix);
                }
                break;
            }
            case Measure::SurfaceFeatureType::Circle:
            {
                const auto& [center, radius, normal] = feature.get_circle();
                // render center
                if (update_raycasters_transform) {
                    const Transform3d center_matrix = Geometry::translation_transform(center) * Geometry::scale_transform(inv_zoom);
                    set_matrix_uniforms(center_matrix);
                    set_emission_uniform(colors.front(), hover);
                    m_sphere.model.set_color(colors.front());
                    m_sphere.model.render();
                    auto it = m_raycasters.find(POINT_ID);
                    if (it != m_raycasters.end() && it->second != nullptr)
                        it->second->set_transform(center_matrix);
                }
                // render circle
                if (m_mode != EMode::CenterSelection) {
                    const Transform3d circle_matrix = Transform3d::Identity();
                    set_matrix_uniforms(circle_matrix);
                    if (update_raycasters_transform) {
                        set_emission_uniform(colors.back(), hover);
                        m_circle.model.set_color(colors.back());
                        m_circle.model.render();
                        auto it = m_raycasters.find(CIRCLE_ID);
                        if (it != m_raycasters.end() && it->second != nullptr)
                            it->second->set_transform(circle_matrix);
                    }
                    else {
                        GLModel circle;
                        GLModel::Geometry circle_geometry = init_torus_data(64, 16, center.cast<float>(), float(radius), 5.0f * inv_zoom, normal.cast<float>(), Transform3f::Identity());
                        circle.init_from(std::move(circle_geometry));
                        set_emission_uniform(colors.back(), hover);
                        circle.set_color(colors.back());
                        circle.render();
                    }
                }
                break;
            }
            case Measure::SurfaceFeatureType::Edge:
            {
                const auto& [from, to] = feature.get_edge();
                // render extra point
                if (update_raycasters_transform) {
                    const std::optional<Vec3d> extra = feature.get_extra_point();
                    if (extra.has_value()) {
                        const Transform3d point_matrix = Geometry::translation_transform(*extra) * Geometry::scale_transform(inv_zoom);
                        set_matrix_uniforms(point_matrix);
                        set_emission_uniform(colors.front(), hover);
                        m_sphere.model.set_color(colors.front());
                        m_sphere.model.render();
                        auto it = m_raycasters.find(POINT_ID);
                        if (it != m_raycasters.end() && it->second != nullptr)
                            it->second->set_transform(point_matrix);
                    }
                }
                // render edge
                if (m_mode != EMode::CenterSelection) {
                    const Transform3d edge_matrix = Geometry::translation_transform(from) *
                    Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitZ(), to - from) *
                    Geometry::scale_transform({ (double)inv_zoom, (double)inv_zoom, (to - from).norm() });
                    set_matrix_uniforms(edge_matrix);
                    set_emission_uniform(colors.back(), hover);
                    m_cylinder.model.set_color(colors.back());
                    m_cylinder.model.render();
                    if (update_raycasters_transform) {
                        auto it = m_raycasters.find(EDGE_ID);
                        if (it != m_raycasters.end() && it->second != nullptr)
                            it->second->set_transform(edge_matrix);
                    }
                }
                break;
            }
            case Measure::SurfaceFeatureType::Plane:
            {
                // no need to render the plane in case it is rendered with the same color as the volume in the 3D scene
                if (colors.front() != m_parent.get_selection().get_first_volume()->render_color) {
                    const auto& [idx, normal, pt] = feature.get_plane();
                    assert(idx < m_plane_models_cache.size());
                    set_matrix_uniforms(Transform3d::Identity());
                    set_emission_uniform(colors.front(), hover);
                    m_plane_models_cache[idx].set_color(colors.front());
                    m_plane_models_cache[idx].render();
                }
                if (update_raycasters_transform) {
                    auto it = m_raycasters.find(PLANE_ID);
                    if (it != m_raycasters.end() && it->second != nullptr)
                        it->second->set_transform(Transform3d::Identity());
                }
                break;
            }
            }
    };

    auto hover_selection_color = [this]() {
      return ((m_mode == EMode::PointSelection && !m_selected_features.first.feature.has_value()) ||
              (m_mode != EMode::PointSelection && (!m_selected_features.first.feature.has_value() || *m_curr_feature == *m_selected_features.first.feature))) ?
          SELECTED_1ST_COLOR : SELECTED_2ND_COLOR;
    };

    auto hovering_color = [this, hover_selection_color]() {
        return (m_mode == EMode::PointSelection) ? GLVolume::SELECTED_COLOR : hover_selection_color();
    };

    if (m_curr_feature.has_value()) {
        std::vector<ColorRGBA> colors;
        if (m_selected_features.first.feature.has_value() && *m_curr_feature == *m_selected_features.first.feature)
            colors.emplace_back(hovering_color());
        else if (m_selected_features.second.feature.has_value() && *m_curr_feature == *m_selected_features.second.feature)
            colors.emplace_back(hovering_color());
        else {
            switch (m_curr_feature->get_type())
            {
            default: { assert(false); break; }
            case Measure::SurfaceFeatureType::Point:
            {
                colors.emplace_back(hover_selection_color());
                break;
            }
            case Measure::SurfaceFeatureType::Edge:
            case Measure::SurfaceFeatureType::Circle:
            {
                colors.emplace_back((m_hover_id == POINT_ID) ? hover_selection_color() : hovering_color());
                colors.emplace_back(hovering_color());
                break;
            }
            case Measure::SurfaceFeatureType::Plane:
            {
                colors.emplace_back(hovering_color());
                break;
            }
            }
        }

        render_feature(*m_curr_feature, colors, inv_zoom, true, true);
    }

    if (m_selected_features.first.feature.has_value() && (!m_curr_feature.has_value() || *m_curr_feature != *m_selected_features.first.feature)) {
        const std::vector<ColorRGBA> colors = { SELECTED_1ST_COLOR };
        render_feature(*m_selected_features.first.feature, colors, inv_zoom, m_hover_id == SELECTION_1_ID, false);
        if (m_selected_features.first.feature->get_type() == Measure::SurfaceFeatureType::Point) {
            auto it = std::find_if(m_selection_raycasters.begin(), m_selection_raycasters.end(),
                [](std::shared_ptr<SceneRaycasterItem> item) { return SceneRaycaster::decode_id(SceneRaycaster::EType::Gizmo, item->get_id()) == SELECTION_1_ID; });
            if (it != m_selection_raycasters.end())
                (*it)->set_transform(Geometry::translation_transform(m_selected_features.first.feature->get_point()) * Geometry::scale_transform(inv_zoom));
        }
    }
    if (m_selected_features.second.feature.has_value() && (!m_curr_feature.has_value() || *m_curr_feature != *m_selected_features.second.feature)) {
        const std::vector<ColorRGBA> colors = { SELECTED_2ND_COLOR };
        render_feature(*m_selected_features.second.feature, colors, inv_zoom, m_hover_id == SELECTION_2_ID, false);
        if (m_selected_features.second.feature->get_type() == Measure::SurfaceFeatureType::Point) {
            auto it = std::find_if(m_selection_raycasters.begin(), m_selection_raycasters.end(),
                [](std::shared_ptr<SceneRaycasterItem> item) { return SceneRaycaster::decode_id(SceneRaycaster::EType::Gizmo, item->get_id()) == SELECTION_2_ID; });
            if (it != m_selection_raycasters.end())
                (*it)->set_transform(Geometry::translation_transform(m_selected_features.second.feature->get_point()) * Geometry::scale_transform(inv_zoom));
        }
    }

    if (is_hovering_on_feature && m_curr_point_on_feature_position.has_value()) {
        if (m_hover_id != POINT_ID) {
            const Transform3d matrix = Geometry::translation_transform(*m_curr_point_on_feature_position) * Geometry::scale_transform(inv_zoom);
            set_matrix_uniforms(matrix);
            const ColorRGBA color = hover_selection_color();
            set_emission_uniform(color, true);
            m_sphere.model.set_color(color);
            m_sphere.model.render();
        }
    }

    shader->stop_using();

    if (old_cullface)
        glsafe(::glEnable(GL_CULL_FACE));

    render_dimensioning();
}

void GLGizmoMeasure::update_if_needed()
{
    auto update_plane_models_cache = [this](const indexed_triangle_set& its) {
        m_plane_models_cache.clear();
        const std::vector<std::vector<int>> planes_triangles = m_measuring->get_planes_triangle_indices();
        for (int idx = 0; idx < (int)planes_triangles.size(); ++idx) {
            m_plane_models_cache.emplace_back(GLModel());
            GLModel::Geometry init_data = init_plane_data(its, planes_triangles, idx);
            m_plane_models_cache.back().init_from(std::move(init_data));
        }
    };

    auto do_update = [this, update_plane_models_cache](const std::vector<VolumeCacheItem>& volumes_cache, const Selection& selection) {
        TriangleMesh composite_mesh;
        for (const auto& vol : volumes_cache) {
//          if (selection.is_single_full_instance() && vol.volume->is_modifier())
//              continue;

            TriangleMesh volume_mesh = vol.volume->mesh();
            volume_mesh.transform(vol.world_trafo);
            composite_mesh.merge(volume_mesh);
        }

        m_measuring.reset(new Measure::Measuring(composite_mesh.its));
        update_plane_models_cache(m_measuring->get_mesh().its);
        m_raycaster.reset(new MeshRaycaster(std::make_shared<const TriangleMesh>(m_measuring->get_mesh())));
        m_volumes_cache = volumes_cache;
    };

    const Selection& selection = m_parent.get_selection();
    if (selection.is_empty())
        return;

    const Selection::IndicesList& idxs = selection.get_volume_idxs();
    std::vector<VolumeCacheItem> volumes_cache;
    volumes_cache.reserve(idxs.size());
    for (unsigned int idx : idxs) {
        const GLVolume* v = selection.get_volume(idx);
        const int volume_idx = v->volume_idx();
        if (volume_idx < 0)
            continue;

        const ModelObject* obj = selection.get_model()->objects[v->object_idx()];
        const ModelInstance* inst = obj->instances[v->instance_idx()];
        const ModelVolume* vol = obj->volumes[volume_idx];
        const VolumeCacheItem item = {
            obj, inst, vol,
            Geometry::translation_transform(selection.get_first_volume()->get_sla_shift_z() * Vec3d::UnitZ()) * inst->get_matrix() * vol->get_matrix()
        };
        volumes_cache.emplace_back(item);
    }

    if (m_state != On || volumes_cache.empty())
        return;

    if (m_measuring == nullptr || m_volumes_cache != volumes_cache)
        do_update(volumes_cache, selection);
}

void GLGizmoMeasure::disable_scene_raycasters()
{
    for (auto r : m_scene_raycasters) {
        r.raycaster->set_active(false);
    }
}

void GLGizmoMeasure::restore_scene_raycasters_state()
{
    for (auto r : m_scene_raycasters) {
        r.raycaster->set_active(r.state);
    }
}

void GLGizmoMeasure::render_dimensioning()
{
    static SelectedFeatures last_selected_features;

    if (!m_selected_features.first.feature.has_value() || !m_selected_features.second.feature.has_value())
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    auto point_point = [this, &shader](const Vec3d& v1, const Vec3d& v2, float distance) {
        if ((v2 - v1).squaredNorm() < 0.000001 || distance < 0.001f)
            return;

        const Camera& camera = wxGetApp().plater()->get_camera();
        const Matrix4d projection_view_matrix = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
        const std::array<int, 4>& viewport = camera.get_viewport();

        // screen coordinates
        const Vec2d v1ss = TransformHelper::world_to_ss(v1, projection_view_matrix, viewport);
        const Vec2d v2ss = TransformHelper::world_to_ss(v2, projection_view_matrix, viewport);

        if (v1ss.isApprox(v2ss))
            return;

        const Vec2d v12ss = v2ss - v1ss;
        const double v12ss_len = v12ss.norm();

        const bool overlap = v12ss_len - 2.0 * TRIANGLE_HEIGHT < 0.0;

        const auto q12ss = Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitX(), Vec3d(v12ss.x(), v12ss.y(), 0.0));
        const auto q21ss = Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitX(), Vec3d(-v12ss.x(), -v12ss.y(), 0.0));

        shader->set_uniform("projection_matrix", Transform3d::Identity());

        const Vec3d v1ss_3 = { v1ss.x(), v1ss.y(), 0.0 };
        const Vec3d v2ss_3 = { v2ss.x(), v2ss.y(), 0.0 };

        const Transform3d ss_to_ndc_matrix = TransformHelper::ndc_to_ss_matrix_inverse(viewport);

#if ENABLE_GL_CORE_PROFILE
        if (OpenGLManager::get_gl_info().is_core_profile()) {
            shader->stop_using();

            shader = wxGetApp().get_shader("dashed_thick_lines");
            if (shader == nullptr)
                return;

            shader->start_using();
            shader->set_uniform("projection_matrix", Transform3d::Identity());
            const std::array<int, 4>& viewport = camera.get_viewport();
            shader->set_uniform("viewport_size", Vec2d(double(viewport[2]), double(viewport[3])));
            shader->set_uniform("width", 1.0f);
            shader->set_uniform("gap_size", 0.0f);
        }
        else
#endif // ENABLE_GL_CORE_PROFILE
            glsafe(::glLineWidth(2.0f));

        // stem
        shader->set_uniform("view_model_matrix", overlap ?
            ss_to_ndc_matrix * Geometry::translation_transform(v1ss_3) * q12ss * Geometry::translation_transform(-2.0 * TRIANGLE_HEIGHT * Vec3d::UnitX()) * Geometry::scale_transform({ v12ss_len + 4.0 * TRIANGLE_HEIGHT, 1.0f, 1.0f }) :
            ss_to_ndc_matrix * Geometry::translation_transform(v1ss_3) * q12ss * Geometry::scale_transform({ v12ss_len, 1.0f, 1.0f }));
        m_dimensioning.line.set_color(ColorRGBA::WHITE());
        m_dimensioning.line.render();

#if ENABLE_GL_CORE_PROFILE
        if (OpenGLManager::get_gl_info().is_core_profile()) {
            shader->stop_using();

            shader = wxGetApp().get_shader("flat");
            if (shader == nullptr)
                return;

            shader->start_using();
        }
        else
#endif // ENABLE_GL_CORE_PROFILE
            glsafe(::glLineWidth(1.0f));

        // arrow 1
        shader->set_uniform("view_model_matrix", overlap ?
            ss_to_ndc_matrix * Geometry::translation_transform(v1ss_3) * q12ss :
            ss_to_ndc_matrix * Geometry::translation_transform(v1ss_3) * q21ss);
        m_dimensioning.triangle.render();

        // arrow 2
        shader->set_uniform("view_model_matrix", overlap ?
            ss_to_ndc_matrix * Geometry::translation_transform(v2ss_3) * q21ss :
            ss_to_ndc_matrix * Geometry::translation_transform(v2ss_3) * q12ss);
        m_dimensioning.triangle.render();

        const bool use_inches = wxGetApp().app_config->get("use_inches") == "1";
        const double curr_value = use_inches ? ObjectManipulation::mm_to_in * distance : distance;
        const std::string curr_value_str = format_double(curr_value);
        const std::string units = use_inches ? _u8L("in") : _u8L("mm");
        const float value_str_width = 20.0f + ImGui::CalcTextSize(curr_value_str.c_str()).x;
        static double edit_value = 0.0;

        const Vec2d label_position = 0.5 * (v1ss + v2ss);
        m_imgui->set_next_window_pos(label_position.x(), viewport[3] - label_position.y(), ImGuiCond_Always, 0.0f, 1.0f);
        m_imgui->set_next_window_bg_alpha(0.0f);

        if (!m_editing_distance) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 1.0f, 1.0f });
            m_imgui->begin(std::string("distance"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration);
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
            ImGui::AlignTextToFramePadding();
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const std::string txt = curr_value_str + " " + units;
            ImVec2 txt_size = ImGui::CalcTextSize(txt.c_str());
            const ImGuiStyle& style = ImGui::GetStyle();
            draw_list->AddRectFilled({ pos.x - style.FramePadding.x, pos.y + style.FramePadding.y }, { pos.x + txt_size.x + 2.0f * style.FramePadding.x , pos.y + txt_size.y + 2.0f * style.FramePadding.y },
              ImGuiWrapper::to_ImU32(ColorRGBA(0.5f, 0.5f, 0.5f, 0.5f)));
            ImGui::SetCursorScreenPos({ pos.x + style.FramePadding.x, pos.y });
            m_imgui->text(txt);
            ImGui::SameLine();
            if (m_imgui->image_button(ImGui::SliderFloatEditBtnIcon, _L("Edit to scale"))) {
                m_editing_distance = true;
                edit_value = curr_value;
                m_imgui->requires_extra_frame();
            }
            m_imgui->end();
            ImGui::PopStyleVar(3);
        }

        if (m_editing_distance && !ImGui::IsPopupOpen("distance_popup"))
            ImGui::OpenPopup("distance_popup");

        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 1.0f, 1.0f });
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 4.0f, 0.0f });
        if (ImGui::BeginPopupModal("distance_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration)) {
            auto perform_scale = [this](double new_value, double old_value) {
                if (new_value == old_value || new_value <= 0.0)
                    return;

                const double ratio = new_value / old_value;
                wxGetApp().plater()->take_snapshot(_L("Scale"));

                struct TrafoData
                {
                    double ratio;
                    Vec3d old_pivot;
                    Vec3d new_pivot;
                    Transform3d scale_matrix;

                    TrafoData(double ratio, const Vec3d& old_pivot, const Vec3d& new_pivot) {
                        this->ratio = ratio;
                        this->scale_matrix = Geometry::scale_transform(ratio);
                        this->old_pivot = old_pivot;
                        this->new_pivot = new_pivot;
                    }

                    Vec3d transform(const Vec3d& point) const { return this->scale_matrix * (point - this->old_pivot) + this->new_pivot; }
                };

                auto scale_feature = [](Measure::SurfaceFeature& feature, const TrafoData& trafo_data) {
                    switch (feature.get_type())
                    {
                    case Measure::SurfaceFeatureType::Point:
                    {
                        feature = Measure::SurfaceFeature(trafo_data.transform(feature.get_point()));
                        break;
                    }
                    case Measure::SurfaceFeatureType::Edge:
                    {
                        const auto [from, to] = feature.get_edge();
                        const std::optional<Vec3d> extra = feature.get_extra_point();
                        const std::optional<Vec3d> new_extra = extra.has_value() ? trafo_data.transform(*extra) : extra;
                        feature = Measure::SurfaceFeature(Measure::SurfaceFeatureType::Edge, trafo_data.transform(from), trafo_data.transform(to), new_extra);
                        break;
                    }
                    case Measure::SurfaceFeatureType::Circle:
                    {
                        const auto [center, radius, normal] = feature.get_circle();
                        feature = Measure::SurfaceFeature(Measure::SurfaceFeatureType::Circle, trafo_data.transform(center), normal, std::nullopt, trafo_data.ratio * radius);
                        break;
                    }
                    case Measure::SurfaceFeatureType::Plane:
                    {
                        const auto [idx, normal, origin] = feature.get_plane();
                        feature = Measure::SurfaceFeature(Measure::SurfaceFeatureType::Plane, normal, trafo_data.transform(origin), std::nullopt, idx);
                        break;
                    }
                    default: { break; }
                    }
                  };

                // apply scale
                TransformationType type;
                type.set_world();
                type.set_relative();
                type.set_joint();

                // scale selection
                Selection& selection = m_parent.get_selection();
                const Vec3d old_center = selection.get_bounding_box().center();
                selection.setup_cache();
                selection.scale(ratio * Vec3d::Ones(), type);
                wxGetApp().plater()->canvas3D()->do_scale(""); // avoid storing another snapshot
                wxGetApp().obj_manipul()->set_dirty();

                // scale dimensioning
                const Vec3d new_center = selection.get_bounding_box().center();
                const TrafoData trafo_data(ratio, old_center, new_center);
                scale_feature(*m_selected_features.first.feature, trafo_data);
                scale_feature(*m_selected_features.second.feature, trafo_data);

                // update measure on next call to data_changed()
                m_pending_scale = true;
            };
            auto action_exit = [this]() {
                m_editing_distance = false;
                m_is_editing_distance_first_frame = true;
                ImGui::CloseCurrentPopup();
            };
            auto action_scale = [perform_scale, action_exit](double new_value, double old_value) {
                perform_scale(new_value, old_value);
                action_exit();
            };

            m_imgui->disable_background_fadeout_animation();
            ImGui::PushItemWidth(value_str_width);
            if (ImGui::InputDouble("##distance", &edit_value, 0.0f, 0.0f, "%.3f")) {
            }

            // trick to auto-select text in the input widgets on 1st frame
            if (m_is_editing_distance_first_frame) {
                ImGui::SetKeyboardFocusHere(0);
                m_is_editing_distance_first_frame = false;
                m_imgui->set_requires_extra_frame();
            }

            // handle keys input
            if (ImGui::IsKeyPressedMap(ImGuiKey_Enter) || ImGui::IsKeyPressedMap(ImGuiKey_KeyPadEnter))
                action_scale(edit_value, curr_value);
            else if (ImGui::IsKeyPressedMap(ImGuiKey_Escape))
                action_exit();

            ImGui::SameLine();
            if (m_imgui->button(_u8L("Scale")))
                action_scale(edit_value, curr_value);
            ImGui::SameLine();
            if (m_imgui->button(_u8L("Cancel")))
                action_exit();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(4);
    };

    auto point_edge = [this, shader](const Measure::SurfaceFeature& f1, const Measure::SurfaceFeature& f2) {
        assert(f1.get_type() == Measure::SurfaceFeatureType::Point && f2.get_type() == Measure::SurfaceFeatureType::Edge);
        std::pair<Vec3d, Vec3d> e = f2.get_edge();
        const Vec3d v_proj = m_measurement_result.distance_infinite->to;
        const Vec3d e1e2 = e.second - e.first;
        const Vec3d v_proje1 = v_proj - e.first;
        const bool on_e1_side = v_proje1.dot(e1e2) < -EPSILON;
        const bool on_e2_side = !on_e1_side && v_proje1.norm() > e1e2.norm();
        if (on_e1_side || on_e2_side) {
            const Camera& camera = wxGetApp().plater()->get_camera();
            const Matrix4d projection_view_matrix = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
            const std::array<int, 4>& viewport = camera.get_viewport();
            const Transform3d ss_to_ndc_matrix = TransformHelper::ndc_to_ss_matrix_inverse(viewport);

            const Vec2d v_projss = TransformHelper::world_to_ss(v_proj, projection_view_matrix, viewport);
            auto render_extension = [this, &v_projss, &projection_view_matrix, &viewport, &ss_to_ndc_matrix, shader](const Vec3d& p) {
                const Vec2d pss = TransformHelper::world_to_ss(p, projection_view_matrix, viewport);
                if (!pss.isApprox(v_projss)) {
                    const Vec2d pv_projss = v_projss - pss;
                    const double pv_projss_len = pv_projss.norm();

                    const auto q = Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitX(), Vec3d(pv_projss.x(), pv_projss.y(), 0.0));

                    shader->set_uniform("projection_matrix", Transform3d::Identity());
                    shader->set_uniform("view_model_matrix", ss_to_ndc_matrix * Geometry::translation_transform({ pss.x(), pss.y(), 0.0 }) * q *
                        Geometry::scale_transform({ pv_projss_len, 1.0f, 1.0f }));
                    m_dimensioning.line.set_color(ColorRGBA::LIGHT_GRAY());
                    m_dimensioning.line.render();
                }
            };

            render_extension(on_e1_side ? e.first : e.second);
        }
    };

    auto arc_edge_edge = [this, &shader](const Measure::SurfaceFeature& f1, const Measure::SurfaceFeature& f2, double radius = 0.0) {
        assert(f1.get_type() == Measure::SurfaceFeatureType::Edge && f2.get_type() == Measure::SurfaceFeatureType::Edge);
        if (!m_measurement_result.angle.has_value())
            return;

        const double angle = m_measurement_result.angle->angle;
        const Vec3d  center = m_measurement_result.angle->center;
        const std::pair<Vec3d, Vec3d> e1 = m_measurement_result.angle->e1;
        const std::pair<Vec3d, Vec3d> e2 = m_measurement_result.angle->e2;
        const double calc_radius = m_measurement_result.angle->radius;
        const bool   coplanar = m_measurement_result.angle->coplanar;

        if (std::abs(angle) < EPSILON || std::abs(calc_radius) < EPSILON)
            return;

        const double draw_radius = (radius > 0.0) ? radius : calc_radius;

        const Vec3d e1_unit = Measure::edge_direction(e1);
        const Vec3d e2_unit = Measure::edge_direction(e2);

        const unsigned int resolution = std::max<unsigned int>(2, 64 * angle / double(PI));
        const double step = angle / double(resolution);
        const Vec3d normal = e1_unit.cross(e2_unit).normalized();

        if (!m_dimensioning.arc.is_initialized()) {
            GLModel::Geometry init_data;
            init_data.format = { GLModel::Geometry::EPrimitiveType::LineStrip, GLModel::Geometry::EVertexLayout::P3 };
            init_data.color = ColorRGBA::WHITE();
            init_data.reserve_vertices(resolution + 1);
            init_data.reserve_indices(resolution + 1);

            // vertices + indices
            for (unsigned int i = 0; i <= resolution; ++i) {
                const double a = step * double(i);
                const Vec3d v = draw_radius * (Eigen::Quaternion<double>(Eigen::AngleAxisd(a, normal)) * e1_unit);
                init_data.add_vertex((Vec3f)v.cast<float>());
                init_data.add_index(i);
            }

            m_dimensioning.arc.init_from(std::move(init_data));
        }

        const Camera& camera = wxGetApp().plater()->get_camera();
#if ENABLE_GL_CORE_PROFILE
        if (OpenGLManager::get_gl_info().is_core_profile()) {
            shader->stop_using();

            shader = wxGetApp().get_shader("dashed_thick_lines");
            if (shader == nullptr)
                return;

            shader->start_using();
            shader->set_uniform("projection_matrix", Transform3d::Identity());
            const std::array<int, 4>& viewport = camera.get_viewport();
            shader->set_uniform("viewport_size", Vec2d(double(viewport[2]), double(viewport[3])));
            shader->set_uniform("width", 1.0f);
            shader->set_uniform("gap_size", 0.0f);
        }
        else
#endif // ENABLE_GL_CORE_PROFILE
          glsafe(::glLineWidth(2.0f));

        // arc
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        shader->set_uniform("view_model_matrix", camera.get_view_matrix() * Geometry::translation_transform(center));
        m_dimensioning.arc.render();

#if ENABLE_GL_CORE_PROFILE
        if (OpenGLManager::get_gl_info().is_core_profile()) {
            shader->stop_using();

            shader = wxGetApp().get_shader("flat");
            if (shader == nullptr)
                return;

            shader->start_using();
        }
        else
#endif // ENABLE_GL_CORE_PROFILE
          glsafe(::glLineWidth(1.0f));

        // arrows
        auto render_arrow = [this, shader, &camera, &normal, &center, &e1_unit, draw_radius, step, resolution](unsigned int endpoint_id) {
            const double angle = (endpoint_id == 1) ? 0.0 : step * double(resolution);
            const Vec3d position_model = Geometry::translation_transform(center) * (draw_radius * (Eigen::Quaternion<double>(Eigen::AngleAxisd(angle, normal)) * e1_unit));
            const Vec3d direction_model = (endpoint_id == 1) ? -normal.cross(position_model - center).normalized() : normal.cross(position_model - center).normalized();
            const auto qz = Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitZ(), (endpoint_id == 1) ? normal : -normal);
            const auto qx = Eigen::Quaternion<double>::FromTwoVectors(qz * Vec3d::UnitX(), direction_model);
            const Transform3d view_model_matrix = camera.get_view_matrix() * Geometry::translation_transform(position_model) *
                qx * qz * Geometry::scale_transform(camera.get_inv_zoom());
            shader->set_uniform("view_model_matrix", view_model_matrix);
            m_dimensioning.triangle.render();
        };

        glsafe(::glDisable(GL_CULL_FACE));
        render_arrow(1);
        render_arrow(2);
        glsafe(::glEnable(GL_CULL_FACE));

        // edge 1 extension
        const Vec3d e11e12 = e1.second - e1.first;
        const Vec3d e11center = center - e1.first;
        const double e11center_len = e11center.norm();
        if (e11center_len > EPSILON && e11center.dot(e11e12) < 0.0) {
            shader->set_uniform("view_model_matrix", camera.get_view_matrix() * Geometry::translation_transform(center) *
                Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitX(), Measure::edge_direction(e1.first, e1.second)) *
                Geometry::scale_transform({ e11center_len, 1.0f, 1.0f }));
            m_dimensioning.line.set_color(ColorRGBA::LIGHT_GRAY());
            m_dimensioning.line.render();
        }

        // edge 2 extension
        const Vec3d e21center = center - e2.first;
        const double e21center_len = e21center.norm();
        if (e21center_len > EPSILON) {
            shader->set_uniform("view_model_matrix", camera.get_view_matrix() * Geometry::translation_transform(center) *
                Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitX(), Measure::edge_direction(e2.first, e2.second)) *
                Geometry::scale_transform({ (coplanar && radius > 0.0) ? e21center_len : draw_radius, 1.0f, 1.0f }));
            m_dimensioning.line.set_color(ColorRGBA::LIGHT_GRAY());
            m_dimensioning.line.render();
        }

        // label
        // label world coordinates
        const Vec3d label_position_world = Geometry::translation_transform(center) * (draw_radius * (Eigen::Quaternion<double>(Eigen::AngleAxisd(step * 0.5 * double(resolution), normal)) * e1_unit));

        // label screen coordinates
        const std::array<int, 4>& viewport = camera.get_viewport();
        const Vec2d label_position_ss = TransformHelper::world_to_ss(label_position_world,
            camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix(), viewport);

        m_imgui->set_next_window_pos(label_position_ss.x(), viewport[3] - label_position_ss.y(), ImGuiCond_Always, 0.0f, 1.0f);
        m_imgui->set_next_window_bg_alpha(0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        m_imgui->begin(_L("##angle"), ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        ImGui::AlignTextToFramePadding();
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const std::string txt = format_double(Geometry::rad2deg(angle)) + "°";
        ImVec2 txt_size = ImGui::CalcTextSize(txt.c_str());
        const ImGuiStyle& style = ImGui::GetStyle();
        draw_list->AddRectFilled({ pos.x - style.FramePadding.x, pos.y + style.FramePadding.y }, { pos.x + txt_size.x + 2.0f * style.FramePadding.x , pos.y + txt_size.y + 2.0f * style.FramePadding.y },
          ImGuiWrapper::to_ImU32(ColorRGBA(0.5f, 0.5f, 0.5f, 0.5f)));
        ImGui::SetCursorScreenPos({ pos.x + style.FramePadding.x, pos.y });
        m_imgui->text(txt);
        m_imgui->end();
        ImGui::PopStyleVar();
    };

    auto arc_edge_plane = [this, arc_edge_edge](const Measure::SurfaceFeature& f1, const Measure::SurfaceFeature& f2) {
        assert(f1.get_type() == Measure::SurfaceFeatureType::Edge && f2.get_type() == Measure::SurfaceFeatureType::Plane);
        if (!m_measurement_result.angle.has_value())
            return;

        const std::pair<Vec3d, Vec3d> e1 = m_measurement_result.angle->e1;
        const std::pair<Vec3d, Vec3d> e2 = m_measurement_result.angle->e2;
        const double calc_radius = m_measurement_result.angle->radius;

        if (calc_radius == 0.0)
            return;

        arc_edge_edge(Measure::SurfaceFeature(Measure::SurfaceFeatureType::Edge, e1.first, e1.second),
            Measure::SurfaceFeature(Measure::SurfaceFeatureType::Edge, e2.first, e2.second), calc_radius);
    };

    auto arc_plane_plane = [this, arc_edge_edge](const Measure::SurfaceFeature& f1, const Measure::SurfaceFeature& f2) {
        assert(f1.get_type() == Measure::SurfaceFeatureType::Plane && f2.get_type() == Measure::SurfaceFeatureType::Plane);
        if (!m_measurement_result.angle.has_value())
            return;

        const std::pair<Vec3d, Vec3d> e1 = m_measurement_result.angle->e1;
        const std::pair<Vec3d, Vec3d> e2 = m_measurement_result.angle->e2;
        const double calc_radius = m_measurement_result.angle->radius;

        if (calc_radius == 0.0)
            return;

        arc_edge_edge(Measure::SurfaceFeature(Measure::SurfaceFeatureType::Edge, e1.first, e1.second),
            Measure::SurfaceFeature(Measure::SurfaceFeatureType::Edge, e2.first, e2.second), calc_radius);
    };

    shader->start_using();

    if (!m_dimensioning.line.is_initialized()) {
        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        init_data.color = ColorRGBA::WHITE();
        init_data.reserve_vertices(2);
        init_data.reserve_indices(2);

        // vertices
        init_data.add_vertex(Vec3f(0.0f, 0.0f, 0.0f));
        init_data.add_vertex(Vec3f(1.0f, 0.0f, 0.0f));

        // indices
        init_data.add_line(0, 1);

        m_dimensioning.line.init_from(std::move(init_data));
    }

    if (!m_dimensioning.triangle.is_initialized()) {
        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        init_data.color = ColorRGBA::WHITE();
        init_data.reserve_vertices(3);
        init_data.reserve_indices(3);

        // vertices
        init_data.add_vertex(Vec3f(0.0f, 0.0f, 0.0f));
        init_data.add_vertex(Vec3f(-TRIANGLE_HEIGHT, 0.5f * TRIANGLE_BASE, 0.0f));
        init_data.add_vertex(Vec3f(-TRIANGLE_HEIGHT, -0.5f * TRIANGLE_BASE, 0.0f));

        // indices
        init_data.add_triangle(0, 1, 2);

        m_dimensioning.triangle.init_from(std::move(init_data));
    }

    if (last_selected_features != m_selected_features)
        m_dimensioning.arc.reset();

    glsafe(::glDisable(GL_DEPTH_TEST));

    if (m_selected_features.second.feature.has_value()) {
        const bool has_distance = m_measurement_result.has_distance_data();

        const Measure::SurfaceFeature* f1 = &(*m_selected_features.first.feature);
        const Measure::SurfaceFeature* f2 = &(*m_selected_features.second.feature);
        Measure::SurfaceFeatureType ft1 = f1->get_type();
        Measure::SurfaceFeatureType ft2 = f2->get_type();

        // Order features by type so following conditions are simple.
        if (ft1 > ft2) {
            std::swap(ft1, ft2);
            std::swap(f1, f2);
        }

        // If there is an angle to show, draw the arc:
        if (ft1 == Measure::SurfaceFeatureType::Edge && ft2 == Measure::SurfaceFeatureType::Edge)
            arc_edge_edge(*f1, *f2);
        else if (ft1 == Measure::SurfaceFeatureType::Edge && ft2 == Measure::SurfaceFeatureType::Plane)
            arc_edge_plane(*f1, *f2);
        else if (ft1 == Measure::SurfaceFeatureType::Plane && ft2 == Measure::SurfaceFeatureType::Plane)
            arc_plane_plane(*f1, *f2);

        if (has_distance){
            // Where needed, draw the extension of the edge to where the dist is measured:
            if (ft1 == Measure::SurfaceFeatureType::Point && ft2 == Measure::SurfaceFeatureType::Edge)
                point_edge(*f1, *f2);

            // Render the arrow between the points that the backend passed:
            const Measure::DistAndPoints& dap = m_measurement_result.distance_infinite.has_value()
                ? *m_measurement_result.distance_infinite
                : *m_measurement_result.distance_strict;
            point_point(dap.from, dap.to, dap.dist);
        }
    }
    
    glsafe(::glEnable(GL_DEPTH_TEST));

    shader->stop_using();
}

static void add_row_to_table(std::function<void(void)> col_1 = nullptr, std::function<void(void)> col_2 = nullptr)
{
    assert(col_1 != nullptr && col_2 != nullptr);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    col_1();
    ImGui::TableSetColumnIndex(1);
    col_2();
}

static void add_strings_row_to_table(ImGuiWrapper& imgui, const std::string& col_1, const ImVec4& col_1_color, const std::string& col_2, const ImVec4& col_2_color)
{
    add_row_to_table([&]() { imgui.text_colored(col_1_color, col_1); }, [&]() { imgui.text_colored(col_2_color, col_2); });
};

#if ENABLE_MEASURE_GIZMO_DEBUG
void GLGizmoMeasure::render_debug_dialog()
{
    auto add_feature_data = [this](const SelectedFeatures::Item& item) {
        add_strings_row_to_table(*m_imgui, "Type", ImGuiWrapper::COL_ORANGE_LIGHT, item.source, ImGui::GetStyleColorVec4(ImGuiCol_Text));
        switch (item.feature->get_type())
        {
        case Measure::SurfaceFeatureType::Point:
        {
            add_strings_row_to_table(*m_imgui, "m_pt1", ImGuiWrapper::COL_ORANGE_LIGHT, format_vec3(item.feature->get_point()), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            break;
        }
        case Measure::SurfaceFeatureType::Edge:
        {
            auto [from, to] = item.feature->get_edge();
            add_strings_row_to_table(*m_imgui, "m_pt1", ImGuiWrapper::COL_ORANGE_LIGHT, format_vec3(from), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            add_strings_row_to_table(*m_imgui, "m_pt2", ImGuiWrapper::COL_ORANGE_LIGHT, format_vec3(to), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            break;
        }
        case Measure::SurfaceFeatureType::Plane:
        {
            auto [idx, normal, origin] = item.feature->get_plane();
            add_strings_row_to_table(*m_imgui, "m_pt1", ImGuiWrapper::COL_ORANGE_LIGHT, format_vec3(normal), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            add_strings_row_to_table(*m_imgui, "m_pt2", ImGuiWrapper::COL_ORANGE_LIGHT, format_vec3(origin), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            add_strings_row_to_table(*m_imgui, "m_value", ImGuiWrapper::COL_ORANGE_LIGHT, format_double(idx), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            break;
        }
        case Measure::SurfaceFeatureType::Circle:
        {
            auto [center, radius, normal] = item.feature->get_circle();
            const Vec3d on_circle = center + radius * Measure::get_orthogonal(normal, true);
            radius = (on_circle - center).norm();
            add_strings_row_to_table(*m_imgui, "m_pt1", ImGuiWrapper::COL_ORANGE_LIGHT, format_vec3(center), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            add_strings_row_to_table(*m_imgui, "m_pt2", ImGuiWrapper::COL_ORANGE_LIGHT, format_vec3(normal), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            add_strings_row_to_table(*m_imgui, "m_value", ImGuiWrapper::COL_ORANGE_LIGHT, format_double(radius), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            break;
        }
        }
        std::optional<Vec3d> extra_point = item.feature->get_extra_point();
        if (extra_point.has_value())
            add_strings_row_to_table(*m_imgui, "m_pt3", ImGuiWrapper::COL_ORANGE_LIGHT, format_vec3(*extra_point), ImGui::GetStyleColorVec4(ImGuiCol_Text));
    };

    m_imgui->begin(_L("Measure tool debug"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    if (ImGui::BeginTable("Mode", 2)) {
        std::string txt;
        switch (m_mode)
        {
        case EMode::FeatureSelection: { txt = "Feature selection"; break; }
        case EMode::PointSelection:   { txt = "Point selection"; break; }
        case EMode::CenterSelection:  { txt = "Center selection"; break; }
        default:                      { assert(false); break; }
        }
        add_strings_row_to_table(*m_imgui, "Mode", ImGuiWrapper::COL_ORANGE_LIGHT, txt, ImGui::GetStyleColorVec4(ImGuiCol_Text));
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (ImGui::BeginTable("Hover", 2)) {
        add_strings_row_to_table(*m_imgui, "Hover id", ImGuiWrapper::COL_ORANGE_LIGHT, std::to_string(m_hover_id), ImGui::GetStyleColorVec4(ImGuiCol_Text));
        const std::string txt = m_curr_feature.has_value() ? surface_feature_type_as_string(m_curr_feature->get_type()) : "None";
        add_strings_row_to_table(*m_imgui, "Current feature", ImGuiWrapper::COL_ORANGE_LIGHT, txt, ImGui::GetStyleColorVec4(ImGuiCol_Text));
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (!m_selected_features.first.feature.has_value() && !m_selected_features.second.feature.has_value())
        m_imgui->text("Empty selection");
    else {
        const ImGuiTableFlags flags = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersH;
        if (m_selected_features.first.feature.has_value()) {
            m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, "Selection 1");
            if (ImGui::BeginTable("Selection 1", 2, flags)) {
                add_feature_data(m_selected_features.first);
                ImGui::EndTable();
            }
        }
        if (m_selected_features.second.feature.has_value()) {
            m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, "Selection 2");
            if (ImGui::BeginTable("Selection 2", 2, flags)) {
                add_feature_data(m_selected_features.second);
                ImGui::EndTable();
            }
        }
    }
    m_imgui->end();
}
#endif // ENABLE_MEASURE_GIZMO_DEBUG

void GLGizmoMeasure::on_render_input_window(float x, float y, float bottom_limit)
{
    static std::optional<Measure::SurfaceFeature> last_feature;
    static EMode last_mode = EMode::FeatureSelection;
    static SelectedFeatures last_selected_features;

    static float last_y = 0.0f;
    static float last_h = 0.0f;

    if (m_editing_distance)
        return;

    m_imgui->begin(get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    // adjust window position to avoid overlap the view toolbar
    const float win_h = ImGui::GetWindowHeight();
    y = std::min(y, bottom_limit - win_h);
    ImGui::SetWindowPos(ImVec2(x, y), ImGuiCond_Always);
    if (last_h != win_h || last_y != y) {
        // ask canvas for another frame to render the window in the correct position
        m_imgui->set_requires_extra_frame();
        if (last_h != win_h)
            last_h = win_h;
        if (last_y != y)
            last_y = y;
    }

    if (ImGui::BeginTable("Commands", 2)) {
        unsigned int row_count = 1;
        add_row_to_table(
            [this]() {
                m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, _u8L("Left mouse button"));
            },
            [this]() {
                std::string text;
                ColorRGBA color;
                if (m_selected_features.second.feature.has_value()) {
                    if (m_selected_features.first.feature == m_curr_feature && m_mode == EMode::FeatureSelection) {
                        text = _u8L("Unselect feature");
                        color = SELECTED_1ST_COLOR;
                    }
                    else if (m_selected_features.second.feature == m_curr_feature && m_mode == EMode::FeatureSelection) {
                        text = _u8L("Unselect feature");
                        color = SELECTED_2ND_COLOR;
                    }
                    else if (m_hover_id == SELECTION_1_ID) {
                        text = (m_mode == EMode::CenterSelection) ? _u8L("Unselect center") : _u8L("Unselect point");
                        color = SELECTED_1ST_COLOR;
                    }
                    else if (m_hover_id == SELECTION_2_ID) {
                        text = (m_mode == EMode::CenterSelection) ? _u8L("Unselect center") : _u8L("Unselect point");
                        color = SELECTED_2ND_COLOR;
                    }
                    else {
                        text = (m_mode == EMode::PointSelection) ? _u8L("Select point") :
                            (m_mode == EMode::CenterSelection) ? _u8L("Select center") : _u8L("Select feature");
                        color = SELECTED_2ND_COLOR;
                    }
                }
                else {
                    if (m_selected_features.first.feature.has_value()) {
                        if (m_selected_features.first.feature == m_curr_feature && m_mode == EMode::FeatureSelection) {
                            text = _u8L("Unselect feature");
                            color = SELECTED_1ST_COLOR;
                        }
                        else if (m_hover_id == SELECTION_1_ID) {
                            text = (m_mode == EMode::CenterSelection) ? _u8L("Unselect center") : _u8L("Unselect point");
                            color = SELECTED_1ST_COLOR;
                        }
                    }
                    if (text.empty()) {
                        text = (m_mode == EMode::PointSelection) ? _u8L("Select point") :
                            (m_mode == EMode::CenterSelection) ? _u8L("Select center") : _u8L("Select feature");
                        color = m_selected_features.first.feature.has_value() ? SELECTED_2ND_COLOR : SELECTED_1ST_COLOR;
                    }
                }

                m_imgui->text_colored(ImGui::GetStyleColorVec4(ImGuiCol_Text), text);
                ImGui::SameLine();
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                const float rect_size = ImGui::GetTextLineHeight();
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x + 1.0f, pos.y + 1.0f), ImVec2(pos.x + rect_size, pos.y + rect_size), ImGuiWrapper::to_ImU32(color));
                ImGui::Dummy(ImVec2(rect_size, rect_size));
            }
            );

        if (m_mode == EMode::FeatureSelection && m_hover_id != -1) {
            add_strings_row_to_table(*m_imgui, _u8L("Shift"), ImGuiWrapper::COL_ORANGE_LIGHT, _u8L("Enable point selection"), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            ++row_count;
        }

        if (m_mode != EMode::CenterSelection && feature_has_center(m_curr_feature)) {
            add_strings_row_to_table(*m_imgui, _u8L("Shift") + "+" + CTRL_STR, ImGuiWrapper::COL_ORANGE_LIGHT, _u8L("Enable center selection"), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            ++row_count;
        }

        if (m_selected_features.first.feature.has_value()) {
            add_strings_row_to_table(*m_imgui, _u8L("Delete"), ImGuiWrapper::COL_ORANGE_LIGHT, _u8L("Restart selection"), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            ++row_count;
        }

        if (m_selected_features.first.feature.has_value() || m_selected_features.second.feature.has_value()) {
          add_row_to_table(
            [this]() {
                m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, _u8L("Esc"));
            },
            [this]() {
                m_imgui->text_colored(ImGui::GetStyleColorVec4(ImGuiCol_Text), _u8L("Unselect"));
                ImGui::SameLine();
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                const float rect_size = ImGui::GetTextLineHeight();
                const ColorRGBA color = m_selected_features.second.feature.has_value() ? SELECTED_2ND_COLOR : SELECTED_1ST_COLOR;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x + 1.0f, pos.y + 1.0f), ImVec2(pos.x + rect_size, pos.y + rect_size), ImGuiWrapper::to_ImU32(color));
                ImGui::Dummy(ImVec2(rect_size, rect_size));
            }
            );
          
          ++row_count;
        }

        // add dummy rows to keep dialog size fixed
        for (unsigned int i = row_count; i < 5; ++i) {
            add_strings_row_to_table(*m_imgui, " ", ImGuiWrapper::COL_ORANGE_LIGHT, " ", ImGui::GetStyleColorVec4(ImGuiCol_Text));
        }

        ImGui::EndTable();
    }

    const bool use_inches = wxGetApp().app_config->get("use_inches") == "1";
    const std::string units = use_inches ? " " + _u8L("in") : " " + _u8L("mm");

    ImGui::Separator();
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersH;
    if (ImGui::BeginTable("Selection", 2, flags)) {
        auto format_item_text = [use_inches, &units](const SelectedFeatures::Item& item) {
          std::string txt = item.feature.has_value() ? item.source : _u8L("None");
          if (item.feature.has_value() && item.feature->get_type() == Measure::SurfaceFeatureType::Circle) {
              auto [center, radius, normal] = item.feature->get_circle();
              const Vec3d on_circle = center + radius * Measure::get_orthogonal(normal, true);
              radius = (on_circle - center).norm();
              if (use_inches)
                  radius = ObjectManipulation::mm_to_in * radius;
              txt += " (" + _u8L("Diameter:") + " " + format_double(2.0 * radius) + units + ")";
          }
          return txt;
        };

        add_strings_row_to_table(*m_imgui, _u8L("Selection") + " 1:", ImGuiWrapper::to_ImVec4(SELECTED_1ST_COLOR), format_item_text(m_selected_features.first),
          ImGuiWrapper::to_ImVec4(SELECTED_1ST_COLOR));
        add_strings_row_to_table(*m_imgui, _u8L("Selection") + " 2:", ImGuiWrapper::to_ImVec4(SELECTED_2ND_COLOR), format_item_text(m_selected_features.second),
          ImGuiWrapper::to_ImVec4(SELECTED_2ND_COLOR));
        ImGui::EndTable();
    }

    m_imgui->disabled_begin(!m_selected_features.first.feature.has_value());
        if (m_imgui->button(_u8L("Restart selection"))) {
            m_selected_features.reset();
            m_selection_raycasters.clear();
            m_imgui->set_requires_extra_frame();
        }
    m_imgui->disabled_end();

    auto add_measure_row_to_table = [this](const std::string& col_1, const ImVec4& col_1_color, const std::string& col_2, const ImVec4& col_2_color) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        m_imgui->text_colored(col_1_color, col_1);
        ImGui::TableSetColumnIndex(1);
        m_imgui->text_colored(col_2_color, col_2);
        ImGui::TableSetColumnIndex(2);
        if (m_imgui->image_button(ImGui::ClipboardBtnIcon, _L("Copy to clipboard"))) {
            wxTheClipboard->Open();
            wxTheClipboard->SetData(new wxTextDataObject(col_1 + ": " + col_2));
            wxTheClipboard->Close();
        }
    };

    ImGui::Separator();
    m_imgui->text(_u8L("Measure"));

    const unsigned int max_measure_row_count = 2;
    unsigned int measure_row_count = 0;
    if (ImGui::BeginTable("Measure", 4)) {
        if (m_selected_features.second.feature.has_value()) {
            const Measure::MeasurementResult& measure = m_measurement_result;
            if (measure.angle.has_value()) {
                ImGui::PushID("ClipboardAngle");
                add_measure_row_to_table(_u8L("Angle"), ImGuiWrapper::COL_ORANGE_LIGHT, format_double(Geometry::rad2deg(measure.angle->angle)) + "°",
                    ImGui::GetStyleColorVec4(ImGuiCol_Text));
                ++measure_row_count;
                ImGui::PopID();
            }
            if (measure.distance_infinite.has_value()) {
                double distance = measure.distance_infinite->dist;
                if (use_inches)
                    distance = ObjectManipulation::mm_to_in * distance;
                ImGui::PushID("ClipboardDistanceInfinite");
                add_measure_row_to_table(_u8L("Distance Infinite"), ImGuiWrapper::COL_ORANGE_LIGHT, format_double(distance) + units,
                    ImGui::GetStyleColorVec4(ImGuiCol_Text));
                ++measure_row_count;
                ImGui::PopID();
            }
            if (measure.distance_strict.has_value() &&
                (!measure.distance_infinite.has_value() || std::abs(measure.distance_strict->dist - measure.distance_infinite->dist) > EPSILON)) {
                double distance = measure.distance_strict->dist;
                if (use_inches)
                    distance = ObjectManipulation::mm_to_in * distance;
                ImGui::PushID("ClipboardDistanceStrict");
                add_measure_row_to_table(_u8L("Distance Strict"), ImGuiWrapper::COL_ORANGE_LIGHT, format_double(distance) + units,
                    ImGui::GetStyleColorVec4(ImGuiCol_Text));
                ++measure_row_count;
                ImGui::PopID();
            }
            if (measure.distance_xyz.has_value() && measure.distance_xyz->norm() > EPSILON) {
                Vec3d distance = *measure.distance_xyz;
                if (use_inches)
                    distance = ObjectManipulation::mm_to_in * distance;
                ImGui::PushID("ClipboardDistanceXYZ");
                add_measure_row_to_table(_u8L("Distance XYZ"), ImGuiWrapper::COL_ORANGE_LIGHT, format_vec3(distance),
                    ImGui::GetStyleColorVec4(ImGuiCol_Text));
                ++measure_row_count;
                ImGui::PopID();
            }
        }

        // add dummy rows to keep dialog size fixed
        for (unsigned int i = measure_row_count; i < max_measure_row_count; ++i) {
            add_strings_row_to_table(*m_imgui, " ", ImGuiWrapper::COL_ORANGE_LIGHT, " ", ImGui::GetStyleColorVec4(ImGuiCol_Text));
        }
        ImGui::EndTable();
    }

    if (last_feature != m_curr_feature || last_mode != m_mode || last_selected_features != m_selected_features) {
        // the dialog may have changed its size, ask for an extra frame to render it properly
        last_feature = m_curr_feature;
        last_mode = m_mode;
        last_selected_features = m_selected_features;
        m_imgui->set_requires_extra_frame();
    }

    m_imgui->end();
}

void GLGizmoMeasure::on_register_raycasters_for_picking()
{
    // the features are rendered on top of the scene, so the raytraced picker should take it into account
    m_parent.set_raycaster_gizmos_on_top(true);
}

void GLGizmoMeasure::on_unregister_raycasters_for_picking()
{
    m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo);
    m_parent.set_raycaster_gizmos_on_top(false);
    m_raycasters.clear();
    m_selection_raycasters.clear();
}

} // namespace GUI
} // namespace Slic3r
