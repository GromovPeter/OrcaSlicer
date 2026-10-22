#include "HotendThermalResponse.hpp"
#include "LoadedThermalCalibrationDialog.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "Tab.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Technologies.hpp"
#include "libslic3r/ThermalResponse.hpp"

#include <boost/nowide/fstream.hpp>
#include <exception>
#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <cstddef>
#include <cmath>
#include <optional>
#include <vector>
#include <wx/dcbuffer.h>
#include <wx/brush.h>
#include <wx/dc.h>
#include <wx/defs.h>
#include <wx/panel.h>
#include <wx/pen.h>
#include <wx/colour.h>
#include <wx/event.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/scrolwin.h>
#include <wx/display.h>
#include <wx/stattext.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/gdicmn.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/toplevel.h>
#include <wx/window.h>

namespace Slic3r { namespace GUI {
namespace {
struct ThermalApplicationState {
    std::optional<DynamicPrintConfig> previous;
    std::string printer_name;
};

void add_thermal_settings_controls(wxWindow& dialog, wxBoxSizer& layout,
                                   const std::optional<ThermalTargetProposal>& proposal)
{
    if (!proposal) {
        auto* unavailable = new wxStaticText(&dialog, wxID_ANY, _L("Settings cannot be proposed: accepted heating and cooling responses under positive printing load are required, within the supported setting ranges."));
        unavailable->Wrap(dialog.FromDIP(750));
        layout.Add(unavailable, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
        return;
    }
    auto* dialog_ptr = &dialog;
    auto* bundle = wxGetApp().preset_bundle;
    auto* tab = wxGetApp().get_tab(Preset::TYPE_PRINTER);
    if (!bundle || !tab || !tab->get_config()) return;
    auto* printers = &bundle->printers;
    const std::array<const char*, 4> keys{{"adaptive_temperature_rise_time", "adaptive_temperature_fall_time",
        "adaptive_temperature_lead_time", "adaptive_temperature_smoothing_period"}};
    if (printers->get_edited_preset().printer_technology() != ptFFF) return;
    for (const char* key : keys)
        if (!tab->get_config()->has(key)) return;
    const std::array<wxString, 4> labels{{_L("Target rise time"), _L("Target fall time"),
        _L("Additional temperature lead time"), _L("Flow smoothing period")}};
    const std::array<double, 4> proposed{{proposal->rise_time, proposal->fall_time, proposal->lead_time, proposal->smoothing_period}};
    const auto state = std::make_shared<ThermalApplicationState>();
    state->printer_name = printers->get_edited_preset().name;
    auto* printer_label = new wxStaticText(&dialog, wxID_ANY,
        wxString::Format(_L("Printer profile: %s"), wxString::FromUTF8(state->printer_name.c_str())));
    layout.Add(printer_label, 0, wxLEFT | wxRIGHT | wxTOP, dialog.FromDIP(12));
    auto* explanation = new wxStaticText(&dialog, wxID_ANY, _L("Provisional target ramp limits use the slower observed settling envelope with a 25% margin. Delay plus one observed sampling interval is supplied separately as additional lead. These are starting settings, not measured ramp limits or a verified thermal model."));
    explanation->Wrap(dialog.FromDIP(750));
    layout.Add(explanation, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
    auto* grid = new wxFlexGridSizer(3, dialog.FromDIP(4), dialog.FromDIP(16));
    for (const auto& label : {_L("Setting"), _L("Current"), _L("Proposed")})
        grid->Add(new wxStaticText(&dialog, wxID_ANY, label));
    std::array<wxStaticText*, 4> current{};
    for (size_t i = 0; i < keys.size(); ++i) {
        grid->Add(new wxStaticText(&dialog, wxID_ANY, labels[i]));
        current[i] = new wxStaticText(&dialog, wxID_ANY, wxString::Format("%.1f", tab->get_config()->opt_float(keys[i])));
        grid->Add(current[i]);
        grid->Add(new wxStaticText(&dialog, wxID_ANY, wxString::Format(i < 2 ? "%.1f s/C" : "%.1f s", proposed[i])));
    }
    layout.Add(grid, 0, wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    auto* smoothing = new wxCheckBox(&dialog, wxID_ANY, _L("Also apply the suggested 5 s flow smoothing period (recommendation, not a measured value)"));
    smoothing->SetValue(false);
    layout.Add(smoothing, 0, wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    auto* status = new wxStaticText(&dialog, wxID_ANY, _L("Apply changes only the printer settings. Save printer preset keeps them using the normal Orca save dialog; system presets are saved as user copies. Undo restores the previous values as unsaved edits. Filament settings are unchanged."));
    status->Wrap(dialog.FromDIP(750));
    layout.Add(status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* apply = new wxButton(&dialog, wxID_ANY, _L("Apply to printer settings"));
    auto* save = new wxButton(&dialog, wxID_ANY, _L("Save printer preset..."));
    auto* undo = new wxButton(&dialog, wxID_ANY, _L("Undo application"));
    save->Enable(false); undo->Enable(false);
    for (auto* button : {apply, save, undo}) buttons->Add(button, 0, wxRIGHT, dialog.FromDIP(8));
    layout.Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    auto same_printer = [=]() {
        if (printers->get_edited_preset().name == state->printer_name) return true;
        wxMessageBox(_L("The selected printer changed. Reopen the calibration log for the intended printer."),
                     _L("Cannot apply thermal settings"), wxOK | wxICON_ERROR, dialog_ptr);
        return false;
    };
    auto refresh = [=]() {
        for (size_t i = 0; i < keys.size(); ++i)
            current[i]->SetLabel(wxString::Format("%.1f", tab->get_config()->opt_float(keys[i])));
        printer_label->SetLabel(wxString::Format(_L("Printer profile: %s"), wxString::FromUTF8(state->printer_name.c_str())));
    };
    auto set_status = [=](const wxString& text) {
        status->SetLabel(text);
        status->Wrap(dialog_ptr->FromDIP(750));
        dialog_ptr->Layout();
        if (auto* scroll = dynamic_cast<wxScrolledWindow*>(dialog_ptr)) scroll->FitInside();
    };
    apply->Bind(wxEVT_BUTTON, [=](wxCommandEvent&) {
        if (!same_printer()) return;
        try {
            const auto patch = thermal_target_settings_patch(*proposal, smoothing->GetValue());
            DynamicPrintConfig previous;
            for (const auto& key : patch.keys()) previous.set_key_value(key, tab->get_config()->option(key)->clone());
            DynamicPrintConfig updated = *tab->get_config();
            updated.apply(patch);
            state->previous = previous;
            tab->load_config(updated);
            refresh();
            apply->Enable(false); smoothing->Enable(false); undo->Enable(true); save->Enable(true);
            set_status(_L("Applied as unsaved printer edits. Save printer preset to keep them. Saving also includes any other unsaved printer edits."));
        } catch (const std::exception& error) {
            wxMessageBox(wxString::FromUTF8(error.what()), _L("Cannot apply thermal settings"), wxOK | wxICON_ERROR, dialog_ptr);
        }
    });
    save->Bind(wxEVT_BUTTON, [=](wxCommandEvent&) {
        if (!same_printer()) return;
        try {
            tab->save_preset();
            // The normal save dialog may create a user copy with a new name.
            state->printer_name = printers->get_edited_preset().name;
            refresh();
            set_status(printers->current_is_dirty() ? _L("Printer edits are still unsaved.") :
                _L("Printer preset saved. Undo restores the previous values as unsaved edits; save again to keep the undo."));
        } catch (const std::exception& error) {
            wxMessageBox(wxString::FromUTF8(error.what()), _L("Cannot save printer preset"), wxOK | wxICON_ERROR, dialog_ptr);
        }
    });
    undo->Bind(wxEVT_BUTTON, [=](wxCommandEvent&) {
        if (!same_printer() || !state->previous) return;
        DynamicPrintConfig restored = *tab->get_config();
        restored.apply(*state->previous);
        tab->load_config(restored);
        state->previous.reset();
        refresh();
        apply->Enable(true); smoothing->Enable(true); undo->Enable(false);
        set_status(_L("Previous values restored as unsaved printer edits. Save printer preset to keep the restored values."));
    });
}

class ThermalPlot : public wxPanel {
    std::vector<ThermalSample> m_samples;
public:
    ThermalPlot(wxWindow* parent, const std::vector<ThermalSample>& samples)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(760, 230))), m_samples(samples)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetBackgroundColour()));
            dc.Clear();
            dc.SetTextForeground(GetForegroundColour());
            const int left = FromDIP(55), top = FromDIP(30);
            const int right = GetClientSize().x - FromDIP(20), bottom = GetClientSize().y - FromDIP(35);
            if (right <= left || bottom <= top) return;
            double low = m_samples.front().temperature_c, high = low;
            for (const auto& point : m_samples) {
                low = std::min({low, point.target_c, point.temperature_c});
                high = std::max({high, point.target_c, point.temperature_c});
            }
            low -= 2; high += 2;
            auto x = [&](double time) { return left + int((right - left) * (time - m_samples.front().time_s) /
                (m_samples.back().time_s - m_samples.front().time_s)); };
            auto y = [&](double temperature) { return bottom - int((bottom - top) * (temperature - low) / (high - low)); };
            dc.SetPen(wxPen(GetForegroundColour()));
            dc.DrawLine(left, top, left, bottom);
            dc.DrawLine(left, bottom, right, bottom);
            dc.DrawText(wxString::Format("%.0f C", high), 0, top);
            dc.DrawText(wxString::Format("%.0f C", low), 0, bottom - FromDIP(14));
            dc.DrawText(wxString::Format("%.0f s", m_samples.front().time_s), left, bottom + FromDIP(5));
            dc.DrawText(wxString::Format("%.0f s", m_samples.back().time_s), right - FromDIP(55), bottom + FromDIP(5));
            const wxColour target(30, 130, 220), sensor(220, 100, 25);
            dc.SetTextForeground(target);
            dc.DrawText(_L("Target temperature"), left, 0);
            dc.SetTextForeground(sensor);
            dc.DrawText(_L("Sensor temperature"), left + FromDIP(240), 0);
            for (size_t i = 1; i < m_samples.size(); ++i) {
                const auto& a = m_samples[i - 1]; const auto& b = m_samples[i];
                dc.SetPen(wxPen(target, FromDIP(2)));
                dc.DrawLine(x(a.time_s), y(a.target_c), x(b.time_s), y(a.target_c));
                dc.DrawLine(x(b.time_s), y(a.target_c), x(b.time_s), y(b.target_c));
                dc.SetPen(wxPen(sensor, FromDIP(2)));
                dc.DrawLine(x(a.time_s), y(a.temperature_c), x(b.time_s), y(b.temperature_c));
            }
        });
    }
};
}
void show_hotend_thermal_response_file(wxWindow* parent, const wxString& path)
{
    try {
        boost::nowide::ifstream input(path.ToUTF8().data());
        if (!input) {
            wxMessageBox(_L("Cannot open the thermal CSV."), _L("Cannot analyze thermal response"), wxOK | wxICON_ERROR, parent);
            return;
        }
        const auto samples = read_thermal_response_csv(input);
        const auto loaded = analyze_loaded_thermal_response(samples);
        const auto response = loaded.empty() ? analyze_thermal_response(samples) : ThermalResponse{};
        wxString report = _L("Measured sensor response from a recorded log. This does not measure melt temperature. Printer settings change only when you apply a proposal below.") + "\n\n";
        report += wxString::Format(_L("Source CSV: %s\n\n"), path);
        if (loaded.empty() && response.transitions.empty())
            report += _L("No temperature steps of at least 5 degrees were found. Record stable plateaus before and after each step.") + "\n";
        auto append_transition = [&](const ThermalTransition& t) {
            // TRN: Values are time in seconds and temperatures in degrees Celsius.
            report += wxString::Format(_L("At %.1f s: %.1f to %.1f C\n"), t.time_s, t.from_c, t.to_c);
            if (t.seconds_per_degree > 0)
                report += wxString::Format(_L("10-90%% response: %.3f s/C; extrapolated delay: %.2f s; overshoot: %.2f C\n"),
                    t.seconds_per_degree, t.delay_s, t.overshoot_c);
            if (t.settling_s)
                report += wxString::Format(_L("Settling within 1 C with at least 10 s of observed dwell: %.2f s\n"), *t.settling_s);
            if (t.usable()) report += _L("Accepted for the measured response summary.") + "\n";
            for (const auto issue : t.issues) {
                switch (issue) {
                case ThermalIssue::UnstableBaseline: report += _L("Excluded: the preceding 5 s are not stable within 1 C of the previous target."); break;
                case ThermalIssue::DataGap: report += _L("Excluded: a sample gap exceeds 2.5 s."); break;
                case ThermalIssue::Unsettled: report += _L("Excluded: the response or stable dwell is incomplete."); break;
                case ThermalIssue::MissingConditions: report += _L("Excluded: fan or flow conditions were not recorded."); break;
                case ThermalIssue::NoisyResponse: report += _L("Excluded: the transition contains a temperature reversal greater than 2 C."); break;
                case ThermalIssue::ChangingConditions: report += _L("Excluded: fan or flow conditions changed during the transition."); break;
                }
                report += "\n";
            }
            report += "\n";
        };
        if (!loaded.empty()) {
            report += _L("Printing-load log: flow is derived from commanded extruder velocity. It does not verify deposited flow or missed steps.") + "\n\n";
            const auto context = std::find_if(samples.begin(), samples.end(), [&](const ThermalSample& sample) {
                return sample.time_s >= loaded.front().begin_s;
            });
            if (context != samples.end() && context->fan && context->auxiliary_fan && context->chamber_fan)
                report += wxString::Format(_L("Initial measurement fan fractions: part %.2f; auxiliary %.2f; chamber %.2f. Changes are checked separately for each response.\n\n"),
                    *context->fan, *context->auxiliary_fan, *context->chamber_fan);
            std::optional<double> rise_envelope, fall_envelope;
            double delay = 0.0;
            for (const auto& stage : loaded) {
                report += wxString::Format(_L("Stage %s: %.1f to %.1f s; target %.1f C\n"),
                    wxString::FromUTF8(stage.name.c_str()), stage.begin_s, stage.end_s, stage.target_c);
                if (stage.commanded_flow_mm3_s)
                    report += wxString::Format(_L("Mean commanded flow over the final 5 s: %.3f mm3/s\n"), *stage.commanded_flow_mm3_s);
                if (stage.load_change)
                    report += _L("The extrusion load changed here. This stage is not a fixed-load temperature response.") + "\n";
                if (stage.transition) {
                    append_transition(*stage.transition);
                    const auto& t = *stage.transition;
                    if (t.usable() && t.settling_s) {
                        auto& envelope = t.to_c > t.from_c ? rise_envelope : fall_envelope;
                        envelope = std::max(envelope.value_or(0.0), *t.settling_s / std::abs(t.to_c - t.from_c));
                        delay = std::max(delay, t.delay_s);
                    }
                }
                report += "\n";
            }
            if (rise_envelope && fall_envelope) {
                report += wxString::Format(_L("Planning reference from observed settling: heating %.3f s/C; cooling %.3f s/C; observed delay %.2f s.\n"),
                    *rise_envelope, *fall_envelope, delay);
                report += _L("These are provisional planning references from the recorded steps, not verified limits for smooth temperature ramps. The proposal below converts them into conservative target limits; it does not copy measured slopes directly.") + "\n\n";
            }
        } else {
            for (const auto& t : response.transitions) append_transition(t);
        }
        if (response.rise_seconds_per_degree)
            report += wxString::Format(_L("Slowest accepted heating response: %.3f s/C\n"), *response.rise_seconds_per_degree);
        if (response.fall_seconds_per_degree)
            report += wxString::Format(_L("Slowest accepted cooling response: %.3f s/C\n"), *response.fall_seconds_per_degree);
        report += _L("These measurements describe only the recorded temperature range, fan and extrusion conditions. Proposals are starting settings for this printer under those conditions. Applying them does not enable adaptive temperature or change printing speeds.");
        wxDialog dialog(parent, wxID_ANY, _L("Hotend thermal response"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* outer = new wxBoxSizer(wxVERTICAL);
        auto* scroll = new wxScrolledWindow(&dialog, wxID_ANY, wxDefaultPosition, dialog.FromDIP(wxSize(800, 600)));
        scroll->SetScrollRate(dialog.FromDIP(10), dialog.FromDIP(10));
        auto* layout = new wxBoxSizer(wxVERTICAL);
        layout->Add(new ThermalPlot(scroll, samples), 0, wxEXPAND | wxALL, dialog.FromDIP(12));
        auto* text = new wxTextCtrl(scroll, wxID_ANY, report, wxDefaultPosition, dialog.FromDIP(wxSize(760, 170)), wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL);
        layout->Add(text, 1, wxEXPAND | wxALL, dialog.FromDIP(12));
        add_thermal_settings_controls(*scroll, *layout, propose_thermal_target_settings(samples));
        scroll->SetSizer(layout);
        outer->Add(scroll, 1, wxEXPAND);
        outer->Add(dialog.CreateButtonSizer(wxOK), 0, wxALIGN_RIGHT | wxALL, dialog.FromDIP(12));
        dialog.SetSizerAndFit(outer);
        dialog.SetMinSize(dialog.FromDIP(wxSize(480, 350)));
        const int display_index = wxDisplay::GetFromWindow(parent);
        const wxSize available = wxDisplay(display_index == wxNOT_FOUND ? 0 : display_index).GetClientArea().GetSize();
        const wxSize fitted = dialog.GetSize();
        dialog.SetSize(wxSize(std::min(fitted.x, available.x - dialog.FromDIP(40)),
                              std::min(fitted.y, available.y - dialog.FromDIP(60))));
        scroll->FitInside();
        dialog.CentreOnParent();
        dialog.ShowModal();
    } catch (const std::exception& error) {
        wxMessageBox(wxString::FromUTF8(error.what()), _L("Cannot analyze thermal response"), wxOK | wxICON_ERROR, parent);
    }
}
void show_hotend_thermal_response(wxWindow* parent)
{
    show_loaded_thermal_calibration(parent);
}
}} // namespace Slic3r::GUI
