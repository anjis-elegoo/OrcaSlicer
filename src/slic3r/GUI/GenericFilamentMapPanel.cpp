#include "GenericFilamentMapPanel.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"

namespace Slic3r { namespace GUI {

static const wxColour BgNormalColor        = wxColour("#FFFFFF");
static const wxColour TextNormalGreyColor  = wxColour("#6B6B6B");

GenericFilamentMapManualPanel::GenericFilamentMapManualPanel(wxWindow                       *parent,
                                                             const std::vector<std::string> &color,
                                                             const std::vector<std::string> &type,
                                                             const std::vector<int>         &filament_list,
                                                             const std::vector<int>         &filament_map,
                                                             size_t                          nozzle_count)
    : wxPanel(parent), m_filament_map(filament_map)
{
    SetName(wxT("GenericFilamentMapManualPanel"));
    SetBackgroundColour(BgNormalColor);

    auto *top_sizer   = new wxBoxSizer(wxVERTICAL);
    auto *description = new Label(this, _L("We will slice according to this grouping method:"));
    description->Wrap(FromDIP(520));
    top_sizer->Add(description, 0, wxALIGN_LEFT | wxLEFT, FromDIP(15));
    top_sizer->AddSpacer(FromDIP(8));

    nozzle_count = std::max<size_t>(1, nozzle_count);
    for (int &mapped_nozzle : m_filament_map)
        if (mapped_nozzle < 1 || size_t(mapped_nozzle) > nozzle_count)
            mapped_nozzle = 1;

    auto *drag_sizer = new wxGridSizer(0, nozzle_count == 1 ? 1 : 2, FromDIP(7), FromDIP(7));
    m_nozzle_panels.reserve(nozzle_count);
    for (size_t nozzle_id = 0; nozzle_id < nozzle_count; ++nozzle_id) {
        auto *panel = new DragDropPanel(this, wxString::Format(_L("Nozzle %d"), int(nozzle_id + 1)), false);
        panel->SetMinSize({FromDIP(260), -1});
        m_nozzle_panels.emplace_back(panel);
        drag_sizer->Add(panel, 1, wxEXPAND);
    }

    for (size_t idx = 0; idx < m_filament_map.size(); ++idx) {
        if (std::find(filament_list.begin(), filament_list.end(), int(idx + 1)) == filament_list.end() ||
            idx >= color.size() || idx >= type.size())
            continue;
        const int    mapped_nozzle = m_filament_map[idx] - 1;
        const size_t nozzle_id = mapped_nozzle >= 0 && size_t(mapped_nozzle) < nozzle_count ? size_t(mapped_nozzle) : 0;
        m_nozzle_panels[nozzle_id]->AddColorBlock(Hex2Color(color[idx]), type[idx], int(idx + 1));
    }
    top_sizer->Add(drag_sizer, 0, wxEXPAND);

    if (nozzle_count == 2) {
        m_switch_btn = new ScalableButton(this, wxID_ANY, "switch_filament_maps");
        auto *switch_sizer = new wxBoxSizer(wxHORIZONTAL);
        switch_sizer->AddStretchSpacer();
        switch_sizer->Add(m_switch_btn, 0, wxTOP, FromDIP(7));
        switch_sizer->AddStretchSpacer();
        top_sizer->Add(switch_sizer, 0, wxEXPAND);
        m_switch_btn->Bind(wxEVT_BUTTON, &GenericFilamentMapManualPanel::OnSwitchFilament, this);
    }

    auto *tips = new Label(this, _L("Tip: You can drag the filaments to reassign them to different nozzles."));
    tips->SetFont(Label::Body_14);
    tips->SetForegroundColour(TextNormalGreyColor);
    tips->Wrap(FromDIP(520));
    top_sizer->AddSpacer(FromDIP(20));
    top_sizer->Add(tips, 0, wxALIGN_LEFT | wxLEFT, FromDIP(15));

    SetSizer(top_sizer);
    SetMinSize(wxSize(FromDIP(580), -1));
    Layout();
    Fit();
    wxGetApp().UpdateDarkUIWin(this);
}

std::vector<int> GenericFilamentMapManualPanel::GetFilamentMaps() const
{
    std::vector<int> filament_map = m_filament_map;
    for (size_t nozzle_id = 0; nozzle_id < m_nozzle_panels.size(); ++nozzle_id)
        for (int filament_id : m_nozzle_panels[nozzle_id]->GetAllFilaments())
            if (filament_id > 0 && size_t(filament_id) <= filament_map.size())
                filament_map[filament_id - 1] = int(nozzle_id + 1);
    return filament_map;
}

void GenericFilamentMapManualPanel::OnSwitchFilament(wxCommandEvent &)
{
    if (m_nozzle_panels.size() != 2)
        return;

    auto first_nozzle_blocks  = m_nozzle_panels[0]->get_filament_blocks();
    auto second_nozzle_blocks = m_nozzle_panels[1]->get_filament_blocks();
    for (auto &block : first_nozzle_blocks) {
        m_nozzle_panels[1]->AddColorBlock(block->GetColor(), block->GetType(), block->GetFilamentId(), false);
        m_nozzle_panels[0]->RemoveColorBlock(block, false);
    }
    for (auto &block : second_nozzle_blocks) {
        m_nozzle_panels[0]->AddColorBlock(block->GetColor(), block->GetType(), block->GetFilamentId(), false);
        m_nozzle_panels[1]->RemoveColorBlock(block, false);
    }
    GetParent()->Layout();
    GetParent()->Fit();
}

GenericFilamentMapAutoPanel::GenericFilamentMapAutoPanel(wxWindow *parent) : wxPanel(parent)
{
    auto *sizer       = new wxBoxSizer(wxHORIZONTAL);
    auto *flush_panel = new FilamentMapBtnPanel(
        this,
        _L("Filament-Saving Mode"),
        _L("Generates filament grouping for the available nozzles based on the most filament-saving principles to minimize waste."),
        "flush_mode_panel_icon");
    flush_panel->Select(true);
    sizer->AddStretchSpacer();
    sizer->Add(flush_panel, 1, wxEXPAND);
    sizer->AddStretchSpacer();
    SetSizerAndFit(sizer);
    Layout();
    wxGetApp().UpdateDarkUIWin(this);
}

}} // namespace Slic3r::GUI
