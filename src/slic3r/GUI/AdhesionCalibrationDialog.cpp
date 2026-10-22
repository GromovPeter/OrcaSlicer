#include "AdhesionCalibrationDialog.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "libslic3r/InterlayerAdhesionCalibration.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <exception>
#include <optional>
#include <string>
#include <stdexcept>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/clipbrd.h>
#include <wx/panel.h>
#include <wx/tglbtn.h>
#include <wx/dataobj.h>
#include <wx/dialog.h>
#include <wx/display.h>
#include <wx/event.h>
#include <wx/filename.h>
#include <wx/gdicmn.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/toplevel.h>
#include <wx/window.h>

namespace Slic3r { namespace GUI {
namespace {
class AdhesionDialog : public wxDialog {
    std::array<wxTextCtrl*,8> m_fields{};
    wxTextCtrl* m_preview{};
    wxCheckBox* m_disable_layer_time_slowdown{};
    wxTextCtrl* m_measured{};
    wxStaticText* m_result{};
    wxButton* m_create{};
    wxButton* m_copy{};
    wxButton* m_apply{};
    std::optional<AdhesionCalibrationPlan> m_estimate;
    std::optional<AdhesionCalibrationPlan> m_project;
    std::optional<int> m_temperature;
    double m_result_flow{};
    std::string m_result_filament;
    bool m_initialized{false};

    DynamicPrintConfig current_config() const
    {
        auto* bundle=wxGetApp().preset_bundle;
        if(!bundle || bundle->filament_presets.size()!=1)
            throw std::invalid_argument(_u8L("Use one filament slot for interlayer adhesion calibration."));
        return bundle->full_config();
    }
    AdhesionCalibrationInput input() const
    {
        std::array<double,8> v{};
        for(size_t i=0;i<v.size();++i)
            if(!m_fields[i]->GetValue().ToDouble(&v[i])) throw std::invalid_argument(_u8L("Enter a number in each calibration field."));
        AdhesionCalibrationInput a;
        a.width=v[0];a.layer_height=v[1];a.flow=v[2];
        for(size_t i=3;i<=5;++i)
            if(!std::isfinite(v[i]) || v[i]<0 || v[i]>1000 || v[i]!=std::floor(v[i])) throw std::invalid_argument(_u8L("Temperatures and temperature step must be whole numbers."));
        a.start_temperature=int(v[3]);a.end_temperature=int(v[4]);a.temperature_step=int(v[5]);a.stable_height=v[6];a.minimum_diameter=v[7];
        a.disable_layer_time_slowdown=m_disable_layer_time_slowdown->GetValue();
        return a;
    }
    void estimate()
    {
        if(!m_initialized) return;
        try {
            m_estimate=plan_adhesion_calibration(current_config(),input());
            const auto& p=*m_estimate;
            m_preview->SetValue(wxString::Format(_L("Cylinder diameter: %.1f mm\nTotal height: %.2f mm; steps: %zu\nPrinting speed: %.2f mm/s; turn time: %.2f s\nMeasurement begins at: %.2f mm\nCooling section per step: %.2f mm; stable section: %.2f mm\nEstimated wall printing: %.1f min; wall volume: %.0f mm3 (brim and printer startup are additional)"),
                p.diameter,p.height,p.bands.size(),p.speed,p.turn_time,p.measurement_z,p.transition_height,p.stable_height,p.estimated_wall_time/60,p.estimated_wall_volume));
            if(p.input.disable_layer_time_slowdown)
                m_preview->AppendText("\n"+_L("Layer-time slowdown is disabled for this calibration. Fan settings are unchanged; shorter turns change cooling between layers."));
            m_create->Enable(true);
        } catch(const std::exception& e) {
            m_estimate.reset();m_preview->SetValue(wxString::FromUTF8(e.what()));m_create->Enable(false);
        }
    }
    static void apply_patch(Preset::Type type,const DynamicPrintConfig& patch)
    {
        auto* tab=wxGetApp().get_tab(type);
        DynamicPrintConfig updated=*tab->get_config();updated.apply(patch);tab->load_config(updated);
    }
    void create()
    {
        estimate();if(!m_estimate) return;
        try {
            auto* plater=wxGetApp().plater();
            if(plater->new_project(false,false,_L("Interlayer adhesion calibration"),false)!=wxID_YES) return;
            {
            SuppressBackgroundProcessingUpdate suppress_updates;
            const auto p=plan_adhesion_calibration(current_config(),input());
            // Generate the solid mesh natively; the ordinary spiral slicer creates its hollow wall.
            const boost::filesystem::path directory=boost::filesystem::path(data_dir())/"calibration";
            boost::filesystem::create_directories(directory);
            const wxString temp=wxFileName::CreateTempFileName(wxString::FromUTF8((directory/"adhesion-").string()));
            const std::string file=std::string(temp.ToUTF8().data())+".stl";
            boost::filesystem::rename(boost::filesystem::path(std::string(temp.ToUTF8().data())),boost::filesystem::path(file));
            if(!make_cylinder(p.diameter/2,p.height).write_binary(file.c_str()))
                throw std::runtime_error(_u8L("Cannot save the generated calibration mesh."));
            apply_patch(Preset::TYPE_PRINT,adhesion_process_patch(p));
            apply_patch(Preset::TYPE_FILAMENT,adhesion_filament_patch(p));
            if(!plater->add_model(false,file)) throw std::runtime_error(_u8L("Cannot load the generated calibration cylinder."));
            auto& model=plater->model();
            model.objects.front()->name="Interlayer adhesion cylinder";
            model.plates_custom_gcodes[model.curr_plate_index]=adhesion_custom_gcodes(p);
            plater->changed_objects({0});plater->update(true,true);
            m_project=p;
            }
            plater->schedule_background_process();
            wxMessageBox(_L("The cylinder and temperature height map are ready. Use the normal Slice plate and Print plate controls. Save the project as 3MF to retain the map. Filament fan settings are retained; layer-time slowdown follows the calibration checkbox. Automatic temperature and flow adaptation are disabled for this calibration. Printer presets are not saved automatically."),
                _L("Calibration created"),wxOK|wxICON_INFORMATION,this);
            EndModal(wxID_OK);
        } catch(const std::exception& e) {
            wxMessageBox(wxString::FromUTF8(e.what()),_L("Cannot create calibration"),wxOK|wxICON_ERROR,this);
        }
    }
    void evaluate()
    {
        m_temperature.reset();m_apply->Enable(false);m_copy->Enable(false);
        try {
            auto* plater=wxGetApp().plater();
            m_project=adhesion_plan_from_codes(plater->model().get_curr_plate_custom_gcodes());
            if(!m_project) throw std::invalid_argument(_u8L("Open the saved calibration project to use its height map."));
            validate_adhesion_project(plater->model(),current_config(),*m_project);
            double height=0;
            if(!m_measured->GetValue().ToDouble(&height)) throw std::invalid_argument(_u8L("Enter the remaining cylinder height measured from the build plate."));
            const auto result=adhesion_temperature_at_height(*m_project,height);
            using Kind=AdhesionHeightResult::Kind;
            if(result.kind==Kind::Outside) m_result->SetLabel(_L("This height is outside the calibration cylinder."));
            else if(result.kind==Kind::Warmup) m_result->SetLabel(_L("This is the base or warmup section, outside the measurement."));
            else if(result.kind==Kind::Transition)
                m_result->SetLabel(wxString::Format(_L("Cooling section: commanded target changed from %d to %d C. This height does not identify a settled sensor temperature."),result.previous_temperature,result.temperature));
            else {
                m_temperature=result.temperature;m_result_flow=m_project->input.flow;
                m_result_filament=wxGetApp().preset_bundle->filaments.get_edited_preset().name;
                m_result->SetLabel(wxString::Format(_L("Stable-section target: %d C at %.3f mm3/s. This is a commanded target, not a measured melt temperature or a guaranteed working-temperature recommendation."),*m_temperature,m_result_flow));
                m_copy->Enable(true);m_apply->Enable(true);
            }
            m_result->Wrap(FromDIP(700));Layout();
        } catch(const std::exception& e) { m_result->SetLabel(wxString::FromUTF8(e.what()));m_result->Wrap(FromDIP(700)); }
    }
    void apply_result()
    {
        if(!m_temperature) return;
        try {
            if(wxGetApp().preset_bundle->filaments.get_edited_preset().name!=m_result_filament)
                throw std::invalid_argument(_u8L("The selected filament changed. Evaluate the measurement again before applying it."));
            validate_adhesion_project(wxGetApp().plater()->model(),current_config(),*m_project);
            auto* tab=wxGetApp().get_tab(Preset::TYPE_FILAMENT);
            DynamicPrintConfig updated=*tab->get_config();
            const std::string merged=adhesion_table_with_point(updated.opt_string("filament_flow_temperature_table",0u),m_result_flow,*m_temperature);
            updated.set_key_value("filament_flow_temperature_table",new ConfigOptionStrings{merged});tab->load_config(updated);
            wxMessageBox(_L("The measured point was added as an unsaved filament edit. Review and save the filament preset normally. Adaptive temperature was not enabled."),_L("Calibration result"),wxOK|wxICON_INFORMATION,this);
        } catch(const std::exception& e) {wxMessageBox(wxString::FromUTF8(e.what()),_L("Cannot add calibration point"),wxOK|wxICON_ERROR,this);}
    }
public:
    explicit AdhesionDialog(wxWindow* parent)
        : wxDialog(parent,wxID_ANY,_L("Interlayer adhesion / temperature"),wxDefaultPosition,wxDefaultSize,wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER)
    {
        auto* outer=new wxBoxSizer(wxVERTICAL);
        auto* page=new wxScrolledWindow(this,wxID_ANY,wxDefaultPosition,FromDIP(wxSize(760,650)));
        page->SetScrollRate(FromDIP(10),FromDIP(10));auto* layout=new wxBoxSizer(wxVERTICAL);
        auto* description=new wxStaticText(page,wxID_ANY,_L("Uses the current printer, filament and plate. A native spiral cylinder tests one constant flow while temperature decreases. Filament fan settings are retained. Cooling transitions use the printer's response references; stable sections use at least three turns and five seconds. These are provisional test conditions, not sensor feedback."));
        description->Wrap(FromDIP(700));layout->Add(description,0,wxEXPAND|wxALL,FromDIP(8));
        const std::array<wxString,8> labels{{_L("Wall line width (mm)"),_L("Layer height (mm)"),_L("Target volumetric flow (mm3/s)"),_L("Start temperature (C)"),_L("End temperature (C)"),_L("Temperature step (C)"),_L("Stable section height (mm; 0 = automatic)"),_L("Minimum cylinder diameter (mm; 0 = automatic)")}};
        std::array<wxString,8> values{{"0.5","0.2","5","230","200","1","0","0"}};
        try {
            const auto c=current_config();values[0]=wxString::Format("%.3f",1.25*c.opt_float("nozzle_diameter",0u));const int start=std::min(c.opt_int("nozzle_temperature",0)+10,c.opt_int("nozzle_temperature_range_high",0));
            values[3]=wxString::Format("%d",start);values[4]=wxString::Format("%d",std::max(start-30,c.opt_int("nozzle_temperature_range_low",0)));
            m_project=adhesion_plan_from_codes(wxGetApp().plater()->model().get_curr_plate_custom_gcodes());
            if(m_project) {
                const auto& a=m_project->input;
                values={{wxString::Format("%.3f",a.width),wxString::Format("%.3f",a.layer_height),wxString::Format("%.3f",a.flow),wxString::Format("%d",a.start_temperature),wxString::Format("%d",a.end_temperature),wxString::Format("%d",a.temperature_step),wxString::Format("%.3f",a.stable_height),wxString::Format("%.3f",a.minimum_diameter)}};
            }
        } catch(const std::exception&) {}
        auto make_fields=[&](wxWindow* owner,wxSizer* box,size_t first,size_t last) {
            auto* grid=new wxFlexGridSizer(2,FromDIP(6),FromDIP(14));grid->AddGrowableCol(1);
            for(size_t i=first;i<last;++i) {
                grid->Add(new wxStaticText(owner,wxID_ANY,labels[i]),0,wxALIGN_CENTER_VERTICAL);
                m_fields[i]=new wxTextCtrl(owner,wxID_ANY,values[i]);grid->Add(m_fields[i],1,wxEXPAND);
                m_fields[i]->Bind(wxEVT_TEXT,[this](wxCommandEvent&){estimate();});
            }
            box->Add(grid,0,wxEXPAND|wxALL,FromDIP(8));
        };
        make_fields(page,layout,0,6);
        m_disable_layer_time_slowdown=new wxCheckBox(page,wxID_ANY,_L("Disable layer-time slowdown for this calibration"));
        m_disable_layer_time_slowdown->SetValue(m_project && m_project->input.disable_layer_time_slowdown);
        m_disable_layer_time_slowdown->SetToolTip(_L("Allows high test flows on a smaller cylinder. Fan settings are retained, but each layer has less cooling time. Temperature transitions and stable sections still retain their required duration. This changes only the unsaved calibration filament settings."));
        layout->Add(m_disable_layer_time_slowdown,0,wxALL,FromDIP(8));
        m_disable_layer_time_slowdown->Bind(wxEVT_CHECKBOX,[this](wxCommandEvent&){estimate();});
        // Keep expansion inside the scrolled page; native collapsible panes can overlap their
        // header with child text controls when their top-level resize meets the scroll layout.
        auto* advanced=new wxToggleButton(page,wxID_ANY,_L("Advanced"));
        layout->Add(advanced,0,wxALL,FromDIP(8));
        auto* advanced_panel=new wxPanel(page);
        auto* advanced_layout=new wxBoxSizer(wxVERTICAL);
        make_fields(advanced_panel,advanced_layout,6,8);
        advanced_panel->SetSizerAndFit(advanced_layout);
        layout->Add(advanced_panel,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(8));
        advanced_panel->Hide();
        advanced->Bind(wxEVT_TOGGLEBUTTON,[page,advanced_panel,advanced](wxCommandEvent&){
            page->Freeze();
            advanced_panel->Show(advanced->GetValue());
            page->Layout();page->FitInside();page->Refresh();
            page->Thaw();
        });
        m_preview=new wxTextCtrl(page,wxID_ANY,"",wxDefaultPosition,FromDIP(wxSize(700,155)),wxTE_MULTILINE|wxTE_READONLY);
        layout->Add(m_preview,0,wxEXPAND|wxALL,FromDIP(8));
        m_create=new wxButton(page,wxID_ANY,_L("Create calibration"));layout->Add(m_create,0,wxALL,FromDIP(8));
        m_create->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){create();});
        auto* measurement_label=new wxStaticText(page,wxID_ANY,_L("After peeling, open the saved 3MF and enter the remaining cylinder height from the build plate (mm):"));measurement_label->Wrap(FromDIP(700));layout->Add(measurement_label,0,wxALL,FromDIP(8));
        m_measured=new wxTextCtrl(page,wxID_ANY,"0");layout->Add(m_measured,0,wxEXPAND|wxALL,FromDIP(8));
        auto* evaluate_button=new wxButton(page,wxID_ANY,_L("Find temperature at height"));layout->Add(evaluate_button,0,wxALL,FromDIP(8));evaluate_button->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){evaluate();});
        m_result=new wxStaticText(page,wxID_ANY,_L("The height map distinguishes cooling transitions from stable sections."));m_result->Wrap(FromDIP(700));layout->Add(m_result,0,wxEXPAND|wxALL,FromDIP(8));
        auto* actions=new wxBoxSizer(wxHORIZONTAL);m_copy=new wxButton(page,wxID_ANY,_L("Copy table point"));m_apply=new wxButton(page,wxID_ANY,_L("Add point to filament table"));actions->Add(m_copy,0,wxRIGHT,FromDIP(8));actions->Add(m_apply);layout->Add(actions,0,wxALL,FromDIP(8));
        m_copy->Enable(false);m_apply->Enable(false);
        m_measured->Bind(wxEVT_TEXT,[this](wxCommandEvent&){m_temperature.reset();m_copy->Enable(false);m_apply->Enable(false);});
        m_copy->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){if(m_temperature && wxTheClipboard->Open()){wxTheClipboard->SetData(new wxTextDataObject(wxString::Format("%.3f,%d",m_result_flow,*m_temperature)));wxTheClipboard->Close();}});
        m_apply->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){apply_result();});
        page->SetSizer(layout);outer->Add(page,1,wxEXPAND);outer->Add(CreateButtonSizer(wxCANCEL),0,wxALIGN_RIGHT|wxALL,FromDIP(10));SetSizerAndFit(outer);
        SetMinSize(FromDIP(wxSize(500,380)));const int display=wxDisplay::GetFromWindow(parent);const wxSize available=wxDisplay(display==wxNOT_FOUND ? 0 : display).GetClientArea().GetSize();SetSize(wxSize(std::min(GetSize().x,available.x-FromDIP(40)),std::min(GetSize().y,available.y-FromDIP(60))));page->FitInside();CentreOnParent();m_initialized=true;estimate();
    }
};
}
void show_adhesion_calibration(wxWindow* parent) { AdhesionDialog dialog(parent);dialog.ShowModal(); }
}} // namespace Slic3r::GUI
