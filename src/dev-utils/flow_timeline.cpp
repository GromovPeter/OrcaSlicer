// libslic3r references nanosvg; each executable supplies its header-only implementation.
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/FlowTimeline.hpp"
#include "libslic3r/Utils.hpp"

#include <cstdlib>
#include <cstddef>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::cerr << "Usage: flow_timeline input.gcode output.csv resources-directory\n";
        return EXIT_FAILURE;
    }
    try {
        Slic3r::set_resources_dir(argv[3]);
        Slic3r::GCodeProcessor baseline;
        baseline.process_file(argv[1]);
        Slic3r::GCodeProcessor processor;
        processor.enable_flow_timeline();
        processor.process_file(argv[1]);
        const auto* timeline = processor.flow_timeline();
        if (timeline == nullptr)
            throw std::runtime_error("Flow timeline was not collected");
        const auto mode = Slic3r::PrintEstimatedStatistics::ETimeMode::Normal;
        if (processor.get_time(mode) != baseline.get_time(mode))
            throw std::runtime_error("Collection changed the time estimate");
        const auto& original = baseline.get_result().moves;
        const auto& captured = processor.get_result().moves;
        if (original.size() != captured.size())
            throw std::runtime_error("Collection changed preview vertices");
        for (size_t i = 0; i < original.size(); ++i)
            if (original[i].gcode_id != captured[i].gcode_id || original[i].type != captured[i].type ||
                original[i].position != captured[i].position || original[i].feedrate != captured[i].feedrate ||
                original[i].time != captured[i].time)
                throw std::runtime_error("Collection changed a preview move");
        std::ofstream out(argv[2]);
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out << std::setprecision(17);
        out << "start_s,duration_s,source_line,remaining_internal_moves,kind,role,filament_id,layer_id,prepare_stage,protected_region,distance_mm,entry_mm_s,cruise_mm_s,exit_mm_s,acceleration_s,deceleration_s,recovery_mm,deposited_mm3,average_flow_mm3_s\n";
        for (const auto& interval : timeline->intervals()) {
            const auto& m = interval.motion;
            const double volume = interval.deposited_volume(0.0, m.duration);
            out << m.start_time << ',' << m.duration << ',' << m.source_line << ',' << m.remaining_internal_moves
                << ',' << static_cast<int>(m.kind) << ',' << static_cast<int>(m.role) << ',' << m.filament_id
                << ',' << m.layer_id << ',' << m.prepare_stage << ',' << m.protected_region << ',' << m.distance
                << ',' << m.entry_speed << ',' << m.cruise_speed << ',' << m.exit_speed
                << ',' << interval.acceleration_time << ',' << interval.deceleration_time << ',' << interval.recovery_distance
                << ',' << volume << ',' << (m.duration > 0.0 ? volume / m.duration : 0.0) << '\n';
        }
        out.close();
        std::cout << std::setprecision(17) << "{\"estimated_time_s\":" << processor.get_time(mode)
                  << ",\"timeline_time_s\":" << timeline->total_time()
                  << ",\"deposited_mm3\":" << timeline->deposited_volume()
                  << ",\"intervals\":" << timeline->intervals().size()
                  << ",\"preview_moves\":" << captured.size()
                  << ",\"collection_preserves_motion\":true}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
