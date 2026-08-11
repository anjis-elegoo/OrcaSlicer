#include "GenericFilamentGroupPopup.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"

namespace Slic3r { namespace GUI {

static const wxColour LabelEnableColor = wxColour("#262E30");
static const wxColour GreyColor        = wxColour("#6B6B6B");
static const wxColour BackGroundColor  = wxColour("#FFFFFF");

static bool should_pop_up_generic()
{
    const auto &preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle || preset_bundle->is_bbl_vendor())
        return false;
    return is_multiple_filaments_per_nozzle_enabled(preset_bundle->full_config());
}

GenericFilamentGroupPopup::GenericFilamentGroupPopup(wxWindow *parent)
    : PopupWindow(parent, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS)
{
    SetBackgroundColour(BackGroundColor);
    m_checked_bmp         = create_scaled_bitmap("radio_on", nullptr, 16);
    m_unchecked_bmp       = create_scaled_bitmap("radio_off", nullptr, 16);
    m_checked_hover_bmp   = create_scaled_bitmap("radio_on_hover", nullptr, 16);
    m_unchecked_hover_bmp = create_scaled_bitmap("radio_off_hover", nullptr, 16);

    const std::vector<wxString> labels = {_L("Filament-Saving Mode"), _L("Custom Mode")};
    const std::vector<wxString> details = {
        _L("Generates filament grouping for the available nozzles based on the most filament-saving principles to minimize waste."),
        _L("Manually assign filaments to the available nozzles")};
    const int horizontal_margin = FromDIP(16);
    const int vertical_padding  = FromDIP(12);
    const int ratio_spacing     = FromDIP(4);
    auto     *top_sizer         = new wxBoxSizer(wxVERTICAL);
    top_sizer->AddSpacer(FromDIP(15));

    m_radio_btns.resize(btCount);
    m_button_labels.resize(btCount);
    for (size_t idx = 0; idx < btCount; ++idx) {
        auto *button_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_radio_btns[idx] = new wxBitmapButton(this, wxID_ANY, m_unchecked_bmp, wxDefaultPosition, wxDefaultSize, wxNO_BORDER);
        m_radio_btns[idx]->SetBackgroundColour(BackGroundColor);
        m_button_labels[idx] = new Label(this, labels[idx]);
        m_button_labels[idx]->SetBackgroundColour(BackGroundColor);
        m_button_labels[idx]->SetForegroundColour(LabelEnableColor);
        m_button_labels[idx]->SetFont(Label::Body_14);
        button_sizer->Add(m_radio_btns[idx], 0, wxALIGN_CENTER);
        button_sizer->AddSpacer(ratio_spacing);
        button_sizer->Add(m_button_labels[idx], 0, wxALIGN_CENTER);

        auto *detail_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto *detail       = new Label(this, details[idx]);
        detail->SetBackgroundColour(BackGroundColor);
        detail->SetForegroundColour(GreyColor);
        detail->SetFont(Label::Body_12);
        detail->Wrap(FromDIP(320));
        detail_sizer->AddSpacer(m_radio_btns[idx]->GetRect().width + ratio_spacing);
        detail_sizer->Add(detail, 1, wxALIGN_CENTER_VERTICAL);

        top_sizer->Add(button_sizer, 0, wxLEFT | wxRIGHT, horizontal_margin);
        top_sizer->Add(detail_sizer, 0, wxLEFT | wxRIGHT, horizontal_margin);
        top_sizer->AddSpacer(vertical_padding);

        m_radio_btns[idx]->Bind(wxEVT_LEFT_DOWN, [this, idx](wxMouseEvent &) { OnRadioBtn(int(idx)); });
        m_button_labels[idx]->Bind(wxEVT_LEFT_DOWN, [this, idx](wxMouseEvent &) { OnRadioBtn(int(idx)); });
        m_radio_btns[idx]->Bind(wxEVT_ENTER_WINDOW, [this, idx](wxMouseEvent &) { UpdateButtonStatus(int(idx)); });
        m_button_labels[idx]->Bind(wxEVT_ENTER_WINDOW, [this, idx](wxMouseEvent &) { UpdateButtonStatus(int(idx)); });
        m_radio_btns[idx]->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &) { UpdateButtonStatus(); });
        m_button_labels[idx]->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &) { UpdateButtonStatus(); });
    }
    top_sizer->AddSpacer(FromDIP(3));
    SetSizerAndFit(top_sizer);

    m_timer = new wxTimer(this);
    Bind(wxEVT_PAINT, &GenericFilamentGroupPopup::OnPaint, this);
    Bind(wxEVT_TIMER, &GenericFilamentGroupPopup::OnTimer, this);
    Bind(wxEVT_ENTER_WINDOW, &GenericFilamentGroupPopup::OnEnterWindow, this);
    Bind(wxEVT_LEAVE_WINDOW, &GenericFilamentGroupPopup::OnLeaveWindow, this);
    wxGetApp().UpdateDarkUIWin(this);
}

void GenericFilamentGroupPopup::Init()
{
    m_auto_grouping_allowed = !has_different_nozzle_diameters(wxGetApp().preset_bundle->full_config());
    m_mode = GetFilamentMapMode();
    if (m_mode != fmmAutoForFlush && m_mode != fmmManual)
        m_mode = fmmAutoForFlush;
    if (!m_auto_grouping_allowed && is_auto_filament_map_mode(m_mode))
        m_mode = fmmManual;

    m_radio_btns[btForFlush]->Enable(m_auto_grouping_allowed);
    m_button_labels[btForFlush]->Enable(m_auto_grouping_allowed);
    const wxString auto_disabled_tip =
        _L("Automatic grouping is unavailable when nozzle diameters differ.");
    m_radio_btns[btForFlush]->SetToolTip(m_auto_grouping_allowed ? wxString() : auto_disabled_tip);
    m_button_labels[btForFlush]->SetToolTip(m_auto_grouping_allowed ? wxString() : auto_disabled_tip);
    UpdateButtonStatus();
    wxGetApp().UpdateDarkUIWin(this);
}

void GenericFilamentGroupPopup::tryPopup(Plater *plater, PartPlate *plate, bool slice_all)
{
    if (!should_pop_up_generic())
        return;
    m_plater    = plater;
    m_partplate = plate;
    m_slice_all = slice_all;
    if (m_active) {
        ResetTimer();
        return;
    }
    m_active = true;
    Init();
    ResetTimer();
    DrawRoundedCorner(16);
    PopupWindow::Popup();
}

FilamentMapMode GenericFilamentGroupPopup::GetFilamentMapMode() const
{
    return m_partplate->get_real_filament_map_mode(wxGetApp().preset_bundle->project_config);
}

void GenericFilamentGroupPopup::SetFilamentMapMode(FilamentMapMode mode)
{
    if (m_slice_all)
        for (PartPlate *plate : m_plater->get_partplate_list().get_plate_list())
            plate->set_filament_map_mode(mode);
    else
        m_partplate->set_filament_map_mode(mode);
}

void GenericFilamentGroupPopup::OnRadioBtn(int idx)
{
    if (idx < 0 || idx >= int(m_mode_list.size()))
        return;
    if (idx == btForFlush && !m_auto_grouping_allowed)
        return;
    m_mode = m_mode_list[idx];
    SetFilamentMapMode(m_mode);
    m_plater->update();
    UpdateButtonStatus(idx);
}

void GenericFilamentGroupPopup::UpdateButtonStatus(int hover_idx)
{
    for (int idx = 0; idx < btCount; ++idx) {
        const bool selected = m_mode_list[idx] == m_mode;
        m_radio_btns[idx]->SetBitmap(selected ? (idx == hover_idx ? m_checked_hover_bmp : m_checked_bmp) :
                                                (idx == hover_idx ? m_unchecked_hover_bmp : m_unchecked_bmp));
        m_button_labels[idx]->SetFont(selected ? Label::Head_14 : Label::Body_14);
    }
    Layout();
    Fit();
}

void GenericFilamentGroupPopup::DrawRoundedCorner(int radius)
{
#ifdef __WIN32__
    HWND hwnd = GetHWND();
    if (hwnd) {
        HRGN hrgn = CreateRoundRectRgn(0, 0, GetRect().GetWidth(), GetRect().GetHeight(), radius, radius);
        SetWindowRgn(hwnd, hrgn, FALSE);
        SetWindowLong(hwnd, GWL_EXSTYLE, GetWindowLong(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(hwnd, 0, 0, LWA_COLORKEY);
    }
#endif
}

void GenericFilamentGroupPopup::tryClose() { StartTimer(); }
void GenericFilamentGroupPopup::StartTimer() { m_timer->StartOnce(300); }
void GenericFilamentGroupPopup::ResetTimer() { if (m_timer->IsRunning()) m_timer->Stop(); }
void GenericFilamentGroupPopup::OnPaint(wxPaintEvent &) { DrawRoundedCorner(16); }

void GenericFilamentGroupPopup::OnTimer(wxTimerEvent &)
{
#if __APPLE__
    wxPoint pos = ScreenToClient(wxGetMousePosition());
    if (GetClientRect().Contains(pos)) return;
#endif
    Dismiss();
}

void GenericFilamentGroupPopup::Dismiss()
{
    m_active = false;
    PopupWindow::Dismiss();
    m_timer->Stop();
}

void GenericFilamentGroupPopup::OnLeaveWindow(wxMouseEvent &)
{
    wxPoint pos = ScreenToClient(wxGetMousePosition());
    if (!GetClientRect().Contains(pos)) StartTimer();
}

void GenericFilamentGroupPopup::OnEnterWindow(wxMouseEvent &)
{
    wxPoint pos = ScreenToClient(wxGetMousePosition());
    if (GetClientRect().Contains(pos)) ResetTimer();
}

}} // namespace Slic3r::GUI
