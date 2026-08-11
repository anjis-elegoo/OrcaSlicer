#pragma once

#include "GenericFilamentMapPanel.hpp"
#include "CapsuleButton.hpp"

class Button;

namespace Slic3r { namespace GUI {

class PartPlate;
class Plater;

bool try_pop_up_generic_before_slice(bool is_slice_all, Plater *plater, PartPlate *plate, bool force_pop_up = false);

class GenericFilamentMapDialog : public wxDialog
{
    enum PageType { ptAuto, ptManual };

public:
    GenericFilamentMapDialog(wxWindow                       *parent,
                             const std::vector<std::string> &filament_color,
                             const std::vector<std::string> &filament_type,
                             const std::vector<int>         &filament_map,
                             const std::vector<int>         &filaments,
                             FilamentMapMode                 mode,
                             size_t                          nozzle_count,
                             bool                            auto_grouping_allowed);

    int             ShowModal() override;
    FilamentMapMode get_mode() const;
    std::vector<int> get_filament_maps() const;

private:
    void on_ok(wxCommandEvent &event);
    void on_cancel(wxCommandEvent &event);
    void on_switch_mode(wxCommandEvent &event);
    void update_panel_status(PageType page);

    GenericFilamentMapManualPanel *m_manual_panel{nullptr};
    GenericFilamentMapAutoPanel   *m_auto_panel{nullptr};
    CapsuleButton                 *m_auto_btn{nullptr};
    CapsuleButton                 *m_manual_btn{nullptr};
    Button                        *m_ok_btn{nullptr};
    Button                        *m_cancel_btn{nullptr};
    bool                           m_auto_grouping_allowed{true};
    PageType                       m_page_type{ptAuto};
    std::vector<int>               m_filament_map;
};

}} // namespace Slic3r::GUI
