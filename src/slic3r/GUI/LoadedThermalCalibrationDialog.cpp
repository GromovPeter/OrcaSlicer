#include "LoadedThermalCalibrationDialog.hpp"
#include "HotendThermalResponse.hpp"
#include "I18N.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <exception>
#include <memory>
#include <string>
#include <vector>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/chartype.h>
#include <wx/dirdlg.h>
#include <wx/display.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/filedlg.h>
#include <wx/filefn.h>
#include <wx/gdicmn.h>
#include <wx/msgdlg.h>
#include <wx/process.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/toplevel.h>
#include <wx/utils.h>
#include <wx/window.h>

namespace Slic3r { namespace GUI {
namespace {
const wxString fixture_name = "ORCA_PLA_LOADED_CALIBRATION_20261021_v2.gcode";

bool write_signal(const wxString& path, const std::string& value)
{
    boost::nowide::ofstream output(path.ToUTF8().data());
    output << value;
    output.close();
    return bool(output);
}

class LoadedThermalDialog : public wxDialog {
    wxTimer m_timer{this};
    std::unique_ptr<wxProcess> m_process;
    wxTextCtrl* m_url;
    wxTextCtrl* m_filename;
    wxStaticText* m_status;
    wxCheckBox* m_ready;
    wxButton* m_start;
    wxButton* m_stop;
    wxButton* m_analyze;
    wxButton* m_import;
    wxButton* m_close;
    wxString m_output;
    wxString m_stop_path;
    wxString m_heartbeat;
    wxString m_progress;
    unsigned m_tick = 0;
    bool m_stopping = false;

    void set_status(const wxString& text)
    {
        m_status->SetLabel(text);
        m_status->Wrap(FromDIP(700));
        Layout();
    }

    void set_running(bool running)
    {
        m_start->Enable(!running && m_ready->GetValue());
        m_stop->Enable(running && !m_stopping);
        m_analyze->Enable(!running && !m_output.empty() && wxFileExists(m_output));
        m_import->Enable(!running);
        m_url->Enable(!running);
        m_filename->Enable(!running);
        m_ready->Enable(!running);
        m_close->Enable(!running);
    }

    void request_stop()
    {
        if (!m_process || m_stopping) return;
        if (!write_signal(m_stop_path, "stop")) {
            set_status(_L("Cannot write the stop request. Cancel the calibration in the printer interface."));
            return;
        }
        m_stopping = true;
        m_stop->Enable(false);
        set_status(_L("Stopping: waiting for cancellation, heaters off and speed restoration to be confirmed..."));
    }

    void read_progress(bool terminal = false)
    {
        try {
            boost::nowide::ifstream input(m_progress.ToUTF8().data());
            if (!input) return;
            const auto progress = nlohmann::json::parse(input);
            const auto state = progress.value("state", std::string{});
            if (terminal && state != "completed" && state != "stopped" && state != "error") return;
            if (state == "completed")
                set_status(_L("Measurement complete. Cancellation, heaters off and speed restoration confirmed. Analyze the recorded CSV to review and apply the proposed settings."));
            else if (state == "stopped")
                set_status(_L("Recording stopped. Calibration cancellation, heaters off and speed restoration confirmed. The partial CSV is retained."));
            else if (state == "error")
                set_status(progress.value("cleanup_confirmed", false) ?
                    _L("Measurement failed. Calibration cancellation, heaters off and speed restoration confirmed. The CSV and worker log are retained.") :
                    _L("Recording failed or could not verify the calibration. Check the printer interface; cancellation and heaters off have not been confirmed. The worker log is beside the CSV."));
            else if (state == "stopping" || m_stopping)
                set_status(_L("Stopping: waiting for cancellation, heaters off and speed restoration to be confirmed..."));
            else if (state == "waiting")
                set_status(_L("Recording: waiting for the prepared print to reach layer 7, the measurement height and steady cooling."));
            else if (state == "running")
                set_status(wxString::Format(_L("Stage %d / %d; nozzle %.1f C; target %.1f C; requested flow %.1f mm3/s"),
                    progress.at("stage").get<int>(), progress.at("total").get<int>(),
                    progress.at("temperature").get<double>(), progress.at("target").get<double>(), progress.at("flow").get<double>()));
        } catch (const std::exception&) {
            // A missing or temporarily unreadable progress file is not evidence of completion.
        }
    }

    void start()
    {
        if (m_process || !m_ready->GetValue()) return;
        const auto python = PythonInterpreter::bundled_python_executable();
        const wxString worker = wxString::FromUTF8(resources_dir()) + "/calibration/run_loaded_thermal_calibration.py";
        if (python.empty() || !wxFileExists(worker)) {
            wxMessageBox(_L("The bundled calibration worker or Python runtime is missing."), _L("Cannot start recording"), wxOK | wxICON_ERROR, this);
            return;
        }
        wxFileDialog output(this, _L("Save a new thermal recording"), "", "hotend-loaded.csv", "CSV (*.csv)|*.csv", wxFD_SAVE);
        if (output.ShowModal() != wxID_OK) return;
        const wxString path = output.GetPath();
        for (const wxString& suffix : {wxString{}, wxString(".summary.json"), wxString(".stop"), wxString(".heartbeat"), wxString(".progress.json"), wxString(".worker.log")}) {
            if (wxFileExists(path + suffix)) {
                wxMessageBox(_L("Choose a new CSV name. Existing recordings and their control files are never replaced."), _L("Cannot start recording"), wxOK | wxICON_ERROR, this);
                return;
            }
        }
        m_output = path;
        m_stop_path = path + ".stop";
        m_heartbeat = path + ".heartbeat";
        m_progress = path + ".progress.json";
        m_tick = 0;
        m_stopping = false;
        if (!write_signal(m_heartbeat, "0")) {
            wxMessageBox(_L("Cannot create the recording control files."), _L("Cannot start recording"), wxOK | wxICON_ERROR, this);
            return;
        }
        // wxExecute's argument-vector overload avoids shell parsing, including spaces and Unicode paths.
        std::vector<wxString> arguments{wxString::FromUTF8(python), "-B", "-u", "-X", "utf8", worker,
            "--url", m_url->GetValue(), "--filename", m_filename->GetValue(), "--output", m_output,
            "--stop-file", m_stop_path, "--heartbeat-file", m_heartbeat, "--progress-json", m_progress,
            "--log-file", path + ".worker.log", "--verify-upload", "--execute"};
        std::vector<const wxChar*> argv;
        for (const auto& argument : arguments) argv.push_back(argument.c_str());
        argv.push_back(nullptr);
        m_process = std::make_unique<wxProcess>(this);
        if (wxExecute(argv.data(), wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE, m_process.get()) == 0) {
            m_process.reset();
            set_status(_L("The calibration worker could not be launched. No printer commands were sent."));
            return;
        }
        set_status(wxString::Format(_L("Verifying the uploaded calibration file. Recording: %s"), m_output));
        set_running(true);
        m_timer.Start(500);
    }

public:
    explicit LoadedThermalDialog(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, _L("Hotend thermal response"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        auto* outer = new wxBoxSizer(wxVERTICAL);
        auto* page = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(760, 540)));
        page->SetScrollRate(FromDIP(10), FromDIP(10));
        auto* layout = new wxBoxSizer(wxVERTICAL);
        auto add_text = [&](const wxString& text) {
            auto* label = new wxStaticText(page, wxID_ANY, text);
            label->Wrap(FromDIP(700));
            layout->Add(label, 0, wxEXPAND | wxALL, FromDIP(8));
        };
        add_text(_L("Measure the hotend while printing a continuous spiral. This prepared protocol is for K2PRO, PLA 1.75 mm, a 0.4 mm nozzle and Textured PEI Plate at 50 C. It uses the fixed table 1 mm3/s at 196 C, 10 at 213 C and 22 at 228 C; it does not use the currently selected filament table."));
        add_text(_L("1. Save the prepared G-code, transfer it to the printer and start it yourself with the original filename. Start recording here while its initial layers are printing. The recorder verifies the uploaded file and waits for the measurement region."));
        auto* prepare = new wxButton(page, wxID_ANY, _L("Save K2PRO PLA calibration G-code..."));
        layout->Add(prepare, 0, wxALL, FromDIP(8));
        prepare->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            wxDirDialog folder(this, _L("Choose the calibration G-code folder"));
            if (folder.ShowModal() != wxID_OK) return;
            const wxString source = wxString::FromUTF8(resources_dir()) + "/calibration/" + fixture_name;
            const wxString destination = folder.GetPath() + "/" + fixture_name;
            if (!wxCopyFile(source, destination, false))
                wxMessageBox(_L("Cannot save the calibration file. Choose a writable folder without an existing file of that name."), _L("Cannot prepare calibration"), wxOK | wxICON_ERROR, this);
            else
                wxMessageBox(wxString::Format(_L("Saved: %s\nStart this file as a separate print, then start recording here. Do not reslice or rename it."), destination), _L("Calibration file prepared"), wxOK | wxICON_INFORMATION, this);
        });
        add_text(_L("2. Recording controls temperature and speed through Moonraker. Nine stages check actual settling under commanded extrusion load. At completion, Stop, or failure it attempts to restore speed and cancel only the verified calibration job, then checks that both heaters are off. Keep the printer supervised; use its interface if the connection fails."));
        auto* fields = new wxFlexGridSizer(2, FromDIP(6), FromDIP(12));
        fields->AddGrowableCol(1);
        fields->Add(new wxStaticText(page, wxID_ANY, _L("Moonraker URL")), 0, wxALIGN_CENTER_VERTICAL);
        m_url = new wxTextCtrl(page, wxID_ANY, wxEmptyString);
        fields->Add(m_url, 1, wxEXPAND);
        fields->Add(new wxStaticText(page, wxID_ANY, _L("Printer file path")), 0, wxALIGN_CENTER_VERTICAL);
        m_filename = new wxTextCtrl(page, wxID_ANY, fixture_name);
        fields->Add(m_filename, 1, wxEXPAND);
        layout->Add(fields, 0, wxEXPAND | wxALL, FromDIP(8));
        m_ready = new wxCheckBox(page, wxID_ANY, _L("I checked these conditions; the prepared K2PRO PLA print is running."));
        layout->Add(m_ready, 0, wxALL, FromDIP(8));
        m_ready->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { set_running(bool(m_process)); });
        auto* actions = new wxBoxSizer(wxHORIZONTAL);
        m_start = new wxButton(page, wxID_ANY, _L("Start recording..."));
        m_stop = new wxButton(page, wxID_ANY, _L("Stop and cancel calibration"));
        actions->Add(m_start, 0, wxRIGHT, FromDIP(8));
        actions->Add(m_stop);
        layout->Add(actions, 0, wxALL, FromDIP(8));
        m_start->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start(); });
        m_stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { request_stop(); });
        m_status = new wxStaticText(page, wxID_ANY, _L("Ready. No connection or printer commands have been made."), wxDefaultPosition, FromDIP(wxSize(700, 90)), wxST_NO_AUTORESIZE);
        layout->Add(m_status, 0, wxEXPAND | wxALL, FromDIP(8));
        add_text(_L("3. Analyze the recorded CSV, review the graph and proposed values, then apply and save them using the printer preset controls. Measurements describe the nozzle sensor under these conditions, not melt temperature. Settings are never applied automatically."));
        m_analyze = new wxButton(page, wxID_ANY, _L("Analyze recorded CSV..."));
        m_import = new wxButton(page, wxID_ANY, _L("Open an existing thermal CSV..."));
        layout->Add(m_analyze, 0, wxALL, FromDIP(8));
        layout->Add(m_import, 0, wxALL, FromDIP(8));
        m_analyze->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_hotend_thermal_response_file(this, m_output); });
        m_import->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            wxFileDialog input(this, _L("Import hotend thermal response CSV"), "", "", "CSV (*.csv)|*.csv", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (input.ShowModal() == wxID_OK) show_hotend_thermal_response_file(this, input.GetPath());
        });
        page->SetSizer(layout);
        outer->Add(page, 1, wxEXPAND);
        m_close = new wxButton(this, wxID_CANCEL, _L("Close"));
        outer->Add(m_close, 0, wxALIGN_RIGHT | wxALL, FromDIP(10));
        m_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (!m_process) EndModal(wxID_CANCEL); });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
            if (m_process) {
                request_stop();
                if (event.CanVeto()) { event.Veto(); return; }
            }
            event.Skip();
        });
        // Escape is a request to stop, never a way to destroy an active controller.
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (m_process) request_stop(); else EndModal(wxID_CANCEL);
        }, wxID_CANCEL);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            if (!write_signal(m_heartbeat, std::to_string(++m_tick))) request_stop();
            read_progress();
        });
        Bind(wxEVT_END_PROCESS, [this](wxProcessEvent& event) {
            m_timer.Stop();
            m_process.reset();
            set_status(event.GetExitCode() == 0 ? _L("Recording finished. Open the recorded CSV for analysis.") :
                _L("Recording stopped or failed. Check the printer interface and the worker log beside the CSV."));
            read_progress(true);
            set_running(false);
        });
        set_running(false);
        SetSizerAndFit(outer);
        SetMinSize(FromDIP(wxSize(480, 360)));
        const int display = wxDisplay::GetFromWindow(parent);
        const wxSize available = wxDisplay(display == wxNOT_FOUND ? 0 : display).GetClientArea().GetSize();
        SetSize(wxSize(std::min(GetSize().x, available.x - FromDIP(40)), std::min(GetSize().y, available.y - FromDIP(60))));
        page->FitInside();
        CentreOnParent();
    }

    ~LoadedThermalDialog() override
    {
        m_timer.Stop();
        if (m_process) {
            // Forced application shutdown: leave the worker alive to perform guarded cancellation.
            // The heartbeat lease also detects crashes without relying on this destructor.
            write_signal(m_stop_path, "stop");
            m_process->Detach();
            m_process.release();
        }
    }
};
}
void show_loaded_thermal_calibration(wxWindow* parent)
{
    LoadedThermalDialog dialog(parent);
    dialog.ShowModal();
}
}} // namespace Slic3r::GUI
