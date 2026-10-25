#include "LoadedThermalCalibrationDialog.hpp"
#include "HotendThermalResponse.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/LoadedThermalCalibration.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <ios>
#include <stdexcept>
#include <utility>
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
#include <wx/filename.h>
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


bool write_signal(const wxString& path, const std::string& value)
{
    boost::nowide::ofstream output(path.ToUTF8().data());
    output << value;
    output.close();
    return bool(output);
}

class LoadedThermalDialog : public wxDialog {
    wxTimer m_timer{this};
    std::array<wxTextCtrl*,6> m_parameters{};
    wxButton* m_generate{};
    wxButton* m_select{};
    wxTextCtrl* m_conditions{};
    wxString m_fixture;
    wxString m_pending_fixture;
    std::future<LoadedThermalPlan> m_generation;

    DynamicPrintConfig current_config() const
    {
        auto* bundle=wxGetApp().preset_bundle;
        if(!bundle || bundle->filament_presets.size()!=1)
            throw std::invalid_argument(_u8L("Use one filament slot for loaded thermal calibration."));
        return bundle->full_config();
    }

    LoadedThermalInput input() const
    {
        std::array<double,6> values{};
        for(size_t i=0;i<values.size();++i) {
            wxString text=m_parameters[i]->GetValue();text.Replace(",",".");
            m_parameters[i]->ChangeValue(text);
            if(!text.ToCDouble(&values[i])) throw std::invalid_argument(_u8L("Enter a number in each calibration field."));
        }
        return {values[0],values[1],values[2],values[3],values[4],values[5]};
    }

    void generate()
    {
        if(m_process || m_generation.valid()) return;
        try {
            auto config=current_config();
            const auto plan=plan_loaded_thermal_calibration(config,input());
            m_conditions->SetValue(wxString::FromUTF8(plan.description));
            wxFileDialog output(this,_L("Save generated thermal calibration G-code"),"","ORCA_THERMAL_RESPONSE.gcode",
                "G-code (*.gcode)|*.gcode",wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
            if(output.ShowModal()!=wxID_OK) return;
            m_pending_fixture=output.GetPath();
            const std::string path=m_pending_fixture.ToUTF8().data();
            m_generation=std::async(std::launch::async,[config=std::move(config),plan,path] {
                export_loaded_thermal_calibration(config,plan,path);
                return plan;
            });
            m_ready->SetValue(false);
            set_status(_L("Generating and slicing the calibration for the current printer and filament..."));
            set_running(false);m_timer.Start(250);
        } catch(const std::exception& error) {
            wxMessageBox(wxString::FromUTF8(error.what()),_L("Cannot prepare calibration"),wxOK|wxICON_ERROR,this);
        }
    }

    void select_fixture(const wxString& path)
    {
        m_ready->SetValue(false);
        try {
            boost::nowide::ifstream source(path.ToUTF8().data(),std::ios::binary);
            std::string line;std::getline(source,line);
            const std::string marker="; _ORCA_LOADED_THERMAL_V1 ";
            if(!source || line.size()>65536 || line.rfind(marker,0)!=0)
                throw std::invalid_argument(_u8L("Choose an Orca-generated loaded thermal calibration file."));
            const auto metadata=nlohmann::json::parse(line.substr(marker.size()));
            if(metadata.value("version",0)!=1 || metadata.value("kind",std::string{})!="orca_loaded_thermal")
                throw std::invalid_argument(_u8L("Unsupported calibration protocol."));
            m_fixture=path;
            m_conditions->SetValue(wxString::FromUTF8(metadata.value("description",std::string{})));
            m_filename->SetValue(wxFileName(path).GetFullName());
            set_status(wxString::Format(_L("Calibration file: %s\nTransfer this exact file and start it yourself. Check its conditions before recording."),path));
        } catch(const std::exception& error) {
            m_fixture.clear();
            wxMessageBox(wxString::FromUTF8(error.what()),_L("Cannot prepare calibration"),wxOK|wxICON_ERROR,this);
        }
        set_running(false);
    }
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
        const bool busy=running || m_generation.valid();
        m_start->Enable(!busy && !m_fixture.empty() && m_ready->GetValue());
        m_generate->Enable(!busy);m_select->Enable(!busy);
        for(auto* field:m_parameters) field->Enable(!busy);
        m_stop->Enable(running && !m_stopping);
        m_analyze->Enable(!running && !m_output.empty() && wxFileExists(m_output));
        m_import->Enable(!busy);
        m_url->Enable(!busy);
        m_filename->Enable(!busy);
        m_ready->Enable(!busy && !m_fixture.empty());
        m_close->Enable(!busy);
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
                set_status(_L("Recording: waiting for the generated print to finish its warmup layers and reach the measurement region with steady cooling."));
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
        if (m_process || m_generation.valid() || m_fixture.empty() || !m_ready->GetValue()) return;
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
            "--url", m_url->GetValue(), "--filename", m_filename->GetValue(), "--fixture", m_fixture, "--output", m_output,
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
        add_text(_L("Generate a continuous spiral using the selected printer, nozzle, filament, plate and process. Start/end G-code and bed temperatures come from those presets. Recording supports one Klipper hotend through Moonraker. Calibration uses fixed part cooling and disables adaptive temperature/flow, layer-time slowdown and custom layer-change scripts in this file."));
        auto* parameters=new wxFlexGridSizer(2,FromDIP(5),FromDIP(10));parameters->AddGrowableCol(1);
        const std::array<wxString,6> labels{{_L("Low temperature (C)"),_L("Reference temperature (C)"),_L("High temperature (C)"),
            _L("Low flow (mm3/s)"),_L("Reference flow (mm3/s)"),_L("High flow (mm3/s)")}};
        for(size_t i=0;i<labels.size();++i) {
            parameters->Add(new wxStaticText(page,wxID_ANY,labels[i]),0,wxALIGN_CENTER_VERTICAL);
            m_parameters[i]=new wxTextCtrl(page,wxID_ANY,"");parameters->Add(m_parameters[i],1,wxEXPAND);
        }
        layout->Add(parameters,0,wxEXPAND|wxALL,FromDIP(8));
        m_conditions=new wxTextCtrl(page,wxID_ANY,"",wxDefaultPosition,FromDIP(wxSize(700,105)),wxTE_MULTILINE|wxTE_READONLY);
        layout->Add(m_conditions,0,wxEXPAND|wxALL,FromDIP(8));
        add_text(_L("1. Review the test temperatures and flows, then generate the G-code. Transfer and start it yourself. To record a file generated earlier, select its local copy; the recorder verifies its embedded protocol and exact uploaded checksum before controlling the printer."));
        m_generate=new wxButton(page,wxID_ANY,_L("Generate from current presets..."));
        m_select=new wxButton(page,wxID_ANY,_L("Select generated calibration G-code..."));
        layout->Add(m_generate,0,wxALL,FromDIP(8));layout->Add(m_select,0,wxALL,FromDIP(8));
        m_generate->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){generate();});
        m_select->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){
            wxFileDialog file(this,_L("Select generated calibration G-code"),"","","G-code (*.gcode)|*.gcode",wxFD_OPEN|wxFD_FILE_MUST_EXIST);
            if(file.ShowModal()==wxID_OK) select_fixture(file.GetPath());
        });
        try {
            const auto config=current_config();const auto a=loaded_thermal_defaults(config);
            const std::array<double,6> values{{a.low_temperature,a.reference_temperature,a.high_temperature,a.low_flow,a.reference_flow,a.high_flow}};
            for(size_t i=0;i<values.size();++i) m_parameters[i]->ChangeValue(wxString::FromCDouble(values[i],3));
            m_conditions->SetValue(wxString::FromUTF8(plan_loaded_thermal_calibration(config,a).description));
        } catch(const std::exception& error) {
            m_conditions->SetValue(wxString::FromUTF8(error.what()));
        }
        add_text(_L("2. Recording controls temperature and speed through Moonraker. Nine stages check actual settling under commanded extrusion load. At completion, Stop, or failure it attempts to restore speed and cancel only the verified calibration job, then checks that both heaters are off. Keep the printer supervised; use its interface if the connection fails."));
        auto* fields = new wxFlexGridSizer(2, FromDIP(6), FromDIP(12));
        fields->AddGrowableCol(1);
        fields->Add(new wxStaticText(page, wxID_ANY, _L("Moonraker URL")), 0, wxALIGN_CENTER_VERTICAL);
        m_url = new wxTextCtrl(page, wxID_ANY, wxEmptyString);
        fields->Add(m_url, 1, wxEXPAND);
        fields->Add(new wxStaticText(page, wxID_ANY, _L("Printer file path")), 0, wxALIGN_CENTER_VERTICAL);
        m_filename = new wxTextCtrl(page, wxID_ANY, "");
        fields->Add(m_filename, 1, wxEXPAND);
        layout->Add(fields, 0, wxEXPAND | wxALL, FromDIP(8));
        m_ready = new wxCheckBox(page, wxID_ANY, _L("I checked the generated file and its conditions; that calibration print is running."));
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
        m_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (!m_process && !m_generation.valid()) EndModal(wxID_CANCEL); });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
            if (m_generation.valid() && event.CanVeto()) {event.Veto();return;}
            if (m_process) {
                request_stop();
                if (event.CanVeto()) { event.Veto(); return; }
            }
            event.Skip();
        });
        // Escape is a request to stop, never a way to destroy an active controller.
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (m_process) request_stop(); else if (!m_generation.valid()) EndModal(wxID_CANCEL);
        }, wxID_CANCEL);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            if(m_generation.valid()) {
                if(m_generation.wait_for(std::chrono::seconds(0))!=std::future_status::ready) return;
                m_timer.Stop();
                try {
                    const auto plan=m_generation.get();
                    m_conditions->SetValue(wxString::FromUTF8(plan.description));
                    select_fixture(m_pending_fixture);
                } catch(const std::exception& error) {
                    set_status(wxString::FromUTF8(error.what()));set_running(false);
                }
                return;
            }
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
