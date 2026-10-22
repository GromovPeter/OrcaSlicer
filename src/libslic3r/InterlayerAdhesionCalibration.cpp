#include "InterlayerAdhesionCalibration.hpp"
#include "Config.hpp"
#include "CustomGCode.hpp"
#include "Flow.hpp"
#include "Model.hpp"
#include "Point.hpp"
#include "PrintConfig.hpp"
#include "I18N.hpp"
#include "GCode/AdaptiveTemperaturePlanner.hpp"
#include "GCode/FlowTimeline.hpp"
#include <nlohmann/json.hpp>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <exception>
#include <initializer_list>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
namespace {
constexpr double pi = 3.14159265358979323846;
constexpr const char* marker = ";_ORCA_ADHESION_CALIBRATION_V1 ";
void require(bool ok, const std::string& message) { if (!ok) throw std::invalid_argument(message); }
double number(const DynamicPrintConfig& c, const char* key) { if(const auto* v=c.option<ConfigOptionFloats>(key)) return v->get_at(0); return c.opt_float(key); }
double filament_number(const DynamicPrintConfig& c, const char* key) { return c.opt_float(key, 0); }
bool near(double a, double b) { return std::abs(a-b) < 1e-5; }
std::string conditions(const DynamicPrintConfig& c)
{
    // Include fan/load conditions, not the calibration's deliberately replaced temperature or adaptation options.
    nlohmann::json out;
    for (const char* key : {"nozzle_diameter", "filament_diameter", "filament_flow_ratio", "filament_type",
        "fan_min_speed", "fan_max_speed", "fan_cooling_layer_time", "slow_down_layer_time",
        "slow_down_for_layer_cooling", "slow_down_min_speed", "close_fan_the_first_x_layers", "full_fan_speed_layer",
        "additional_cooling_fan_speed", "chamber_temperature", "activate_chamber_temp_control",
        "adaptive_temperature_fall_time", "adaptive_temperature_lead_time"})
        if (c.has(key)) out[key] = c.option(key)->serialize();
    return out.dump();
}
// Exact clearance from the selected bed's bounding-box centre to all boundary edges.
double bed_clearance(const DynamicPrintConfig& c)
{
    const auto& points = c.option<ConfigOptionPoints>("printable_area")->values;
    require(points.size() >= 3, _u8L("The selected printer has no usable printable area."));
    double lx=points.front().x(), hx=lx, ly=points.front().y(), hy=ly;
    for (const auto& p : points) { lx=std::min(lx,p.x()); hx=std::max(hx,p.x()); ly=std::min(ly,p.y()); hy=std::max(hy,p.y()); }
    const Vec2d centre((lx+hx)/2, (ly+hy)/2);
    auto distance_to_boundary = [&](const auto& polygon, bool must_be_inside) {
        bool inside=false;
        double nearest=std::numeric_limits<double>::max();
        for (size_t i=0,j=polygon.size()-1;i<polygon.size();j=i++) {
            const Vec2d a=polygon[j], b=polygon[i], d=b-a;
            const double t=d.squaredNorm()>0 ? std::clamp((centre-a).dot(d)/d.squaredNorm(),0.0,1.0) : 0.0;
            nearest=std::min(nearest,(centre-(a+t*d)).norm());
            if ((a.y()>centre.y()) != (b.y()>centre.y()) &&
                centre.x() < (b.x()-a.x())*(centre.y()-a.y())/(b.y()-a.y())+a.x()) inside=!inside;
        }
        require(inside==must_be_inside, _u8L("A centred calibration cylinder does not fit the selected bed or its exclusion area."));
        return nearest;
    };
    double clearance=distance_to_boundary(points,true);
    if (const auto* excluded=c.option<ConfigOptionPoints>("bed_exclude_area"); excluded && excluded->values.size()>=3)
        clearance=std::min(clearance,distance_to_boundary(excluded->values,false));
    return clearance;
}
nlohmann::json plan_json(const AdhesionCalibrationPlan& p)
{
    const auto& a=p.input;
    nlohmann::json out={{"version",1},{"width",a.width},{"layer_height",a.layer_height},{"flow",a.flow},
        {"start",a.start_temperature},{"end",a.end_temperature},{"step",a.temperature_step},
        {"stable_height_override",a.stable_height},{"minimum_diameter",a.minimum_diameter},
        {"diameter",p.diameter},{"height",p.height},{"speed",p.speed},{"turn_time",p.turn_time},
        {"stable_layers",p.stable_layers},{"transition_layers",p.transition_layers},
        {"warmup_layers",p.warmup_layers},{"conditions",p.conditions}};
    // Omit the false value to retain byte-identical schedules for existing B6 projects.
    if(a.disable_layer_time_slowdown) out["disable_layer_time_slowdown"]=true;
    return out;
}
}

AdhesionCalibrationPlan plan_adhesion_calibration(const DynamicPrintConfig& c, const AdhesionCalibrationInput& a)
{
    for (double v : {a.width,a.layer_height,a.flow,a.stable_height,a.minimum_diameter})
        require(std::isfinite(v) && v>=0, _u8L("Calibration dimensions and flow must be finite and nonnegative."));
    require(c.option<ConfigOptionFloats>("nozzle_diameter")->values.size()==1 &&
        !c.opt_bool("belt_printer") && c.opt_enum<GCodeFlavor>("gcode_flavor")==gcfKlipper,
        _u8L("Interlayer adhesion calibration currently requires one Klipper hotend on a horizontal bed."));
    const double nozzle=filament_number(c,"nozzle_diameter");
    require(a.layer_height>0 && a.layer_height<=0.8*nozzle && a.width>=nozzle && a.width<=2*nozzle && a.width>a.layer_height,
        _u8L("Use a positive layer height up to 80% of the nozzle diameter and a wall width between one and two nozzle diameters."));
    require(a.start_temperature>=0 && a.start_temperature<=1000 && a.end_temperature>=0 && a.end_temperature<=1000, _u8L("Calibration temperatures must be between zero and 1000 C."));
    require(a.flow>0 && a.start_temperature>a.end_temperature && a.temperature_step>=1 &&
        (a.start_temperature-a.end_temperature)%a.temperature_step==0 && (a.start_temperature-a.end_temperature)/a.temperature_step<=150,
        _u8L("Use a positive flow, descending temperatures and an exact whole number of temperature steps (at most 150)."));
    require(a.end_temperature>=c.opt_int("nozzle_temperature_range_low",0) && a.start_temperature<=c.opt_int("nozzle_temperature_range_high",0),
        _u8L("Calibration temperatures must stay within the selected filament's working limits."));
    require(c.option<ConfigOptionStrings>("post_process")->values.empty(), _u8L("External G-code postprocessing is not supported for this calibration."));
    for(const char* key : {"layer_change_gcode","before_layer_change_gcode"}) {
        std::istringstream lines(c.opt_string(key));std::string line;
        while(std::getline(lines,line)) {
            line=line.substr(0,line.find(';'));std::istringstream words(line);std::string command;words>>command;
            bool allowed=command.empty() || command=="SET_PRINT_STATS_INFO";
            if(command=="G92") {
                std::string argument,extra;
                words>>argument>>extra;
                allowed=argument.size()>1 && argument.front()=='E' && extra.empty();
                if(allowed) {
                    try { size_t used=0;const double value=std::stod(argument.substr(1),&used);allowed=std::isfinite(value) && used==argument.size()-1; }
                    catch(const std::exception&) { allowed=false; }
                }
            }
            require(allowed,
                _u8L("This calibration requires layer-change G-code containing only comments, extrusion-coordinate resets or print statistics."));
        }
    }
    const double ratio=filament_number(c,"filament_flow_ratio");
    const double cap=filament_number(c,"filament_max_volumetric_speed");
    require(ratio>0 && std::isfinite(ratio) && (cap<=0 || a.flow<=cap),
        _u8L("The requested flow exceeds the filament flow limit or its flow ratio is invalid."));
    const double area=Flow(float(a.width),float(a.layer_height),float(nozzle)).mm3_per_mm()*ratio;
    AdhesionCalibrationPlan p; p.input=a; p.speed=a.flow/area;
    DynamicPrintConfig effective=c;
    if(a.disable_layer_time_slowdown) effective.set_key_value("slow_down_for_layer_cooling",new ConfigOptionBools{false});
    p.conditions=conditions(effective);
    const double fall=number(c,"adaptive_temperature_fall_time"), lead=number(c,"adaptive_temperature_lead_time");
    require(fall>0 && lead>=0 && std::isfinite(fall) && std::isfinite(lead), _u8L("Valid printer cooling-response and lead-time references are required."));
    double acceleration=number(c,"outer_wall_acceleration");
    if (acceleration<=0) acceleration=number(c,"default_acceleration");
    for (const char* key : {"machine_max_acceleration_x","machine_max_acceleration_y"}) {
        const double value=filament_number(c,key); if(value>0) acceleration=acceleration>0 ? std::min(acceleration,value) : value;
    }
    require(acceleration>0, _u8L("The printer acceleration limit is unavailable."));
    for (const char* key : {"machine_max_speed_x","machine_max_speed_y"}) {
        const double value=filament_number(c,key);
        require(value<=0 || p.speed<=value, _u8L("The requested calibration flow requires a speed above the printer limit."));
    }
    double minimum_turn=2.0;
    if(!a.disable_layer_time_slowdown && c.opt_bool("slow_down_for_layer_cooling",0)) minimum_turn=std::max(minimum_turn,1.1*filament_number(c,"slow_down_layer_time"));
    const double centre_diameter=std::max({40.0, a.minimum_diameter>0 ? a.minimum_diameter-a.width : 0.0,
        minimum_turn*p.speed/pi, 4.0*p.speed*p.speed/acceleration});
    p.diameter=std::ceil((centre_diameter+a.width)*10.0)/10.0;
    require(p.diameter/2+5.0<=bed_clearance(c),
        _u8L("The cylinder needed to maintain this flow without layer-cooling slowdown does not fit the bed. Reduce the test flow."));
    p.turn_time=pi*(p.diameter-a.width)/p.speed;
    p.stable_layers=static_cast<unsigned>(std::max(3.0,std::ceil(5.0/p.turn_time)));
    const double automatic_stable=p.stable_layers*a.layer_height;
    if(a.stable_height>0) {
        require(a.stable_height+1e-6>=automatic_stable, _u8L("Stable section height cannot be smaller than the automatic three-turn, five-second minimum."));
        p.stable_layers=static_cast<unsigned>(std::ceil(a.stable_height/a.layer_height-1e-8));
    }
    p.stable_height=p.stable_layers*a.layer_height;
    p.transition_layers=static_cast<unsigned>(std::max(1.0,std::ceil((1.25*fall*a.temperature_step+lead)/p.turn_time)));
    p.transition_height=p.transition_layers*a.layer_height;
    const int fan_layers=std::max(c.opt_int("close_fan_the_first_x_layers",0),c.opt_int("full_fan_speed_layer",0));
    p.warmup_layers=static_cast<unsigned>(std::max(5,fan_layers+2));
    p.measurement_z=p.warmup_layers*a.layer_height;
    double z=p.measurement_z;
    for(int temp=a.start_temperature;temp>=a.end_temperature;temp-=a.temperature_step) {
        const double transition=temp==a.start_temperature ? 0 : p.transition_height;
        p.bands.push_back({temp,z,z+transition,z+transition+p.stable_height});z=p.bands.back().end_z;
    }
    p.total_layers=p.warmup_layers+p.stable_layers*static_cast<unsigned>(p.bands.size())+
        p.transition_layers*static_cast<unsigned>(p.bands.size()-1);
    p.height=p.total_layers*a.layer_height;
    require(p.height+1.0<=number(c,"printable_height"),
        _u8L("The calibration cylinder is too tall. Narrow the temperature range or split it into separate tests; dwell is not shortened automatically."));
    p.estimated_wall_time=p.total_layers*p.turn_time;
    p.estimated_wall_volume=pi*(p.diameter-a.width)*p.total_layers*area;
    return p;
}

DynamicPrintConfig adhesion_process_patch(const AdhesionCalibrationPlan& p)
{
    DynamicPrintConfig out;
    out.set_deserialize_strict({{"msao_recovery_enable","0"},{"spiral_mode","1"},{"wall_loops","1"},{"top_shell_layers","0"},
        {"bottom_shell_layers","0"},{"bottom_shell_thickness","0"},{"sparse_infill_density","0%"},
        {"enable_support","0"},{"enforce_support_layers","0"},{"raft_layers","0"},
        {"fuzzy_skin","disabled_fuzzy"},{"ironing_type","no ironing"},
        {"brim_type","outer_only"},{"brim_width","3"},{"brim_object_gap","0"},{"skirt_loops","0"},
        {"wall_generator","classic"},{"seam_slope_type","none"},{"alternate_extra_wall","0"},
        {"precise_z_height","0"},{"overhang_reverse","0"},{"enable_arc_fitting","0"}});
    out.set_key_value("layer_height",new ConfigOptionFloat(p.input.layer_height));
    out.set_key_value("initial_layer_print_height",new ConfigOptionFloat(p.input.layer_height));
    for(const char* key : {"line_width","outer_wall_line_width","inner_wall_line_width","initial_layer_line_width"})
        out.set_key_value(key,new ConfigOptionFloatOrPercent(p.input.width,false));
    out.set_key_value("outer_wall_speed",new ConfigOptionFloats{p.speed});
    out.set_key_value("inner_wall_speed",new ConfigOptionFloats{p.speed});
    return out;
}
DynamicPrintConfig adhesion_filament_patch(const AdhesionCalibrationPlan& p)
{
    DynamicPrintConfig out;
    if(p.input.disable_layer_time_slowdown)
        out.set_key_value("slow_down_for_layer_cooling",new ConfigOptionBools{false});
    out.set_key_value("nozzle_temperature_initial_layer",new ConfigOptionInts{p.input.start_temperature});
    out.set_key_value("nozzle_temperature",new ConfigOptionInts{p.input.start_temperature});
    for(const char* key : {"filament_adaptive_temperature","filament_coordinated_temperature","filament_adaptive_volumetric_speed"})
        out.set_key_value(key,new ConfigOptionBools{false});
    return out;
}
CustomGCode::Info adhesion_custom_gcodes(const AdhesionCalibrationPlan& p)
{
    CustomGCode::Info out;out.mode=CustomGCode::SingleExtruder;
    for(size_t i=0;i<p.bands.size();++i) {
        std::string text;
        if(i==0) text=std::string(marker)+plan_json(p).dump()+"\n";
        text+="M104 S"+std::to_string(p.bands[i].temperature)+" ; _ORCA_ADHESION_STEP\n";
        // Native custom code is emitted before this layer's spiral starts at the previous Z.
        out.gcodes.push_back({p.bands[i].begin_z+p.input.layer_height,CustomGCode::Custom,1,"",std::move(text)});
    }
    return out;
}
bool adhesion_has_height_map(const CustomGCode::Info& codes)
{
    return std::any_of(codes.gcodes.begin(),codes.gcodes.end(),[](const auto& item){return item.extra.find(marker)!=std::string::npos;});
}
std::optional<AdhesionCalibrationPlan> adhesion_plan_from_codes(const CustomGCode::Info& codes)
{
    std::optional<AdhesionCalibrationPlan> result;
    for(const auto& item : codes.gcodes) {
        const size_t at=item.extra.find(marker);if(at==std::string::npos) continue;
        require(!result, _u8L("The calibration contains duplicate height maps."));
        const size_t begin=at+std::char_traits<char>::length(marker), end=item.extra.find('\n',begin);
        const auto j=nlohmann::json::parse(item.extra.substr(begin,end-begin));
        require(j.at("version")==1, _u8L("Unsupported interlayer calibration height map."));
        AdhesionCalibrationPlan p;auto& a=p.input;
        a.width=j.at("width");a.layer_height=j.at("layer_height");a.flow=j.at("flow");a.start_temperature=j.at("start");a.end_temperature=j.at("end");a.temperature_step=j.at("step");a.stable_height=j.at("stable_height_override");a.minimum_diameter=j.at("minimum_diameter");
        a.disable_layer_time_slowdown=j.value("disable_layer_time_slowdown",false);
        p.diameter=j.at("diameter");p.height=j.at("height");p.speed=j.at("speed");p.turn_time=j.at("turn_time");p.stable_layers=j.at("stable_layers");p.transition_layers=j.at("transition_layers");p.warmup_layers=j.at("warmup_layers");p.conditions=j.at("conditions");
        require(std::isfinite(p.height) && p.height>0 && a.layer_height>0 && a.temperature_step>0 && a.start_temperature>a.end_temperature && a.start_temperature-a.end_temperature<=1500 && p.stable_layers>=3 && p.transition_layers>=1 && p.warmup_layers>=3,
            _u8L("Invalid calibration height map."));
        p.stable_height=p.stable_layers*a.layer_height;p.transition_height=p.transition_layers*a.layer_height;p.measurement_z=p.warmup_layers*a.layer_height;
        double z=p.measurement_z;
        for(int t=a.start_temperature;t>=a.end_temperature;t-=a.temperature_step) {
            const double stable=z+(t==a.start_temperature ? 0 : p.transition_height);
            p.bands.push_back({t,z,stable,stable+p.stable_height});z=p.bands.back().end_z;
        }
        p.total_layers=static_cast<unsigned>(std::lround(p.height/a.layer_height));
        require(near(z,p.height), _u8L("The calibration height map is inconsistent."));result=std::move(p);
    }
    return result;
}
AdhesionHeightResult adhesion_temperature_at_height(const AdhesionCalibrationPlan& p, double height)
{
    if(!std::isfinite(height) || height<0 || height>p.height+1e-6) return {};
    if(height<p.measurement_z) return {AdhesionHeightResult::Kind::Warmup,p.input.start_temperature,p.input.start_temperature};
    for(size_t i=0;i<p.bands.size();++i) {
        const auto& b=p.bands[i];
        if(height<b.end_z || (i+1==p.bands.size() && height<=b.end_z+1e-6))
            return {height<b.stable_z ? AdhesionHeightResult::Kind::Transition : AdhesionHeightResult::Kind::Stable,
                    b.temperature,i>0 ? p.bands[i-1].temperature : b.temperature};
    }
    return {};
}
void validate_adhesion_project(const Model& model, const DynamicPrintConfig& c, const AdhesionCalibrationPlan& p)
{
    const auto expected=plan_adhesion_calibration(c,p.input);
    require(model.objects.size()==1 && model.objects.front()->instances.size()==1,
        _u8L("Keep exactly one cylinder and one instance in an interlayer calibration project."));
    require(model.objects.front()->volumes.size()==1 && model.objects.front()->layer_height_profile.empty() && model.objects.front()->layer_config_ranges.empty(),
        _u8L("Keep the calibration cylinder without modifiers or variable layer heights."));
    const auto box=model.objects.front()->bounding_box_exact();
    require(std::abs(box.size().z()-p.height)<0.02 && std::abs(box.size().x()-p.diameter)<0.05 && std::abs(box.size().y()-p.diameter)<0.05 &&
        near(expected.diameter,p.diameter) && near(expected.height,p.height) && expected.conditions==p.conditions,
        _u8L("The calibration geometry or printing conditions changed. Recreate it to obtain a valid height map."));
    const auto process=adhesion_process_patch(p), filament=adhesion_filament_patch(p);
    for(const auto* patch : {&process,&filament})
        for(const auto& key : patch->keys())
            require(c.option(key) && c.option(key)->serialize()==patch->option(key)->serialize(),
                _u8L("A required calibration setting changed. Recreate the cylinder before slicing."));
    const auto codes=adhesion_custom_gcodes(p);
    const auto& actual=model.get_curr_plate_custom_gcodes();
    // The preview slider round-trips layer heights through floats and may omit its display mode.
    // Neither changes a one-hotend Custom command; command contents and layer placement remain checked.
    bool matches=(actual.mode==CustomGCode::SingleExtruder || actual.mode==CustomGCode::Undef) && actual.gcodes.size()==codes.gcodes.size();
    if(matches) for(size_t i=0;i<codes.gcodes.size();++i) {
        const auto& a=actual.gcodes[i];const auto& b=codes.gcodes[i];
        matches=matches && std::abs(a.print_z-b.print_z)<0.0001 && a.type==b.type &&
            a.extruder==b.extruder && a.color==b.color && a.extra==b.extra;
    }
    require(matches, _u8L("The calibration temperature schedule changed. Recreate the cylinder."));
}
void validate_adhesion_flow(const FlowTimeline& timeline, const AdhesionCalibrationPlan& p)
{
    double volume=0,time=0,qualified_volume=0;unsigned last_layer=0;
    for(const auto& interval : timeline.intervals()) {
        const auto& m=interval.motion;
        const double v=interval.deposited_volume(0,m.duration);
        if(v<=1e-8) continue;
        last_layer=std::max(last_layer,m.layer_id);
        if(m.layer_id<=p.warmup_layers || m.layer_id>p.total_layers) continue;
        require(interval.recovery_distance<1e-5, _u8L("Retraction recovery interrupts the calibration measurement wall."));
        volume+=v;time+=m.duration;
        if(m.duration>0 && std::abs(v/m.duration-p.input.flow)<=0.05*p.input.flow) qualified_volume+=v;
    }
    if (!(last_layer==p.total_layers && time>0 && std::abs(volume/time-p.input.flow)<=0.03*p.input.flow && qualified_volume>=0.95*volume))
    BOOST_LOG_TRIVIAL(warning) << "Adhesion flow validation: expected=" << p.input.flow << " actual=" << (time>0 ? volume/time : 0)
        << " qualified_volume=" << (volume>0 ? qualified_volume/volume : 0) << " layers=" << last_layer << "/" << p.total_layers;
    require(last_layer==p.total_layers && time>0 && std::abs(volume/time-p.input.flow)<=0.03*p.input.flow && qualified_volume>=0.95*volume,
        _u8L("The sliced cylinder does not maintain the requested flow. Cooling or motion limits invalidate this calibration; use a lower flow or recreate it."));
}
std::string adhesion_table_with_point(const std::string& table, double flow, double temperature)
{
    auto points=parse_flow_temperature_table(table);
    points.erase(std::remove_if(points.begin(),points.end(),[&](const auto& p){return near(p.flow,flow);}),points.end());
    points.push_back({flow,temperature});std::sort(points.begin(),points.end(),[](const auto& a,const auto& b){return a.flow<b.flow;});
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(8);
    for(size_t i=0;i<points.size();++i) {
        require(i==0 || points[i].temperature>points[i-1].temperature, _u8L("This measurement conflicts with the current increasing temperature table. Review the table in filament settings; the result remains available to copy."));
        if(i) out<<'\n';out<<points[i].flow<<','<<points[i].temperature;
    }
    return out.str();
}
} // namespace Slic3r
