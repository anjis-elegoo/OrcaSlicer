#pragma once

#include <wx/bitmap.h>
#include <wx/bmpbuttn.h>
#include <wx/timer.h>

#include "libslic3r/PrintConfig.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/PopupWindow.hpp"

namespace Slic3r { namespace GUI {

class PartPlate;
class Plater;

class GenericFilamentGroupPopup : public PopupWindow
{
public:
    explicit GenericFilamentGroupPopup(wxWindow *parent);
    void tryPopup(Plater *plater, PartPlate *plate, bool slice_all);
    void tryClose();

private:
    enum ButtonType { btForFlush, btManual, btCount };

    void Init();
    void OnRadioBtn(int idx);
    void UpdateButtonStatus(int hover_idx = -1);
    void DrawRoundedCorner(int radius);
    void StartTimer();
    void ResetTimer();
    void Dismiss();
    void OnPaint(wxPaintEvent &event);
    void OnTimer(wxTimerEvent &event);
    void OnLeaveWindow(wxMouseEvent &event);
    void OnEnterWindow(wxMouseEvent &event);
    FilamentMapMode GetFilamentMapMode() const;
    void SetFilamentMapMode(FilamentMapMode mode);

    const std::vector<FilamentMapMode> m_mode_list{fmmAutoForFlush, fmmManual};
    bool                              m_active{false};
    bool                              m_slice_all{false};
    bool                              m_auto_grouping_allowed{true};
    FilamentMapMode                   m_mode{fmmAutoForFlush};
    wxTimer                          *m_timer{nullptr};
    std::vector<wxBitmapButton *>     m_radio_btns;
    std::vector<Label *>              m_button_labels;
    wxBitmap                          m_checked_bmp;
    wxBitmap                          m_unchecked_bmp;
    wxBitmap                          m_checked_hover_bmp;
    wxBitmap                          m_unchecked_hover_bmp;
    PartPlate                        *m_partplate{nullptr};
    Plater                           *m_plater{nullptr};
};

}} // namespace Slic3r::GUI
