#include "GLGizmoMmuSegmentation.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/BitmapCache.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Model.hpp"


#include <GL/glew.h>

namespace Slic3r::GUI {

void GLGizmoMmuSegmentation::on_shutdown()
{
    m_parent.use_slope(false);
}

std::string GLGizmoMmuSegmentation::on_get_name() const
{
    // FIXME Lukas H.: Discuss and change shortcut
    return (_L("MMU painting") + " [N]").ToUTF8().data();
}

bool GLGizmoMmuSegmentation::on_is_selectable() const
{
    return (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF
            && wxGetApp().get_mode() != comSimple && wxGetApp().extruders_edited_cnt() > 1);
}

static std::vector<std::array<float, 4>> get_extruders_colors()
{
    unsigned char                     rgb_color[3] = {};
    std::vector<std::string>          colors       = Slic3r::GUI::wxGetApp().plater()->get_extruder_colors_from_plater_config();
    std::vector<std::array<float, 4>> colors_out(colors.size());
    for (const std::string &color : colors) {
        Slic3r::GUI::BitmapCache::parse_color(color, rgb_color);
        size_t color_idx      = &color - &colors.front();
        colors_out[color_idx] = {float(rgb_color[0]) / 255.f, float(rgb_color[1]) / 255.f, float(rgb_color[2]) / 255.f, 1.f};
    }

    return colors_out;
}

static std::vector<std::string> get_extruders_names()
{
    size_t                   extruders_count = wxGetApp().extruders_edited_cnt();
    std::vector<std::string> extruders_out;
    extruders_out.reserve(extruders_count);
    for (size_t extruder_idx = 1; extruder_idx <= extruders_count; ++extruder_idx)
        extruders_out.emplace_back("Extruder " + std::to_string(extruder_idx));

    return extruders_out;
}

static std::vector<int> get_extruder_id_for_volumes(const ModelObject &model_object)
{
    std::vector<int> extruders_idx;
    extruders_idx.reserve(model_object.volumes.size());
    for (const ModelVolume *model_volume : model_object.volumes) {
        if (!model_volume->is_model_part())
            continue;

        extruders_idx.emplace_back(model_volume->extruder_id());
    }

    return extruders_idx;
}

void GLGizmoMmuSegmentation::init_extruders_data()
{
    m_original_extruders_names     = get_extruders_names();
    m_original_extruders_colors    = get_extruders_colors();
    m_modified_extruders_colors    = m_original_extruders_colors;
    m_first_selected_extruder_idx  = 0;
    m_second_selected_extruder_idx = 1;
}

bool GLGizmoMmuSegmentation::on_init()
{
    // FIXME Lukas H.: Discuss and change shortcut
    m_shortcut_key = WXK_CONTROL_N;

    m_desc["reset_direction"]      = _L("Reset direction");
    m_desc["clipping_of_view"]     = _L("Clipping of view") + ": ";
    m_desc["cursor_size"]          = _L("Brush size") + ": ";
    m_desc["cursor_type"]          = _L("Brush shape") + ": ";
    m_desc["first_color_caption"]  = _L("Left mouse button") + ": ";
    m_desc["first_color"]          = _L("First color");
    m_desc["second_color_caption"] = _L("Right mouse button") + ": ";
    m_desc["second_color"]         = _L("Second color");
    m_desc["remove_caption"]       = _L("Shift + Left mouse button") + ": ";
    m_desc["remove"]               = _L("Remove painted color");
    m_desc["remove_all"]           = _L("Remove all painted colors");
    m_desc["circle"]               = _L("Circle");
    m_desc["sphere"]               = _L("Sphere");
    m_desc["seed_fill_angle"]      = _L("Seed fill angle");

    init_extruders_data();

    return true;
}

void GLGizmoMmuSegmentation::render_painter_gizmo() const
{
    const Selection& selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    render_triangles(selection, false);

    m_c->object_clipper()->render_cut();
    render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

void GLGizmoMmuSegmentation::set_painter_gizmo_data(const Selection &selection)
{
    GLGizmoPainterBase::set_painter_gizmo_data(selection);

    if (m_state != On || wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptFFF || wxGetApp().extruders_edited_cnt() <= 1)
        return;

    ModelObject *model_object         = m_c->selection_info()->model_object();
    int          prev_extruders_count = int(m_original_extruders_colors.size());
    if (prev_extruders_count != wxGetApp().extruders_edited_cnt() || get_extruders_colors() != m_original_extruders_colors) {
        this->init_extruders_data();
        // Reinitialize triangle selectors because of change of extruder count need also change the size of GLIndexedVertexArray
        if (prev_extruders_count != wxGetApp().extruders_edited_cnt())
            this->init_model_triangle_selectors();
    } else if (model_object != nullptr && get_extruder_id_for_volumes(*model_object) != m_original_volumes_extruder_idxs) {
        this->init_model_triangle_selectors();
    }
}

static void render_extruders_combo(const std::string                       &label,
                                   const std::vector<std::string>          &extruders,
                                   const std::vector<std::array<float, 4>> &extruders_colors,
                                   size_t                                  &selection_idx)
{
    assert(!extruders_colors.empty());
    assert(extruders_colors.size() == extruders_colors.size());

    auto convert_to_imu32 = [](const std::array<float, 4> &color) -> ImU32 {
        return IM_COL32(uint8_t(color[0] * 255.f), uint8_t(color[1] * 255.f), uint8_t(color[2] * 255.f), uint8_t(color[3] * 255.f));
    };

    size_t selection_out = selection_idx;
    // It is necessary to use BeginGroup(). Otherwise, when using SameLine() is called, then other items will be drawn inside the combobox.
    ImGui::BeginGroup();
    ImVec2 combo_pos = ImGui::GetCursorScreenPos();
    if (ImGui::BeginCombo(label.c_str(), "")) {
        for (size_t extruder_idx = 0; extruder_idx < extruders.size(); ++extruder_idx) {
            ImGui::PushID(int(extruder_idx));
            ImVec2 start_position = ImGui::GetCursorScreenPos();

            if (ImGui::Selectable("", extruder_idx == selection_idx))
                selection_out = extruder_idx;

            ImGui::SameLine();
            ImGuiStyle &style  = ImGui::GetStyle();
            float       height = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(start_position, ImVec2(start_position.x + height + height / 2, start_position.y + height), convert_to_imu32(extruders_colors[extruder_idx]));
            ImGui::GetWindowDrawList()->AddRect(start_position, ImVec2(start_position.x + height + height / 2, start_position.y + height), IM_COL32_BLACK);

            ImGui::SetCursorScreenPos(ImVec2(start_position.x + height + height / 2 + style.FramePadding.x, start_position.y));
            ImGui::Text("%s", extruders[extruder_idx].c_str());
            ImGui::PopID();
        }

        ImGui::EndCombo();
    }

    ImVec2      backup_pos = ImGui::GetCursorScreenPos();
    ImGuiStyle &style      = ImGui::GetStyle();

    ImGui::SetCursorScreenPos(ImVec2(combo_pos.x + style.FramePadding.x, combo_pos.y + style.FramePadding.y));
    ImVec2 p      = ImGui::GetCursorScreenPos();
    float  height = ImGui::GetTextLineHeight();

    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + height + height / 2, p.y + height), convert_to_imu32(extruders_colors[selection_idx]));
    ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + height + height / 2, p.y + height), IM_COL32_BLACK);

    ImGui::SetCursorScreenPos(ImVec2(p.x + height + height / 2 + style.FramePadding.x, p.y));
    ImGui::Text("%s", extruders[selection_out].c_str());
    ImGui::SetCursorScreenPos(backup_pos);
    ImGui::EndGroup();

    selection_idx = selection_out;
}

void GLGizmoMmuSegmentation::on_render_input_window(float x, float y, float bottom_limit)
{
    if (!m_c->selection_info()->model_object())
        return;

    const float approx_height = m_imgui->scaled(23.0f);
                            y = std::min(y, bottom_limit - approx_height);
    m_imgui->set_next_window_pos(x, y, ImGuiCond_Always);

    m_imgui->begin(on_get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

    // First calculate width of all the texts that are could possibly be shown. We will decide set the dialog width based on that:
    const float clipping_slider_left = std::max(m_imgui->calc_text_size(m_desc.at("clipping_of_view")).x,
                                                m_imgui->calc_text_size(m_desc.at("reset_direction")).x) + m_imgui->scaled(1.5f);
    const float cursor_slider_left       = m_imgui->calc_text_size(m_desc.at("cursor_size")).x + m_imgui->scaled(1.f);
    const float autoset_slider_left      = m_imgui->calc_text_size(m_desc.at("seed_fill_angle")).x + m_imgui->scaled(1.f);
    const float cursor_type_radio_left   = m_imgui->calc_text_size(m_desc.at("cursor_type")).x + m_imgui->scaled(1.f);
    const float cursor_type_radio_width1 = m_imgui->calc_text_size(m_desc["circle"]).x + m_imgui->scaled(2.5f);
    const float cursor_type_radio_width2 = m_imgui->calc_text_size(m_desc["sphere"]).x + m_imgui->scaled(2.5f);
    const float button_width             = m_imgui->calc_text_size(m_desc.at("remove_all")).x + m_imgui->scaled(1.f);
    const float buttons_width            = m_imgui->scaled(0.5f);
    const float minimal_slider_width     = m_imgui->scaled(4.f);
    const float color_button_width       = m_imgui->calc_text_size("").x + m_imgui->scaled(1.75f);
    const float combo_label_width        = std::max(m_imgui->calc_text_size(m_desc.at("first_color")).x,
                                                    m_imgui->calc_text_size(m_desc.at("second_color")).x) + m_imgui->scaled(1.f);

    float caption_max    = 0.f;
    float total_text_max = 0.;
    for (const auto &t : std::array<std::string, 3>{"first_color", "second_color", "remove"}) {
        caption_max    = std::max(caption_max, m_imgui->calc_text_size(m_desc.at(t + "_caption")).x);
        total_text_max = std::max(total_text_max, caption_max + m_imgui->calc_text_size(m_desc.at(t)).x);
    }
    caption_max += m_imgui->scaled(1.f);
    total_text_max += m_imgui->scaled(1.f);

    float window_width = minimal_slider_width + std::max(autoset_slider_left, std::max(cursor_slider_left, clipping_slider_left));
    window_width       = std::max(window_width, total_text_max);
    window_width       = std::max(window_width, button_width);
    window_width       = std::max(window_width, cursor_type_radio_left + cursor_type_radio_width1 + cursor_type_radio_width2);
    window_width       = std::max(window_width, 2.f * buttons_width + m_imgui->scaled(1.f));

    auto draw_text_with_caption = [this, &caption_max](const wxString &caption, const wxString &text) {
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, caption);
        ImGui::SameLine(caption_max);
        m_imgui->text(text);
    };

    for (const auto &t : std::array<std::string, 3>{"first_color", "second_color", "remove"})
        draw_text_with_caption(m_desc.at(t + "_caption"), m_desc.at(t));

    m_imgui->text("");
    ImGui::Separator();

    m_imgui->text(m_desc.at("first_color"));
    ImGui::SameLine(combo_label_width);
    ImGui::PushItemWidth(window_width - combo_label_width - color_button_width);
    render_extruders_combo("##first_color_combo", m_original_extruders_names, m_original_extruders_colors, m_first_selected_extruder_idx);
    ImGui::SameLine();

    const std::array<float, 4> &select_first_color = m_modified_extruders_colors[m_first_selected_extruder_idx];
    ImVec4                      first_color        = ImVec4(select_first_color[0], select_first_color[1], select_first_color[2], select_first_color[3]);
    if(ImGui::ColorEdit4("First color##color_picker", (float*)&first_color, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
        m_modified_extruders_colors[m_first_selected_extruder_idx] = {first_color.x, first_color.y, first_color.z, first_color.w};

    m_imgui->text(m_desc.at("second_color"));
    ImGui::SameLine(combo_label_width);
    ImGui::PushItemWidth(window_width - combo_label_width - color_button_width);
    render_extruders_combo("##second_color_combo", m_original_extruders_names, m_original_extruders_colors, m_second_selected_extruder_idx);
    ImGui::SameLine();

    const std::array<float, 4> &select_second_color = m_modified_extruders_colors[m_second_selected_extruder_idx];
    ImVec4                      second_color        = ImVec4(select_second_color[0], select_second_color[1], select_second_color[2], select_second_color[3]);
    if(ImGui::ColorEdit4("Second color##color_picker", (float*)&second_color, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
        m_modified_extruders_colors[m_second_selected_extruder_idx] = {second_color.x, second_color.y, second_color.z, second_color.w};

    ImGui::Separator();

    if (m_imgui->checkbox(_L("Seed fill"), m_seed_fill_enabled))
        if (!m_seed_fill_enabled)
            for (auto &triangle_selector : m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }

    m_imgui->text(m_desc["seed_fill_angle"] + ":");
    ImGui::AlignTextToFramePadding();
    std::string format_str = std::string("%.f") + I18N::translate_utf8("°", "Degree sign to use in the respective slider in FDM supports gizmo,"
                                                                            "placed after the number with no whitespace in between.");
    ImGui::SameLine(autoset_slider_left);
    ImGui::PushItemWidth(window_width - autoset_slider_left);
    m_imgui->disabled_begin(!m_seed_fill_enabled);
    m_imgui->slider_float("##seed_fill_angle", &m_seed_fill_angle, 0.f, 90.f, format_str.data());
    m_imgui->disabled_end();

    ImGui::Separator();

    if (m_imgui->button(m_desc.at("remove_all"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), wxString(_L("Reset selection")));
        ModelObject *mo  = m_c->selection_info()->model_object();
        int          idx = -1;
        for (ModelVolume *mv : mo->volumes) {
            if (mv->is_model_part()) {
                ++idx;
                m_triangle_selectors[idx]->reset();
                m_triangle_selectors[idx]->request_update_render_data();
            }
        }

        update_model_object();
        m_parent.set_as_dirty();
    }

    const float max_tooltip_width = ImGui::GetFontSize() * 20.0f;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("cursor_size"));
    ImGui::SameLine(cursor_slider_left);
    ImGui::PushItemWidth(window_width - cursor_slider_left);
    ImGui::SliderFloat(" ", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(max_tooltip_width);
        ImGui::TextUnformatted(_L("Alt + Mouse wheel").ToUTF8().data());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("cursor_type"));
    ImGui::SameLine(cursor_type_radio_left + m_imgui->scaled(0.f));
    ImGui::PushItemWidth(cursor_type_radio_width1);

    bool sphere_sel = m_cursor_type == TriangleSelector::CursorType::SPHERE;
    if (m_imgui->radio_button(m_desc["sphere"], sphere_sel))
        sphere_sel = true;

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(max_tooltip_width);
        ImGui::TextUnformatted(_L("Paints all facets inside, regardless of their orientation.").ToUTF8().data());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    ImGui::SameLine(cursor_type_radio_left + cursor_type_radio_width2 + m_imgui->scaled(0.f));
    ImGui::PushItemWidth(cursor_type_radio_width2);

    if (m_imgui->radio_button(m_desc["circle"], !sphere_sel))
        sphere_sel = false;

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(max_tooltip_width);
        ImGui::TextUnformatted(_L("Ignores facets facing away from the camera.").ToUTF8().data());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    m_cursor_type = sphere_sel ? TriangleSelector::CursorType::SPHERE : TriangleSelector::CursorType::CIRCLE;

    m_imgui->checkbox(_L("Split triangles"), m_triangle_splitting_enabled);

    ImGui::Separator();
    if (m_c->object_clipper()->get_position() == 0.f) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("clipping_of_view"));
    } else {
        if (m_imgui->button(m_desc.at("reset_direction"))) {
            wxGetApp().CallAfter([this]() { m_c->object_clipper()->set_position(-1., false); });
        }
    }

    ImGui::SameLine(clipping_slider_left);
    ImGui::PushItemWidth(window_width - clipping_slider_left);
    auto clp_dist = float(m_c->object_clipper()->get_position());
    if (ImGui::SliderFloat("  ", &clp_dist, 0.f, 1.f, "%.2f"))
        m_c->object_clipper()->set_position(clp_dist, true);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(max_tooltip_width);
        ImGui::TextUnformatted(_L("Ctrl + Mouse wheel").ToUTF8().data());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    m_imgui->end();
}

void GLGizmoMmuSegmentation::update_model_object() const
{
    bool updated = false;
    ModelObject* mo = m_c->selection_info()->model_object();
    int idx = -1;
    for (ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;
        ++idx;
        updated |= mv->mmu_segmentation_facets.set(*m_triangle_selectors[idx].get());
    }

    if (updated) {
        const ModelObjectPtrs &mos = wxGetApp().model().objects;
        wxGetApp().obj_list()->update_info_items(std::find(mos.begin(), mos.end(), mo) - mos.begin());
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
}

void GLGizmoMmuSegmentation::init_model_triangle_selectors()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();

    // Don't continue when extruders colors are not initialized
    if(m_original_extruders_colors.empty())
        return;

    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;

        // This mesh does not account for the possible Z up SLA offset.
        const TriangleMesh *mesh = &mv->mesh();

        int extruder_idx = (mv->extruder_id() > 0) ? mv->extruder_id() - 1 : 0;
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorMmGui>(*mesh, m_modified_extruders_colors, m_original_extruders_colors[size_t(extruder_idx)]));
        m_triangle_selectors.back()->deserialize(mv->mmu_segmentation_facets.get_data());
        m_triangle_selectors.back()->request_update_render_data();
    }
    m_original_volumes_extruder_idxs = get_extruder_id_for_volumes(*mo);
}

void GLGizmoMmuSegmentation::update_from_model_object()
{
    wxBusyCursor wait;
    this->init_model_triangle_selectors();
}

PainterGizmoType GLGizmoMmuSegmentation::get_painter_type() const
{
    return PainterGizmoType::MMU_SEGMENTATION;
}

std::array<float, 4> GLGizmoMmuSegmentation::get_cursor_sphere_left_button_color() const
{
    const std::array<float, 4> &color = m_modified_extruders_colors[m_first_selected_extruder_idx];
    return {color[0], color[1], color[2], 0.25f};
}

std::array<float, 4> GLGizmoMmuSegmentation::get_cursor_sphere_right_button_color() const
{
    const std::array<float, 4> &color = m_modified_extruders_colors[m_second_selected_extruder_idx];
    return {color[0], color[1], color[2], 0.25f};
}

void TriangleSelectorMmGui::render(ImGuiWrapper *imgui)
{
    static constexpr std::array<float, 4> seed_fill_color{0.f, 1.f, 0.44f, 1.f};

    if (m_update_render_data)
        update_render_data();

    auto *shader = wxGetApp().get_current_shader();
    if (!shader)
        return;
    assert(shader->get_name() == "gouraud");

    for (size_t i = 0; i <= m_iva_colors.size(); ++i) {
        GLIndexedVertexArray &iva = i == m_iva_colors.size() ? m_iva_seed_fill : m_iva_colors[i];
        if (!iva.vertices_and_normals_interleaved.empty() && m_update_render_data) {
            iva.vertices_and_normals_interleaved_size = iva.vertices_and_normals_interleaved.size();
            iva.triangle_indices.assign(iva.vertices_and_normals_interleaved_size / 6, 0);
            std::iota(iva.triangle_indices.begin(), iva.triangle_indices.end(), 0);
            iva.triangle_indices_size = iva.triangle_indices.size();
            iva.finalize_geometry(true);
        }
        if (iva.has_VBOs()) {
            shader->set_uniform("uniform_color", (i == 0) ? m_default_volume_color : i == m_iva_colors.size() ? seed_fill_color : m_colors[i - 1]);
            iva.render();
        }
    }

    m_update_render_data = false;
}

void TriangleSelectorMmGui::update_render_data()
{
    for (auto &iva_color : m_iva_colors)
        iva_color.release_geometry();
    m_iva_seed_fill.release_geometry();

    for (const Triangle &tr : m_triangles)
        if (tr.valid() && ! tr.is_split()) {
            GLIndexedVertexArray *iva = nullptr;
            if (tr.is_selected_by_seed_fill())
                iva = &m_iva_seed_fill;
            else if (int color = int(tr.get_state()); color < int(m_iva_colors.size()))
                iva = &m_iva_colors[color];
            if (iva) {
                if (iva->vertices_and_normals_interleaved.size() + 18 > iva->vertices_and_normals_interleaved.capacity())
                    iva->vertices_and_normals_interleaved.reserve(next_highest_power_of_2(iva->vertices_and_normals_interleaved.size() + 18));
                const Vec3f &n = m_mesh->stl.facet_start[tr.source_triangle].normal;
                for (int i = 0; i < 3; ++ i) {
                    const Vec3f &v = m_vertices[tr.verts_idxs[i]].v;
                    iva->vertices_and_normals_interleaved.emplace_back(n.x());
                    iva->vertices_and_normals_interleaved.emplace_back(n.y());
                    iva->vertices_and_normals_interleaved.emplace_back(n.z());
                    iva->vertices_and_normals_interleaved.emplace_back(v.x());
                    iva->vertices_and_normals_interleaved.emplace_back(v.y());
                    iva->vertices_and_normals_interleaved.emplace_back(v.z());
                }
            }
        }
}

wxString GLGizmoMmuSegmentation::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    wxString action_name;
    if (shift_down)
        action_name = _L("Remove painted color");
    else {
        size_t extruder_id = (button_down == Button::Left ? m_first_selected_extruder_idx : m_second_selected_extruder_idx) + 1;
        action_name        = GUI::format(_L("Painted using: Extruder %1%"), extruder_id);
    }
    return action_name;
}

} // namespace Slic3r
