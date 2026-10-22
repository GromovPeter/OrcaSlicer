#pragma once
#include <string>
class wxWindow;
namespace Slic3r { namespace GUI {
void show_adaptive_temperature_report(wxWindow* parent, const std::string& filename);
}}
