#pragma once

#include "FilamentMapPanel.hpp"

namespace Slic3r { namespace GUI {

// Generic-printer grouping panels own only nozzle-indexed state. They must not
// acquire Bambu left/right, AMS, or nozzle-volume assumptions.
class GenericFilamentMapManualPanel : public wxPanel
{
public:
    GenericFilamentMapManualPanel(wxWindow                       *parent,
                                  const std::vector<std::string> &color,
                                  const std::vector<std::string> &type,
                                  const std::vector<int>         &filament_list,
                                  const std::vector<int>         &filament_map,
                                  size_t                          nozzle_count);

    std::vector<int> GetFilamentMaps() const;

private:
    void OnSwitchFilament(wxCommandEvent &);

    std::vector<DragDropPanel *> m_nozzle_panels;
    ScalableButton              *m_switch_btn{nullptr};
    std::vector<int>             m_filament_map;
};

class GenericFilamentMapAutoPanel : public wxPanel
{
public:
    explicit GenericFilamentMapAutoPanel(wxWindow *parent);
    FilamentMapMode GetMode() const { return FilamentMapMode::fmmAutoForFlush; }
};

}} // namespace Slic3r::GUI
