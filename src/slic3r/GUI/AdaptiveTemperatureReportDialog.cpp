#include "AdaptiveTemperatureReportDialog.hpp"
#include "libslic3r/GCode/AdaptiveTemperatureReport.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/listctrl.h>
#include <wx/listbase.h>
#include <wx/toplevel.h>
#include <wx/scrolwin.h>
#include <wx/display.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/gdicmn.h>
#include <wx/defs.h>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <cstddef>
#include <exception>
#include <string>

namespace Slic3r { namespace GUI {
namespace {
wxString reason_text(AdaptiveTemperatureReason reason)
{
    switch (reason) {
    case AdaptiveTemperatureReason::HeatingRate: return _L("Heating target rate limit");
    case AdaptiveTemperatureReason::CoolingRate: return _L("Cooling target rate limit");
    case AdaptiveTemperatureReason::CommandSpacing: return _L("Command interval or target quantization");
    case AdaptiveTemperatureReason::ExcursionFilter: return _L("Short temperature excursion suppressed");
    case AdaptiveTemperatureReason::Lookahead: return _L("Target held for future printing demand");
    case AdaptiveTemperatureReason::RegionEntry: return _L("Entry target differs from flow reference");
    case AdaptiveTemperatureReason::MinimumSpeed: return _L("Minimum coordinated print speed reached");
    case AdaptiveTemperatureReason::BridgeSpeed: return _L("Original bridge speed retained");
    case AdaptiveTemperatureReason::ProtectedRecovery: return _L("Combined unretract move left unchanged");
    case AdaptiveTemperatureReason::CommandBoundary: return _L("Target step or motion boundary limit");
    }
    return _L("Unknown planning note");
}
}
void show_adaptive_temperature_report(wxWindow* parent, const std::string& filename)
{
    std::string path = filename;
    if (path.empty()) {
        wxFileDialog select(parent, _L("Open G-code temperature report"), wxEmptyString, wxEmptyString,
            _L("G-code files (*.gcode;*.gco)|*.gcode;*.gco"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (select.ShowModal() != wxID_OK) return;
        path = select.GetPath().ToUTF8().data();
    }
    try {
        const auto loaded = read_adaptive_temperature_report(path);
        if (!loaded) {
            wxMessageBox(_L("This G-code does not contain an adaptive temperature report. Slice the project with adaptive nozzle temperature enabled to create one."),
                _L("Adaptive temperature report"), wxOK | wxICON_INFORMATION, parent);
            return;
        }
        const auto& report = *loaded;
        wxDialog dialog(parent, wxID_ANY, _L("Adaptive temperature report"), wxDefaultPosition, wxDefaultSize,
            wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* outer = new wxBoxSizer(wxVERTICAL);
        auto* scroll = new wxScrolledWindow(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxHSCROLL);
        scroll->SetScrollRate(dialog.FromDIP(10), dialog.FromDIP(10));
        auto* layout = new wxBoxSizer(wxVERTICAL);
        wxString summary = report.coordinated ? _L("Mode: coordinated flow and temperature.") : _L("Mode: adaptive temperature only.");
        summary += "\n" + wxString::Format(_L("Estimated print time: %.1f min to %.1f min (%+.1f s)."),
            report.original_time / 60, report.planned_time / 60, report.planned_time - report.original_time);
        if (report.has_target_range)
            summary += "\n" + wxString::Format(_L("Adaptive printing target range: %.1f to %.1f °C."), report.minimum_target, report.maximum_target);
        summary += "\n" + wxString::Format(_L("Adaptive temperature command count: %zu. Reduced printing move count: %zu."), report.commands, report.slowed_moves);
        summary += "\n" + (report.constrained ? _L("Planning limitations remain; see the layer and reason groups below.") : _L("No planning limitation warning was raised."));
        summary += "\n" + _L("Temperature is a planned target. No nozzle temperature measurements are included.");
        auto* heading = new wxStaticText(scroll, wxID_ANY, summary);
        heading->Wrap(900); layout->Add(heading, 0, wxEXPAND | wxALL, 12);
        auto* notes = new wxStaticText(scroll, wxID_ANY,
            _L("Original clock: before speed changes. Final clock: after speed changes. Layers start at 1. Original-clock differences use the nominal flow/temperature reference; final-clock differences use smoothed lookahead demand. Groups include intentional smoothing and lookahead holds. These are not measured heater errors."));
        notes->Wrap(900); layout->Add(notes, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        auto* list = new wxListCtrl(scroll, wxID_ANY, wxDefaultPosition, wxSize(900, 300), wxLC_REPORT | wxLC_SINGLE_SEL);
        list->InsertColumn(0, _L("Clock"), wxLIST_FORMAT_LEFT, 95);
        list->InsertColumn(1, _L("Layer"), wxLIST_FORMAT_LEFT, 60);
        list->InsertColumn(2, _L("Time range (s)"), wxLIST_FORMAT_LEFT, 135);
        list->InsertColumn(3, _L("Planning reason"), wxLIST_FORMAT_LEFT, 335);
        list->InsertColumn(4, _L("Max. target difference (°C)"), wxLIST_FORMAT_LEFT, 170);
        list->InsertColumn(5, _L("Affected boundaries"), wxLIST_FORMAT_LEFT, 125);
        auto issues = report.issues;
        std::stable_sort(issues.begin(), issues.end(), [](const auto& a, const auto& b) {
            if (a.layer != b.layer) return a.layer < b.layer;
            if (a.original_clock != b.original_clock) return a.original_clock;
            return a.first_time < b.first_time;
        });
        for (const auto& issue : issues) {
            const long row = list->InsertItem(list->GetItemCount(), issue.original_clock ? _L_CONTEXT("Original", "Adaptive temperature report clock") : _L_CONTEXT("Final", "Adaptive temperature report clock"));
            list->SetItem(row, 1, wxString::Format("%u", issue.layer));
            list->SetItem(row, 2, wxString::Format("%.1f - %.1f", issue.first_time, issue.last_time));
            list->SetItem(row, 3, reason_text(issue.reason));
            // Recovery protection is an exclusion, not a quantified thermal gap.
            list->SetItem(row, 4, issue.reason == AdaptiveTemperatureReason::ProtectedRecovery ? _L("Not evaluated") : wxString::Format("%.2f", issue.maximum_gap));
            list->SetItem(row, 5, wxString::Format("%zu", issue.boundaries));
        }
        layout->Add(list, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);
        auto* count = new wxStaticText(scroll, wxID_ANY, wxString::Format(_L("Layer/reason groups shown: %zu of %zu."), report.issues.size(), report.total_issue_groups));
        layout->Add(count, 0, wxALL, 12);
        scroll->SetSizer(layout);
        outer->Add(scroll, 1, wxEXPAND);
        outer->Add(dialog.CreateButtonSizer(wxOK), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        dialog.SetSizerAndFit(outer);
        const int display_index = wxDisplay::GetFromWindow(parent);
        const wxSize available = wxDisplay(display_index == wxNOT_FOUND ? 0 : display_index).GetClientArea().GetSize();
        const wxSize fitted = dialog.GetSize();
        dialog.SetMinSize(wxSize(std::min(dialog.FromDIP(760), available.x - dialog.FromDIP(40)),
                                 std::min(dialog.FromDIP(430), available.y - dialog.FromDIP(60))));
        dialog.SetSize(wxSize(std::min(fitted.x, available.x - dialog.FromDIP(40)),
                             std::min(fitted.y, available.y - dialog.FromDIP(60))));
        scroll->FitInside();
        dialog.CentreOnParent();
        wxGetApp().UpdateDlgDarkUI(&dialog);
        dialog.ShowModal();
    } catch (const std::exception& error) {
        BOOST_LOG_TRIVIAL(error) << "Adaptive temperature report: " << error.what();
        wxMessageBox(_L("Could not read the adaptive temperature report. The file may be missing, incomplete or unsupported. Reslice the project to recreate the report."),
            _L("Adaptive temperature report"), wxOK | wxICON_ERROR, parent);
    }
}
}} // namespace Slic3r::GUI
