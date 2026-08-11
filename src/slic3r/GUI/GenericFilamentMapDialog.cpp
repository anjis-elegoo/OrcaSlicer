#include "GenericFilamentMapDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"

namespace Slic3r { namespace GUI {

static FilamentMapMode get_applied_mode(DynamicConfig &config, const PartPlate *plate)
{
    return plate->get_real_filament_map_mode(config);
}

static std::vector<int> get_applied_map(DynamicConfig &config, const PartPlate *plate)
{
    return plate->get_real_filament_maps(config);
}

bool try_pop_up_generic_before_slice(bool is_slice_all, Plater *plater, PartPlate *plate, bool force_pop_up)
{
    auto         full_config = wxGetApp().preset_bundle->full_config();
    const size_t nozzle_count = full_config.option<ConfigOptionFloats>("nozzle_diameter")->size();
    if (nozzle_count <= 1 || !is_multiple_filaments_per_nozzle_enabled(full_config))
        return true;

    std::vector<std::string> filament_colors = full_config.option<ConfigOptionStrings>("filament_colour")->values;
    std::vector<std::string> filament_types  = full_config.option<ConfigOptionStrings>("filament_type")->values;
    FilamentMapMode          applied_mode    = get_applied_mode(full_config, plate);
    std::vector<int>         applied_maps    = get_applied_map(full_config, plate);
    applied_maps.resize(filament_colors.size(), 1);
    const bool auto_grouping_allowed = !has_different_nozzle_diameters(full_config);
    if (!auto_grouping_allowed && applied_mode != fmmManual && applied_mode != fmmNozzleManual)
        applied_mode = fmmManual;

    if (!force_pop_up && applied_mode != fmmManual && applied_mode != fmmNozzleManual)
        return true;

    std::vector<int> filament_list;
    if (is_slice_all) {
        filament_list.resize(filament_colors.size());
        std::iota(filament_list.begin(), filament_list.end(), 1);
    } else {
        filament_list = plate->get_extruders();
    }

    GenericFilamentMapDialog dialog(plater, filament_colors, filament_types, applied_maps,
                                    filament_list, applied_mode, nozzle_count, auto_grouping_allowed);
    if (dialog.ShowModal() != wxID_OK)
        return false;

    const FilamentMapMode new_mode = dialog.get_mode();
    const std::vector<int> new_maps = dialog.get_filament_maps();
    if (is_slice_all) {
        for (PartPlate *item : plater->get_partplate_list().get_plate_list()) {
            item->set_filament_map_mode(new_mode);
            if (new_mode == fmmManual)
                item->set_filament_maps(new_maps);
        }
    } else {
        plate->set_filament_map_mode(new_mode);
        if (new_mode == fmmManual)
            plate->set_filament_maps(new_maps);
    }
    plater->update();
    return true;
}

GenericFilamentMapDialog::GenericFilamentMapDialog(wxWindow                       *parent,
                                                   const std::vector<std::string> &filament_color,
                                                   const std::vector<std::string> &filament_type,
                                                   const std::vector<int>         &filament_map,
                                                   const std::vector<int>         &filaments,
                                                   FilamentMapMode                 mode,
                                                   size_t                          nozzle_count,
                                                   bool                            auto_grouping_allowed)
    : wxDialog(parent, wxID_ANY, _L("Filament grouping"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_auto_grouping_allowed(auto_grouping_allowed)
    , m_page_type(!auto_grouping_allowed || mode == fmmManual || mode == fmmNozzleManual ? ptManual : ptAuto)
    , m_filament_map(filament_map)
{
    SetBackgroundColour(*wxWHITE);
    SetMinSize(wxSize(FromDIP(580), -1));
    SetMaxSize(wxSize(FromDIP(580), -1));

    auto *main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->AddSpacer(FromDIP(22));

    auto *mode_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_auto_btn        = new CapsuleButton(this, ptAuto, _L("Fila Saving"), false);
    m_manual_btn      = new CapsuleButton(this, ptManual, _L("Custom"), false);
    if (!m_auto_grouping_allowed) {
        m_auto_btn->Enable(false);
        m_auto_btn->SetToolTip(_L("Automatic grouping is unavailable when nozzle diameters differ."));
    }
    mode_sizer->AddStretchSpacer();
    mode_sizer->Add(m_auto_btn, 1, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(2));
    mode_sizer->Add(m_manual_btn, 1, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(2));
    mode_sizer->AddStretchSpacer();
    main_sizer->Add(mode_sizer, 0, wxEXPAND);
    main_sizer->AddSpacer(FromDIP(24));

    m_manual_panel = new GenericFilamentMapManualPanel(this, filament_color, filament_type, filaments, filament_map, nozzle_count);
    m_auto_panel   = new GenericFilamentMapAutoPanel(this);
    auto *panel_sizer = new wxBoxSizer(wxHORIZONTAL);
    panel_sizer->Add(m_manual_panel, 0, wxEXPAND);
    panel_sizer->Add(m_auto_panel, 0, wxEXPAND);
    main_sizer->Add(panel_sizer, 0, wxEXPAND);

    auto *bottom_panel = new wxPanel(this);
    bottom_panel->SetBackgroundColour(*wxWHITE);
    auto *bottom_sizer = new wxBoxSizer(wxHORIZONTAL);
    bottom_panel->SetSizer(bottom_sizer);
    bottom_sizer->AddStretchSpacer();
    auto *dialog_buttons = new DialogButtons(bottom_panel, {"OK", "Cancel"});
    m_ok_btn             = dialog_buttons->GetOK();
    m_cancel_btn         = dialog_buttons->GetCANCEL();
    bottom_sizer->Add(dialog_buttons, 0, wxEXPAND);
    main_sizer->Add(bottom_panel, 0, wxEXPAND);

    m_auto_btn->Bind(wxEVT_BUTTON, &GenericFilamentMapDialog::on_switch_mode, this);
    m_manual_btn->Bind(wxEVT_BUTTON, &GenericFilamentMapDialog::on_switch_mode, this);
    m_ok_btn->Bind(wxEVT_BUTTON, &GenericFilamentMapDialog::on_ok, this);
    m_cancel_btn->Bind(wxEVT_BUTTON, &GenericFilamentMapDialog::on_cancel, this);
    SetEscapeId(wxID_CANCEL);

    SetSizerAndFit(main_sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

int GenericFilamentMapDialog::ShowModal()
{
    update_panel_status(m_page_type);
    return wxDialog::ShowModal();
}

FilamentMapMode GenericFilamentMapDialog::get_mode() const
{
    return m_page_type == ptAuto ? fmmAutoForFlush : fmmManual;
}

std::vector<int> GenericFilamentMapDialog::get_filament_maps() const
{
    return m_page_type == ptManual ? m_filament_map : std::vector<int>{};
}

void GenericFilamentMapDialog::on_ok(wxCommandEvent &)
{
    if (m_page_type == ptManual)
        m_filament_map = m_manual_panel->GetFilamentMaps();
    EndModal(wxID_OK);
}

void GenericFilamentMapDialog::on_cancel(wxCommandEvent &) { EndModal(wxID_CANCEL); }

void GenericFilamentMapDialog::on_switch_mode(wxCommandEvent &event)
{
    if (event.GetId() == ptAuto && !m_auto_grouping_allowed)
        return;
    m_page_type = PageType(event.GetId());
    update_panel_status(m_page_type);
    event.Skip();
}

void GenericFilamentMapDialog::update_panel_status(PageType page)
{
    if (page == ptAuto && !m_auto_grouping_allowed)
        page = ptManual;
    m_page_type = page;
    m_auto_btn->Select(page == ptAuto);
    m_manual_btn->Select(page == ptManual);
    m_auto_panel->Show(page == ptAuto);
    m_manual_panel->Show(page == ptManual);
    Layout();
    Fit();
}

}} // namespace Slic3r::GUI
